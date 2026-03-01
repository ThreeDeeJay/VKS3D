# VKS3D — Vulkan 1.1 Stereoscopic 3D ICD

A **Vulkan 1.1 Installable Client Driver (ICD)** that transparently injects
stereoscopic Side-By-Side (SBS) 3D rendering into any Vulkan application
without requiring source changes.

```
App → Vulkan Loader → VKS3D ICD → Real GPU ICD (Intel / AMD / NVIDIA / LLVMpipe)
```

## Features

| Feature | Detail |
|---|---|
| **VK_KHR_multiview** | Injected into every render pass (viewMask = 0b11) |
| **SPIR-V patching** | Vertex shaders patched at load time for per-eye clip-space offset |
| **Side-By-Side output** | Swapchain width doubled; left eye left half, right eye right half |
| **Configurable IPD** | `STEREO_SEPARATION` env var (default 65 mm) |
| **Configurable convergence** | `STEREO_CONVERGENCE` for frustum shift (default 30 mm) |
| **Zero app changes** | Works as a Vulkan ICD layer below the application |
| **Passthrough mode** | `STEREO_ENABLED=0` for zero-overhead bypass |

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Vulkan Application                                             │
│  (unchanged — renders W×H as usual)                             │
└───────────────────┬─────────────────────────────────────────────┘
                    │  vkCreateInstance / vkCreateDevice / etc.
┌───────────────────▼─────────────────────────────────────────────┐
│  Vulkan Loader  (reads VK_ICD_FILENAMES)                        │
└───────────────────┬─────────────────────────────────────────────┘
                    │
┌───────────────────▼─────────────────────────────────────────────┐
│  VKS3D ICD  (libstereo_vk_icd.so)                               │
│                                                                 │
│  ① CreateInstance   — injects VK_KHR_get_physical_device_props2 │
│  ② CreateDevice     — enables VkPhysicalDeviceMultiviewFeatures │
│                       + VK_KHR_multiview extension              │
│  ③ CreateRenderPass — injects VkRenderPassMultiviewCreateInfo   │
│                       viewMask = 0b11  (both eyes per subpass)  │
│  ④ CreateShaderModule — patches vertex SPIR-V:                  │
│       gl_Position.x += sign(eye) * separation * gl_Position.w  │
│       sign = -1 for view 0 (left), +1 for view 1 (right)       │
│  ⑤ CreateSwapchainKHR — doubles imageExtent.width (SBS)         │
│  ⑥ GetSwapchainImagesKHR — returns 2-layer stereo images to app │
│  ⑦ QueuePresentKHR  — runs composite blit:                      │
│       layer 0 → left  half  [0,   W)  of real swapchain image   │
│       layer 1 → right half  [W, 2W)  of real swapchain image   │
└───────────────────┬─────────────────────────────────────────────┘
                    │  real Vulkan calls
┌───────────────────▼─────────────────────────────────────────────┐
│  Real GPU ICD  (libvulkan_intel.so / libvulkan_radeon.so / etc) │
└─────────────────────────────────────────────────────────────────┘
```

### Stereo Math — Off-Axis Asymmetric Frustum

Per-eye clip-space x offset:

```
left_offset  = -(separation/2) + (convergence/2)
right_offset = +(separation/2) - (convergence/2)
```

The SPIR-V-patched vertex shader applies:

```glsl
// Injected after the last write to gl_Position:
float sign   = (gl_ViewIndex == 0) ? -1.0 : 1.0;
gl_Position.x += sign * eye_offset * gl_Position.w;
```

Multiplying by `gl_Position.w` gives the correct perspective-correct
off-axis shift: objects at the convergence plane appear fused, objects
nearer/farther show controlled disparity.

### Render Pass Multiview Injection

`VkRenderPassMultiviewCreateInfo` is prepended to the `pNext` chain of
every `vkCreateRenderPass` call:

```c
.subpassCount   = N          // all subpasses broadcast to both eyes
.pViewMasks     = {0x3, …}   // bit 0 = left eye, bit 1 = right eye
.correlationMasks = {0x3, …} // GPU renders both eyes in same pass
```

This broadcasts every draw call to both image array layers simultaneously,
with `gl_ViewIndex` identifying the current eye in the vertex shader.

### SBS Composite Pass

At `vkQueuePresentKHR`, before the real present:

```
stereo_images[i]  (W×H, arrayLayers=2)
    │
    ├─ layer 0  ──[CmdBlitImage]──►  sbs_images[i][0 .. W)
    └─ layer 1  ──[CmdBlitImage]──►  sbs_images[i][W .. 2W)
```

The composite uses `VkCmdBlitImage` directly — no pipeline, no shader,
no descriptor sets. The `dstOffsets` of the right-eye blit are shifted
by `W` in x.

---

## Building

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt install cmake build-essential libvulkan-dev vulkan-tools \
                 glslang-tools spirv-tools

# Fedora / RHEL
sudo dnf install cmake gcc libvulkan-devel vulkan-tools glslang
```

### Build

```bash
git clone https://github.com/ThreeDeeJay/VKS3D.git
cd VKS3D
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j$(nproc)
```

This produces:
- `libstereo_vk_icd.so`      — the ICD shared library
- `stereo_demo`               — headless integration demo
- `test_spirv_patch`          — SPIR-V patcher unit tests
- `test_stereo_math`          — stereo math unit tests

### Run tests

```bash
cd build
ctest --output-on-failure
```

---

## Usage

### 1. Point the Vulkan loader at VKS3D

```bash
export VK_ICD_FILENAMES=/path/to/VKS3D/manifest/stereo_vk_icd.json
```

### 2. Tell VKS3D which real GPU ICD to use

```bash
# Intel
export STEREO_REAL_ICD=/usr/lib/x86_64-linux-gnu/libvulkan_intel.so

# AMD
export STEREO_REAL_ICD=/usr/lib/x86_64-linux-gnu/libvulkan_radeon.so

# Software (LLVMpipe / Lavapipe)
export STEREO_REAL_ICD=/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so

# NVIDIA (proprietary)
export STEREO_REAL_ICD=/usr/lib/x86_64-linux-gnu/nvidia/current/libGLX_nvidia.so.0
```

If `STEREO_REAL_ICD` is not set, VKS3D tries common paths automatically.

### 3. Run any Vulkan application

```bash
STEREO_SEPARATION=0.065 STEREO_CONVERGENCE=0.030 vkcube
```

---

## Configuration

All configuration is via environment variables — no recompilation needed.

| Variable | Default | Description |
|---|---|---|
| `STEREO_ENABLED` | `1` | Set to `0` to disable stereo (pure passthrough) |
| `STEREO_SEPARATION` | `0.065` | Inter-pupillary distance in clip-space units (~65 mm) |
| `STEREO_CONVERGENCE` | `0.030` | Convergence frustum shift — reduces near-plane diplopia |
| `STEREO_FLIP_EYES` | `0` | Set to `1` to swap left/right (for cross-eyed displays) |
| `STEREO_REAL_ICD` | *(auto)* | Path to the real GPU ICD `.so` |

### Tuning guide

| Scene | Recommended settings |
|---|---|
| Fast-action / racing | `SEPARATION=0.050 CONVERGENCE=0.015` |
| Cinematic / exploration | `SEPARATION=0.065 CONVERGENCE=0.030` |
| Very deep scenes | `SEPARATION=0.070 CONVERGENCE=0.035` |
| Cross-eyed SBS | `STEREO_FLIP_EYES=1` |
| Passive (anaglyph display) | Combine with post-process anaglyph filter |

---

## File Layout

```
VKS3D/
├── CMakeLists.txt
├── stereo_icd.map.in          ← linker version script (exports 3 symbols only)
├── include/
│   └── stereo_icd.h           ← all types, structs, dispatch tables
├── src/
│   ├── icd_main.c             ← vk_icdNegotiateLoaderICDInterfaceVersion
│   │                             vk_icdGetInstanceProcAddr
│   │                             vk_icdGetPhysicalDeviceProcAddr
│   ├── stereo.c               ← config, global object registry, dispatch init
│   ├── instance.c             ← vkCreateInstance / DestroyInstance / Enumerate
│   ├── device.c               ← vkCreateDevice / DestroyDevice / stereo UBO
│   ├── render_pass.c          ← multiview injection into every render pass
│   ├── shader.c               ← SPIR-V binary patcher for vertex eye offset
│   ├── swapchain.c            ← SBS swapchain (2× width) + stereo image alloc
│   └── present.c             ← SBS composite blit at QueuePresentKHR
├── shaders/
│   ├── composite.vert.glsl    ← full-screen triangle vertex shader
│   └── composite.frag.glsl    ← SBS sample-from-layer fragment shader
├── manifest/
│   └── stereo_vk_icd.json     ← Vulkan loader ICD manifest
└── tests/
    ├── CMakeLists.txt
    ├── demo.c                 ← headless integration demo
    ├── test_spirv_patch.c     ← SPIR-V patcher unit tests
    └── test_stereo_math.c     ← stereo math unit tests
```

---

## ICD Interface

VKS3D implements **Loader/ICD Interface Version 5**:

| Symbol exported | Purpose |
|---|---|
| `vk_icdNegotiateLoaderICDInterfaceVersion` | ABI version handshake |
| `vk_icdGetInstanceProcAddr` | All function pointer lookups |
| `vk_icdGetPhysicalDeviceProcAddr` | Physical device function lookups |

All other symbols are hidden via the linker version script.

---

## Limitations & Known Issues

- **Push constant collision**: The SPIR-V patcher currently reads the stereo
  offset from constants baked at shader-load time, not from a runtime UBO.
  Applications using large push constant ranges may see incorrect offsets
  if the patcher's offset (124 bytes) collides with app data. A future
  version will use a dedicated descriptor set.

- **Geometry / tessellation shaders**: Multiview broadcast works with
  geometry and tessellation shaders if `multiviewGeometryShader` /
  `multiviewTessellationShader` features are present. VKS3D requests these
  as `VK_FALSE` by default; enable them manually for complex pipelines.

- **Composite synchronisation**: The current composite pass submits a
  command buffer and waits synchronously. A semaphore-based pipeline is
  planned to remove the CPU stall at present time.

- **Dynamic rendering** (`VK_KHR_dynamic_rendering`): Not yet intercepted.
  Applications using dynamic rendering will need multiview set at pipeline
  creation time.

- **Ray tracing** (`VK_KHR_ray_tracing_pipeline`): Out of scope; stereo
  for ray tracing requires per-eye ray generation which must be handled
  by the application.

---

## License

MIT — see LICENSE file.

# VKS3D — Vulkan Stereoscopic 3D ICD

A **Vulkan 1.1 Installable Client Driver (ICD)** that transparently injects
Side-By-Side (SBS) stereoscopic 3D rendering into **any Vulkan application**
with no source changes required.

| Platform | 64-bit | 32-bit |
|---|---|---|
| **Windows** | `VKS3D_x64.dll` | `VKS3D_x86.dll` |
| Linux | `VKS3D_x64.so` | `VKS3D_x86.so` |

```
App → Vulkan Loader → VKS3D_x64.dll → Real GPU ICD (nvoglv64.dll / amdvlk64.dll …)
```

## How it works

| Step | What VKS3D does |
|---|---|
| `vkCreateInstance` | Loads real GPU ICD from registry, injects `VK_KHR_get_physical_device_properties2` |
| `vkCreateDevice` | Enables `VkPhysicalDeviceMultiviewFeatures.multiview`, adds `VK_KHR_multiview` |
| `vkCreateRenderPass` | Prepends `VkRenderPassMultiviewCreateInfo` — `viewMask=0b11` for both eyes |
| `vkCreateShaderModule` | Patches vertex SPIR-V binary to apply per-eye clip-space offset |
| `vkCreateSwapchainKHR` | Doubles `imageExtent.width` — app sees `W×H`, real swapchain is `2W×H` |
| `vkGetSwapchainImagesKHR` | Returns 2-layer stereo images to the app (not the real SBS images) |
| `vkQueuePresentKHR` | Runs `vkCmdBlitImage` composite: layer 0 → left half, layer 1 → right half |

### Stereo math — off-axis asymmetric frustum

```
left_offset  = -(separation/2) + (convergence/2)
right_offset = +(separation/2) - (convergence/2)
```

Injected into every vertex shader after the last write to `gl_Position`:

```glsl
// Injected by VKS3D SPIR-V patcher at shader load time:
gl_Position.x += (gl_ViewIndex == 0 ? left_offset : right_offset)
               * gl_Position.w;   // perspective-correct parallax
```

---

## Installation (Windows)

### Prerequisites

- [Vulkan SDK](https://vulkan.lunarg.com/sdk/home#windows) (for headers/loader — already installed with most GPU drivers)
- A Vulkan-capable GPU with a working driver (NVIDIA, AMD, Intel)

### Option A — Pre-built release (recommended)

1. Download `VKS3D-vX.Y.Z-windows.zip` from [Releases](../../releases)
2. Extract to a permanent location, e.g. `C:\VKS3D\`
3. Run `install.bat` **as Administrator** (or `Install-VKS3D.ps1` in an elevated PowerShell)
4. Done — VKS3D is now registered for all Vulkan applications

```
C:\VKS3D\
├── VKS3D_x64.dll       ← 64-bit ICD  (registered for 64-bit apps)
├── VKS3D_x86.dll       ← 32-bit ICD  (registered for 32-bit apps)
├── VKS3D_x64.json      ← Vulkan manifest (64-bit)
├── VKS3D_x86.json      ← Vulkan manifest (32-bit)
├── install.bat
├── uninstall.bat
└── Install-VKS3D.ps1
```

### Option B — Manual registry registration

The Vulkan loader discovers ICDs from:

| Architecture | Registry key |
|---|---|
| 64-bit (`VKS3D_x64.dll`) | `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` |
| 32-bit (`VKS3D_x86.dll`) | `HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers` |

Each value is: **name** = full path to the JSON manifest, **data** = `DWORD 0`.

```cmd
:: 64-bit (run as Administrator)
reg add "HKLM\SOFTWARE\Khronos\Vulkan\Drivers" ^
    /v "C:\VKS3D\VKS3D_x64.json" /t REG_DWORD /d 0 /f

:: 32-bit (run as Administrator)
reg add "HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers" ^
    /v "C:\VKS3D\VKS3D_x86.json" /t REG_DWORD /d 0 /f
```

### Uninstall

```cmd
C:\VKS3D\uninstall.bat
```

or

```powershell
C:\VKS3D\Install-VKS3D.ps1 -Uninstall
```

---

## Configuration

All settings are environment variables — no reboot required, changes take effect on the next app launch.

| Variable | Default | Description |
|---|---|---|
| `STEREO_ENABLED` | `1` | Set to `0` to disable (pure passthrough, zero overhead) |
| `STEREO_SEPARATION` | `0.065` | Inter-pupillary distance in clip-space units (~65 mm) |
| `STEREO_CONVERGENCE` | `0.030` | Convergence frustum shift — reduces near-plane diplopia |
| `STEREO_FLIP_EYES` | `0` | Set to `1` to swap left/right (cross-eyed / mirrored displays) |
| `STEREO_REAL_ICD` | *(auto)* | Override — direct path to real GPU ICD DLL |

Set permanently in System Properties → Environment Variables, or per-session:

```cmd
set STEREO_SEPARATION=0.065
set STEREO_CONVERGENCE=0.030
YourVulkanGame.exe
```

```powershell
$env:STEREO_SEPARATION  = "0.065"
$env:STEREO_CONVERGENCE = "0.030"
.\YourVulkanGame.exe
```

### Tuning guide

| Scene type | Recommended settings |
|---|---|
| Racing / fast action | `SEPARATION=0.050  CONVERGENCE=0.015` |
| General exploration | `SEPARATION=0.065  CONVERGENCE=0.030` |
| Deep vistas | `SEPARATION=0.070  CONVERGENCE=0.035` |
| Cross-eyed SBS viewer | `STEREO_FLIP_EYES=1` |

### Override real ICD (advanced)

By default VKS3D auto-detects the real GPU ICD from the registry, skipping
itself. To force a specific ICD:

```cmd
set STEREO_REAL_ICD=C:\Windows\System32\nvoglv64.dll   :: NVIDIA 64-bit
set STEREO_REAL_ICD=C:\Windows\SysWOW64\nvoglv32.dll   :: NVIDIA 32-bit
set STEREO_REAL_ICD=C:\Windows\System32\amdvlk64.dll   :: AMD 64-bit
```

---

## Building from source

### Requirements

- [Visual Studio 2022](https://visualstudio.microsoft.com/) with **Desktop C++** workload
  (includes the MSVC compiler and x64/x86 toolchains)
- [CMake 3.20+](https://cmake.org/download/)
- [Vulkan SDK 1.3+](https://vulkan.lunarg.com/sdk/home#windows)

### Build both 64-bit and 32-bit

Visual Studio / MSVC cannot target two architectures in a single CMake
configuration. Run the configure + build steps twice:

```powershell
# ── 64-bit ──────────────────────────────────────────────────────────────
cmake -B build_x64 -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build_x64 --config Release --parallel
# Output: build_x64\Release\VKS3D_x64.dll

# ── 32-bit ──────────────────────────────────────────────────────────────
cmake -B build_x86 -A Win32 -DCMAKE_BUILD_TYPE=Release
cmake --build build_x86 --config Release --parallel
# Output: build_x86\Release\VKS3D_x86.dll
```

Or from a **Developer Command Prompt for VS 2022**:

```cmd
:: x64
mkdir build_x64 && cd build_x64
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
cd ..

:: x86
mkdir build_x86 && cd build_x86
cmake .. -G "Visual Studio 17 2022" -A Win32
cmake --build . --config Release
cd ..
```

### Build with Ninja (faster, single-config)

```cmd
:: Open "x64 Native Tools Command Prompt for VS 2022"
cmake -B build_x64 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_x64

:: Open "x86 Native Tools Command Prompt for VS 2022"
cmake -B build_x86 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_x86
```

### Run tests

```powershell
cd build_x64
ctest -C Release --output-on-failure
```

### Install to a directory for packaging

```powershell
cmake --install build_x64 --prefix dist\x64
cmake --install build_x86 --prefix dist\x86
```

---

## File layout

```
VKS3D/
├── .github/workflows/build.yml     ← CI: builds x64 + x86, creates release ZIP
├── CMakeLists.txt
├── include/
│   ├── platform.h                  ← OS abstraction (LoadLibrary/dlopen, CRITICAL_SECTION/pthread, GetEnvironmentVariable/getenv)
│   └── stereo_icd.h                ← types, dispatch tables, object wrappers
├── src/
│   ├── VKS3D.def                   ← Windows DLL export definition (3 ICD symbols)
│   ├── VKS3D.rc                    ← Windows version/metadata resource
│   ├── icd_main.c                  ← vk_icdNegotiateLoaderICDInterfaceVersion, vk_icdGetInstanceProcAddr
│   ├── stereo.c                    ← config, object registry, LoadLibrary/registry ICD loader, DllMain
│   ├── instance.c                  ← vkCreateInstance / DestroyInstance
│   ├── device.c                    ← vkCreateDevice (multiview injection + stereo UBO)
│   ├── render_pass.c               ← multiview render pass injection
│   ├── shader.c                    ← SPIR-V binary patcher (no external tools)
│   ├── swapchain.c                 ← SBS swapchain (2× width)
│   └── present.c                   ← SBS blit composite at QueuePresent
├── shaders/
│   ├── composite.vert.glsl
│   └── composite.frag.glsl
├── manifest/
│   ├── VKS3D_x64.json              ← Vulkan loader manifest (64-bit)
│   └── VKS3D_x86.json              ← Vulkan loader manifest (32-bit)
├── scripts/
│   ├── install.bat                 ← Registry installer (run as Admin)
│   ├── uninstall.bat               ← Registry uninstaller
│   └── Install-VKS3D.ps1          ← PowerShell installer/uninstaller
└── tests/
    ├── demo.c                      ← headless integration demo
    ├── test_spirv_patch.c          ← SPIR-V patcher unit tests
    └── test_stereo_math.c          ← stereo math unit tests
```

---

## ICD interface

VKS3D implements **Vulkan Loader/ICD Interface Version 5**.

The `.def` file (`src/VKS3D.def`) exports exactly three symbols — guaranteeing
correct undecorated names on both x64 and x86 (where `__stdcall` would
otherwise mangle them):

| Exported symbol | Purpose |
|---|---|
| `vk_icdNegotiateLoaderICDInterfaceVersion` | ABI handshake with loader |
| `vk_icdGetInstanceProcAddr` | All function pointer lookups |
| `vk_icdGetPhysicalDeviceProcAddr` | Physical device function lookups |
| `vks3d_internal_marker` | Self-detection guard (prevents loading ourselves) |

---

## Verify installation

```powershell
# List all registered Vulkan ICDs and check VKS3D is present:
vulkaninfo 2>&1 | Select-String -Pattern "VKS3D|icd"

# Or check the registry directly:
Get-ItemProperty "HKLM:\SOFTWARE\Khronos\Vulkan\Drivers"
Get-ItemProperty "HKLM:\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers"
```

---

## Known limitations

- **Dynamic rendering** (`VK_KHR_dynamic_rendering`) not yet intercepted. Apps
  using it need multiview set at pipeline creation time.
- **SPIR-V patcher** is a minimal binary rewriter; it does not use spirv-tools.
  Complex shaders with unusual variable layouts may need the external tool path.
- **Composite synchronisation** uses a synchronous fence wait at present time.
  A semaphore-based pipeline is planned to remove the CPU stall.
- **32-bit apps on 64-bit Windows** use `VKS3D_x86.dll` automatically via the
  `WOW6432Node` registry key — no user action needed.

## License

GPLv3 — see [LICENSE](LICENSE)

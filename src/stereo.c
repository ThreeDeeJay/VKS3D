/*
 * stereo.c — Stereo configuration, global object registry, real-ICD loader
 *
 * Windows ICD auto-detection
 * ──────────────────────────
 * On Windows the Vulkan loader registers ICDs under:
 *   HKLM\SOFTWARE\Khronos\Vulkan\Drivers            (64-bit process)
 *   HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers (32-bit process)
 *
 * Each value name is the path to a JSON manifest whose "library_path"
 * field points to the actual ICD DLL.  We enumerate these, skip any
 * JSON that points to ourselves (detected by checking for our own
 * export symbol), and load the first working one.
 *
 * STEREO_REAL_ICD env var overrides auto-detection and should be the
 * direct path to the real ICD DLL (e.g. C:\Windows\System32\nvoglv64.dll)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"

/* ── Global object registries (some exported for swapchain.c) ─────────────── */

static StereoInstance        g_instances[MAX_INSTANCES];
uint32_t                     g_instance_count = 0;

static StereoPhysicalDevice  g_physdevs[MAX_PHYSICAL_DEVICES];
static uint32_t              g_physdev_count = 0;

StereoDevice                 g_devices[MAX_DEVICES];
uint32_t                     g_device_count = 0;

static stereo_mutex_t        g_registry_lock;

/* One-time mutex initialisation */
#ifdef _WIN32
static INIT_ONCE g_registry_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK registry_init_once(PINIT_ONCE io, PVOID p, PVOID *ctx)
{
    (void)io; (void)p; (void)ctx;
    stereo_mutex_init(&g_registry_lock);
    return TRUE;
}
static void ensure_registry_init(void) {
    InitOnceExecuteOnce(&g_registry_once, registry_init_once, NULL, NULL);
}
#else
#include <pthread.h>
static pthread_once_t g_registry_once = PTHREAD_ONCE_INIT;
static void registry_init_fn(void) { stereo_mutex_init(&g_registry_lock); }
static void ensure_registry_init(void) { pthread_once(&g_registry_once, registry_init_fn); }
#endif

/* ── Real ICD state ──────────────────────────────────────────────────────── */
static stereo_dl_t               g_real_icd_handle = STEREO_DL_NULL;
static PFN_vkGetInstanceProcAddr g_real_giPA        = NULL;

/* ── Stereo config ───────────────────────────────────────────────────────── */

void stereo_config_init(StereoConfig *cfg)
{
    const char *env;
    cfg->enabled = true;
    env = stereo_getenv("STEREO_ENABLED");
    if (env && atoi(env) == 0) cfg->enabled = false;

    cfg->separation = 0.065f;
    env = stereo_getenv("STEREO_SEPARATION");
    if (env) cfg->separation = (float)atof(env);

    cfg->convergence = 0.030f;
    env = stereo_getenv("STEREO_CONVERGENCE");
    if (env) cfg->convergence = (float)atof(env);

    cfg->flip_eyes = false;
    env = stereo_getenv("STEREO_FLIP_EYES");
    if (env && atoi(env) == 1) cfg->flip_eyes = true;

    stereo_config_compute_offsets(cfg);
    STEREO_LOG("Stereo: enabled=%d sep=%.4f conv=%.4f flip=%d",
               cfg->enabled, cfg->separation, cfg->convergence, cfg->flip_eyes);
}

void stereo_config_compute_offsets(StereoConfig *cfg)
{
    float half_sep  = cfg->separation  / 2.0f;
    float half_conv = cfg->convergence / 2.0f;
    if (!cfg->flip_eyes) {
        cfg->left_eye_offset  = -half_sep + half_conv;
        cfg->right_eye_offset = +half_sep - half_conv;
    } else {
        cfg->left_eye_offset  = +half_sep - half_conv;
        cfg->right_eye_offset = -half_sep + half_conv;
    }
}

/* ── Real ICD loading ────────────────────────────────────────────────────── */

static bool try_load_icd(const char *path)
{
    if (!path || !path[0]) return false;
    stereo_dl_t h = stereo_dl_open(path);
    if (!h) return false;
    /* Reject ourselves */
    if (stereo_dl_sym(h, "vks3d_internal_marker")) {
        stereo_dl_close(h);
        return false;
    }
    PFN_vkGetInstanceProcAddr giPA =
        (PFN_vkGetInstanceProcAddr)(uintptr_t)
        stereo_dl_sym(h, "vk_icdGetInstanceProcAddr");
    if (!giPA)
        giPA = (PFN_vkGetInstanceProcAddr)(uintptr_t)
               stereo_dl_sym(h, "vkGetInstanceProcAddr");
    if (!giPA) { stereo_dl_close(h); return false; }
    g_real_icd_handle = h;
    g_real_giPA       = giPA;
    STEREO_LOG("Loaded real ICD: %s", path);
    return true;
}

#ifdef _WIN32
static const char *WIN_FALLBACKS_X64[] = {
    "nvoglv64.dll",
    "amdvlk64.dll",
    "C:\\Windows\\System32\\nvoglv64.dll",
    "C:\\Windows\\System32\\amdvlk64.dll",
    "C:\\Windows\\System32\\igvk64.dll",
    NULL
};
static const char *WIN_FALLBACKS_X86[] = {
    "nvoglv32.dll",
    "amdvlk32.dll",
    "C:\\Windows\\SysWOW64\\nvoglv32.dll",
    "C:\\Windows\\SysWOW64\\amdvlk32.dll",
    "C:\\Windows\\SysWOW64\\igvk32.dll",
    NULL
};

bool stereo_load_real_icd(void)
{
    if (g_real_icd_handle) return true;

    /* 1. Explicit env var */
    const char *explicit_path = stereo_getenv("STEREO_REAL_ICD");
    if (explicit_path && try_load_icd(explicit_path)) return true;

    /* 2. Registry enumeration */
    char **json_paths = stereo_registry_enum_icd_jsons();
    if (json_paths) {
        for (int i = 0; json_paths[i]; i++) {
            char *dll_path = stereo_json_read_library_path(json_paths[i]);
            if (dll_path) {
                bool ok = try_load_icd(dll_path);
                free(dll_path);
                if (ok) {
                    for (int j = i; json_paths[j]; j++) free(json_paths[j]);
                    free(json_paths);
                    return true;
                }
            }
            free(json_paths[i]);
        }
        free(json_paths);
    }

    /* 3. Hard-coded fallbacks */
#ifdef _WIN64
    const char **fb = WIN_FALLBACKS_X64;
#else
    const char **fb = WIN_FALLBACKS_X86;
#endif
    for (int i = 0; fb[i]; i++)
        if (try_load_icd(fb[i])) return true;

    STEREO_ERR("No real GPU ICD found. Set STEREO_REAL_ICD=C:\\path\\to\\icd.dll");
    return false;
}

#else /* Linux */

static const char *LINUX_FALLBACKS[] = {
    "/usr/lib/x86_64-linux-gnu/libvulkan_intel.so",
    "/usr/lib/x86_64-linux-gnu/libvulkan_radeon.so",
    "/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so",
    "/usr/lib64/libvulkan_intel.so",
    "/usr/lib64/libvulkan_radeon.so",
    "/usr/lib/i386-linux-gnu/libvulkan_intel.so",
    "/usr/lib/i386-linux-gnu/libvulkan_radeon.so",
    NULL
};

bool stereo_load_real_icd(void)
{
    if (g_real_icd_handle) return true;
    const char *ep = stereo_getenv("STEREO_REAL_ICD");
    if (ep && try_load_icd(ep)) return true;
    for (int i = 0; LINUX_FALLBACKS[i]; i++)
        if (try_load_icd(LINUX_FALLBACKS[i])) return true;
    STEREO_ERR("No real GPU ICD found. Set STEREO_REAL_ICD=/path/to/libvulkan_xxx.so");
    return false;
}
#endif

PFN_vkGetInstanceProcAddr stereo_get_real_giPA(void) { return g_real_giPA; }

/* ── Instance registry ───────────────────────────────────────────────────── */

StereoInstance *stereo_instance_alloc(void) {
    ensure_registry_init();
    stereo_mutex_lock(&g_registry_lock);
    if (g_instance_count >= MAX_INSTANCES) {
        stereo_mutex_unlock(&g_registry_lock); return NULL;
    }
    StereoInstance *si = &g_instances[g_instance_count++];
    memset(si, 0, sizeof(*si));
    stereo_mutex_unlock(&g_registry_lock);
    return si;
}
StereoInstance *stereo_instance_from_handle(VkInstance h) {
    ensure_registry_init();
    stereo_mutex_lock(&g_registry_lock);
    for (uint32_t i = 0; i < g_instance_count; i++) {
        if (g_instances[i].real_instance == h) {
            stereo_mutex_unlock(&g_registry_lock); return &g_instances[i];
        }
    }
    stereo_mutex_unlock(&g_registry_lock);
    return NULL;
}
void stereo_instance_free(VkInstance h) {
    ensure_registry_init();
    stereo_mutex_lock(&g_registry_lock);
    for (uint32_t i = 0; i < g_instance_count; i++) {
        if (g_instances[i].real_instance == h) {
            g_instances[i] = g_instances[--g_instance_count]; break;
        }
    }
    stereo_mutex_unlock(&g_registry_lock);
}

/* ── Physical device registry ────────────────────────────────────────────── */

StereoPhysicalDevice *stereo_physdev_alloc(void) {
    ensure_registry_init();
    stereo_mutex_lock(&g_registry_lock);
    if (g_physdev_count >= MAX_PHYSICAL_DEVICES) {
        stereo_mutex_unlock(&g_registry_lock); return NULL;
    }
    StereoPhysicalDevice *sp = &g_physdevs[g_physdev_count++];
    memset(sp, 0, sizeof(*sp));
    stereo_mutex_unlock(&g_registry_lock);
    return sp;
}
StereoPhysicalDevice *stereo_physdev_from_handle(VkPhysicalDevice h) {
    ensure_registry_init();
    stereo_mutex_lock(&g_registry_lock);
    for (uint32_t i = 0; i < g_physdev_count; i++) {
        if (g_physdevs[i].real == h) {
            stereo_mutex_unlock(&g_registry_lock); return &g_physdevs[i];
        }
    }
    stereo_mutex_unlock(&g_registry_lock);
    return NULL;
}

/* ── Device registry ─────────────────────────────────────────────────────── */

StereoDevice *stereo_device_alloc(void) {
    ensure_registry_init();
    stereo_mutex_lock(&g_registry_lock);
    if (g_device_count >= MAX_DEVICES) {
        stereo_mutex_unlock(&g_registry_lock); return NULL;
    }
    StereoDevice *sd = &g_devices[g_device_count++];
    memset(sd, 0, sizeof(*sd));
    stereo_mutex_init(&sd->lock);
    stereo_mutex_unlock(&g_registry_lock);
    return sd;
}
StereoDevice *stereo_device_from_handle(VkDevice h) {
    ensure_registry_init();
    stereo_mutex_lock(&g_registry_lock);
    for (uint32_t i = 0; i < g_device_count; i++) {
        if (g_devices[i].real_device == h) {
            stereo_mutex_unlock(&g_registry_lock); return &g_devices[i];
        }
    }
    stereo_mutex_unlock(&g_registry_lock);
    return NULL;
}
void stereo_device_free(VkDevice h) {
    ensure_registry_init();
    stereo_mutex_lock(&g_registry_lock);
    for (uint32_t i = 0; i < g_device_count; i++) {
        if (g_devices[i].real_device == h) {
            stereo_mutex_destroy(&g_devices[i].lock);
            g_devices[i] = g_devices[--g_device_count]; break;
        }
    }
    stereo_mutex_unlock(&g_registry_lock);
}

/* ── Lookup helpers ─────────────────────────────────────────────────────── */

StereoSwapchain *stereo_swapchain_lookup(StereoDevice *dev, VkSwapchainKHR sc) {
    for (uint32_t i = 0; i < dev->swapchain_count; i++)
        if (dev->swapchains[i].real_swapchain == sc) return &dev->swapchains[i];
    return NULL;
}
StereoRenderPassInfo *stereo_rp_lookup(StereoDevice *dev, VkRenderPass rp) {
    for (uint32_t i = 0; i < dev->render_pass_count; i++)
        if (dev->render_passes[i].handle == rp) return &dev->render_passes[i];
    return NULL;
}

/* ── Dispatch table population ──────────────────────────────────────────── */

#define LOAD_INST(table, inst, fn) \
    (table)->fn = (PFN_vk##fn)(g_real_giPA)((inst), "vk"#fn)

void stereo_populate_instance_dispatch(StereoInstance *si)
{
    VkInstance ri = si->real_instance;
    LOAD_INST(&si->real, ri, DestroyInstance);
    LOAD_INST(&si->real, ri, EnumeratePhysicalDevices);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceProperties);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceProperties2);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceFeatures);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceFeatures2);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceMemoryProperties);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceMemoryProperties2);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceQueueFamilyProperties);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceQueueFamilyProperties2);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceFormatProperties);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceFormatProperties2);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceImageFormatProperties);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceSparseImageFormatProperties);
    LOAD_INST(&si->real, ri, EnumerateDeviceExtensionProperties);
    LOAD_INST(&si->real, ri, EnumerateDeviceLayerProperties);
    LOAD_INST(&si->real, ri, CreateDevice);
    LOAD_INST(&si->real, ri, DestroySurfaceKHR);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceSurfaceSupportKHR);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceSurfacePresentModesKHR);
    si->real.CreateDebugUtilsMessengerEXT  = (PFN_vkCreateDebugUtilsMessengerEXT)
        g_real_giPA(ri, "vkCreateDebugUtilsMessengerEXT");
    si->real.DestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)
        g_real_giPA(ri, "vkDestroyDebugUtilsMessengerEXT");
}

void stereo_populate_device_dispatch(StereoDevice *sd, VkInstance real_inst)
{
#define L(fn) sd->real.fn = (PFN_vk##fn)(g_real_giPA)((real_inst), "vk"#fn)
    L(DestroyDevice); L(GetDeviceQueue); L(QueueSubmit); L(QueueWaitIdle);
    L(DeviceWaitIdle); L(AllocateMemory); L(FreeMemory); L(MapMemory);
    L(UnmapMemory); L(FlushMappedMemoryRanges); L(InvalidateMappedMemoryRanges);
    L(BindBufferMemory); L(BindImageMemory);
    L(GetBufferMemoryRequirements); L(GetImageMemoryRequirements);
    L(CreateFence); L(DestroyFence); L(ResetFences); L(GetFenceStatus); L(WaitForFences);
    L(CreateSemaphore); L(DestroySemaphore);
    L(CreateEvent); L(DestroyEvent); L(GetEventStatus); L(SetEvent); L(ResetEvent);
    L(CreateQueryPool); L(DestroyQueryPool); L(GetQueryPoolResults);
    L(CreateBuffer); L(DestroyBuffer); L(CreateBufferView); L(DestroyBufferView);
    L(CreateImage); L(DestroyImage); L(GetImageSubresourceLayout);
    L(CreateImageView); L(DestroyImageView);
    L(CreateShaderModule); L(DestroyShaderModule);
    L(CreatePipelineCache); L(DestroyPipelineCache);
    L(GetPipelineCacheData); L(MergePipelineCaches);
    L(CreateGraphicsPipelines); L(CreateComputePipelines); L(DestroyPipeline);
    L(CreatePipelineLayout); L(DestroyPipelineLayout);
    L(CreateSampler); L(DestroySampler);
    L(CreateDescriptorSetLayout); L(DestroyDescriptorSetLayout);
    L(CreateDescriptorPool); L(DestroyDescriptorPool);
    L(ResetDescriptorPool); L(AllocateDescriptorSets); L(FreeDescriptorSets);
    L(UpdateDescriptorSets);
    L(CreateFramebuffer); L(DestroyFramebuffer);
    L(CreateRenderPass); L(DestroyRenderPass); L(GetRenderAreaGranularity);
    L(CreateCommandPool); L(DestroyCommandPool); L(ResetCommandPool);
    L(AllocateCommandBuffers); L(FreeCommandBuffers);
    L(BeginCommandBuffer); L(EndCommandBuffer); L(ResetCommandBuffer);
    L(CmdBindPipeline); L(CmdSetViewport); L(CmdSetScissor);
    L(CmdSetLineWidth); L(CmdSetDepthBias); L(CmdSetBlendConstants);
    L(CmdSetDepthBounds); L(CmdSetStencilCompareMask);
    L(CmdSetStencilWriteMask); L(CmdSetStencilReference);
    L(CmdBindDescriptorSets); L(CmdBindIndexBuffer); L(CmdBindVertexBuffers);
    L(CmdDraw); L(CmdDrawIndexed); L(CmdDrawIndirect); L(CmdDrawIndexedIndirect);
    L(CmdDispatch); L(CmdDispatchIndirect);
    L(CmdCopyBuffer); L(CmdCopyImage); L(CmdBlitImage);
    L(CmdCopyBufferToImage); L(CmdCopyImageToBuffer);
    L(CmdUpdateBuffer); L(CmdFillBuffer);
    L(CmdClearColorImage); L(CmdClearDepthStencilImage); L(CmdClearAttachments);
    L(CmdResolveImage); L(CmdSetEvent); L(CmdResetEvent); L(CmdWaitEvents);
    L(CmdPipelineBarrier); L(CmdBeginQuery); L(CmdEndQuery);
    L(CmdResetQueryPool); L(CmdWriteTimestamp); L(CmdCopyQueryPoolResults);
    L(CmdPushConstants);
    L(CmdBeginRenderPass); L(CmdNextSubpass); L(CmdEndRenderPass);
    L(CmdExecuteCommands);
    L(CreateSwapchainKHR); L(DestroySwapchainKHR);
    L(GetSwapchainImagesKHR); L(AcquireNextImageKHR); L(QueuePresentKHR);
    sd->real.CreateRenderPass2KHR = (PFN_vkCreateRenderPass2KHR)
        g_real_giPA(real_inst, "vkCreateRenderPass2KHR");
#undef L
}

/* ── Windows DllMain + self-identification marker ───────────────────────── */
#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hinstDLL);
    return TRUE;
}

/* Exported marker so try_load_icd() can detect us and skip self-loading */
STEREO_EXPORT void vks3d_internal_marker(void) {}
#endif

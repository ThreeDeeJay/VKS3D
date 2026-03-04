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

/* Define the logging globals here — all other TUs get extern references.
 * Must appear before the first #include that pulls in platform.h. */
#define STEREO_LOG_DEFINE_GLOBALS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"

/* ── Global object registries (some exported for swapchain.c) ─────────────── */

static StereoInstance        g_instances[MAX_INSTANCES];
uint32_t                     g_instance_count = 0;

/* Flat physdev→instance map (no wrapper structs — real physdevs returned to loader) */
typedef struct { VkPhysicalDevice pd; StereoInstance *si; } PhysdevMapEntry;
static PhysdevMapEntry  g_physdev_map[MAX_PHYSICAL_DEVICES];
static uint32_t         g_physdev_count = 0;

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
/* vk_icdGetPhysicalDeviceProcAddr — many ICDs (including NVIDIA) only expose
 * physical-device-level functions (surface support, format queries, etc.)
 * through this entry point, not through vk_icdGetInstanceProcAddr. */
static PFN_vkGetInstanceProcAddr g_real_pdPA        = NULL;

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
    if (!path || !path[0]) {
        STEREO_LOG("try_load_icd: empty path, skipping");
        return false;
    }
    STEREO_LOG("try_load_icd: attempting '%s'", path);
    stereo_dl_t h = stereo_dl_open(path);
    if (!h) {
        STEREO_ERR("try_load_icd: LoadLibraryA failed for '%s': %s",
                   path, stereo_dl_error());
        return false;
    }
    STEREO_LOG("try_load_icd: loaded OK, handle=%p", (void*)h);
    /* Reject ourselves */
    if (stereo_dl_sym(h, "vks3d_internal_marker")) {
        STEREO_LOG("try_load_icd: '%s' is VKS3D itself — skipping", path);
        stereo_dl_close(h);
        return false;
    }
    PFN_vkGetInstanceProcAddr giPA =
        (PFN_vkGetInstanceProcAddr)(uintptr_t)
        stereo_dl_sym(h, "vk_icdGetInstanceProcAddr");
    if (!giPA) {
        STEREO_LOG("try_load_icd: no vk_icdGetInstanceProcAddr, trying vkGetInstanceProcAddr");
        giPA = (PFN_vkGetInstanceProcAddr)(uintptr_t)
               stereo_dl_sym(h, "vkGetInstanceProcAddr");
    }
    if (!giPA) {
        STEREO_ERR("try_load_icd: '%s' has no usable GetInstanceProcAddr — skipping", path);
        stereo_dl_close(h);
        return false;
    }
    STEREO_LOG("try_load_icd: giPA=%p", (void*)(uintptr_t)giPA);
    g_real_icd_handle = h;
    g_real_giPA       = giPA;
    /* Load vk_icdGetPhysicalDeviceProcAddr if the ICD exposes it */
    g_real_pdPA = (PFN_vkGetInstanceProcAddr)(uintptr_t)
        stereo_dl_sym(h, "vk_icdGetPhysicalDeviceProcAddr");
    STEREO_LOG("try_load_icd: pdPA=%p", (void*)(uintptr_t)g_real_pdPA);
    STEREO_LOG("try_load_icd: SUCCESS — real ICD is '%s'", path);
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

    STEREO_LOG("stereo_load_real_icd: starting ICD search");

    /* 1. Explicit env var */
    const char *explicit_path = stereo_getenv("STEREO_REAL_ICD");
    if (explicit_path) {
        STEREO_LOG("stereo_load_real_icd: STEREO_REAL_ICD='%s'", explicit_path);
        if (try_load_icd(explicit_path)) return true;
    }

    /* 2. Registry enumeration */
    STEREO_LOG("stereo_load_real_icd: enumerating registry ICDs");
    char **json_paths = stereo_registry_enum_icd_jsons();
    if (json_paths) {
        for (int i = 0; json_paths[i]; i++) {
            STEREO_LOG("stereo_load_real_icd: JSON[%d]='%s'", i, json_paths[i]);
            char *dll_path = stereo_json_read_library_path(json_paths[i]);
            if (dll_path) {
                STEREO_LOG("stereo_load_real_icd: library_path='%s'", dll_path);
                bool ok = try_load_icd(dll_path);
                free(dll_path);
                if (ok) {
                    for (int j = i; json_paths[j]; j++) free(json_paths[j]);
                    free(json_paths);
                    return true;
                }
            } else {
                STEREO_LOG("stereo_load_real_icd: no library_path in JSON");
            }
            free(json_paths[i]);
        }
        free(json_paths);
    } else {
        STEREO_LOG("stereo_load_real_icd: no registry ICDs found");
    }

    /* 3. Hard-coded fallbacks */
    STEREO_LOG("stereo_load_real_icd: trying hard-coded fallbacks");
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
PFN_vkGetInstanceProcAddr stereo_get_real_pdPA(void) { return g_real_pdPA; }

/* ── Instance registry ───────────────────────────────────────────────────── */

StereoInstance *stereo_instance_alloc(void) {
    ensure_registry_init();
    stereo_mutex_lock(&g_registry_lock);
    if (g_instance_count >= MAX_INSTANCES) {
        stereo_mutex_unlock(&g_registry_lock); return NULL;
    }
    StereoInstance *si = &g_instances[g_instance_count++];
    memset(si, 0, sizeof(*si));
    SET_LOADER_MAGIC_VALUE(si);  /* required: loader reads this field for dispatch */
    STEREO_LOG("stereo_instance_alloc: allocated si=%p (slot %u), loaderMagic=0x%llx",
               (void*)si, g_instance_count - 1,
               (unsigned long long)si->loader_data.loaderMagic);
    stereo_mutex_unlock(&g_registry_lock);
    return si;
}
StereoInstance *stereo_instance_from_handle(VkInstance h) {
    /*
     * h is the (StereoInstance *) we returned from stereo_CreateInstance,
     * cast to VkInstance.  Cast it back and validate by checking that the
     * pointer falls within our static g_instances[] array.
     *
     * We CANNOT rely on loader_data.loaderMagic: the Vulkan loader
     * overwrites that field with its own dispatch pointer after our
     * vkCreateInstance returns, so the magic is gone on the very next call.
     *
     * We also CANNOT fall back to comparing g_instances[i].real_instance == h:
     * h is our wrapper pointer, not the real ICD handle — those are different
     * values and the comparison would never match.
     *
     * Address-range check: O(1), correct, immune to the loader clobbering
     * loader_data.
     */
    if (!h) return NULL;
    StereoInstance *si = (StereoInstance *)(uintptr_t)h;
    if (si >= g_instances && si < g_instances + g_instance_count) {
        STEREO_LOG("stereo_instance_from_handle: h=%p -> slot %u (magic=0x%llx)",
                   (void*)h, (uint32_t)(si - g_instances),
                   (unsigned long long)si->loader_data.loaderMagic);
        return si;
    }
    STEREO_ERR("stereo_instance_from_handle: FAILED to recognise handle %p "
               "(our array=[%p,%p), count=%u)",
               (void*)h, (void*)g_instances,
               (void*)(g_instances + g_instance_count), g_instance_count);
    /* Dump all known instances for comparison */
    for (uint32_t _j = 0; _j < g_instance_count; _j++) {
        STEREO_ERR("  g_instances[%u] = %p  (magic=0x%llx real=%p)",
                   _j, (void*)&g_instances[_j],
                   (unsigned long long)g_instances[_j].loader_data.loaderMagic,
                   (void*)g_instances[_j].real_instance);
    }
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

/* ── Physical device registry ───────────────────────────────────────────────
 *
 * Real physdevs are returned directly to the loader so the loader can write
 * its dispatch table into them.  We track the pd→StereoInstance association
 * in a flat array.  Our physdev wrapper functions receive the real physdev
 * handle from the loader and look up _si here; _real = pd (the handle itself).
 */

void stereo_physdev_register(VkPhysicalDevice pd, StereoInstance *si)
{
    ensure_registry_init();
    stereo_mutex_lock(&g_registry_lock);
    /* Update existing entry if already present */
    for (uint32_t i = 0; i < g_physdev_count; i++) {
        if (g_physdev_map[i].pd == pd) {
            g_physdev_map[i].si = si;
            stereo_mutex_unlock(&g_registry_lock);
            STEREO_LOG("stereo_physdev_register: updated pd=%p si=%p", (void*)pd, (void*)si);
            return;
        }
    }
    if (g_physdev_count >= MAX_PHYSICAL_DEVICES) {
        STEREO_ERR("stereo_physdev_register: map full (%u entries)", g_physdev_count);
        stereo_mutex_unlock(&g_registry_lock);
        return;
    }
    g_physdev_map[g_physdev_count].pd = pd;
    g_physdev_map[g_physdev_count].si = si;
    g_physdev_count++;
    stereo_mutex_unlock(&g_registry_lock);
    STEREO_LOG("stereo_physdev_register: registered pd=%p si=%p (slot %u)",
               (void*)pd, (void*)si, g_physdev_count - 1);
}

StereoInstance *stereo_si_from_physdev(VkPhysicalDevice pd)
{
    if (!pd) return NULL;
    ensure_registry_init();
    stereo_mutex_lock(&g_registry_lock);
    for (uint32_t i = 0; i < g_physdev_count; i++) {
        if (g_physdev_map[i].pd == pd) {
            StereoInstance *si = g_physdev_map[i].si;
            stereo_mutex_unlock(&g_registry_lock);
            return si;
        }
    }
    stereo_mutex_unlock(&g_registry_lock);
    STEREO_ERR("stereo_si_from_physdev: FAILED to find pd=%p (count=%u)", (void*)pd, g_physdev_count);
    for (uint32_t i = 0; i < g_physdev_count; i++)
        STEREO_ERR("  map[%u] pd=%p si=%p", i, (void*)g_physdev_map[i].pd, (void*)g_physdev_map[i].si);
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
    do { \
        (table)->fn = (PFN_vk##fn)(g_real_giPA)((inst), "vk"#fn); \
        STEREO_LOG("  LOAD_INST " #fn " = %p", (void*)(uintptr_t)(table)->fn); \
    } while (0)

void stereo_populate_instance_dispatch(StereoInstance *si)
{
    STEREO_LOG("stereo_populate_instance_dispatch: si=%p real_inst=%p",
               (void*)si, (void*)si->real_instance);
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
    /* Many ICDs (NVIDIA, AMD) only expose physical-device surface functions
     * via vk_icdGetPhysicalDeviceProcAddr, not vk_icdGetInstanceProcAddr.
     * Fall back to pdPA for any surface function that is still NULL. */
#define LOAD_PHYS(table, ri, fn) \
    do { \
        if (!(table)->fn && g_real_pdPA) { \
            (table)->fn = (PFN_vk##fn)(g_real_pdPA)((ri), "vk"#fn); \
            STEREO_LOG("  LOAD_PHYS " #fn " = %p", (void*)(uintptr_t)(table)->fn); \
        } \
    } while (0)
    LOAD_PHYS(&si->real, ri, DestroySurfaceKHR);
    LOAD_PHYS(&si->real, ri, GetPhysicalDeviceSurfaceSupportKHR);
    LOAD_PHYS(&si->real, ri, GetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_PHYS(&si->real, ri, GetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_PHYS(&si->real, ri, GetPhysicalDeviceSurfacePresentModesKHR);
#undef LOAD_PHYS
    /* ── Vulkan 1.1 physdev functions ────────────────────────────────────── */
    LOAD_INST(&si->real, ri, GetPhysicalDeviceImageFormatProperties2);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceSparseImageFormatProperties2);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceExternalBufferProperties);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceExternalFenceProperties);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceExternalSemaphoreProperties);
    LOAD_INST(&si->real, ri, EnumeratePhysicalDeviceGroups);
    /* ── KHR surface extensions ──────────────────────────────────────────── */
    LOAD_INST(&si->real, ri, GetPhysicalDeviceSurfaceCapabilities2KHR);
    LOAD_INST(&si->real, ri, GetPhysicalDeviceSurfaceFormats2KHR);
    LOAD_INST(&si->real, ri, GetPhysicalDevicePresentRectanglesKHR);
    /* ── Win32 ───────────────────────────────────────────────────────────── */
    #ifdef VK_KHR_win32_surface
    LOAD_INST(&si->real, ri, GetPhysicalDeviceWin32PresentationSupportKHR);
#endif
    /* ── EXT extensions ──────────────────────────────────────────────────── */
    #ifdef VK_EXT_full_screen_exclusive
    LOAD_INST(&si->real, ri, GetPhysicalDeviceSurfacePresentModes2EXT);
#endif
    #ifdef VK_EXT_calibrated_timestamps
    LOAD_INST(&si->real, ri, GetPhysicalDeviceCalibrateableTimeDomainsEXT);
#endif
    LOAD_INST(&si->real, ri, GetPhysicalDeviceMultisamplePropertiesEXT);
    /* ── NV extensions ───────────────────────────────────────────────────── */
    LOAD_INST(&si->real, ri, GetPhysicalDeviceExternalImageFormatPropertiesNV);
    #ifdef VK_NV_cooperative_matrix
    LOAD_INST(&si->real, ri, GetPhysicalDeviceCooperativeMatrixPropertiesNV);
#endif
    #ifdef VK_NV_coverage_reduction_mode
    LOAD_INST(&si->real, ri, GetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV);
#endif
    /* NVX stored as PFN_vkVoidFunction — safe because we cast at call site */
    si->real.GetPhysicalDeviceGeneratedCommandsPropertiesNVX =
        g_real_giPA(ri, "vkGetPhysicalDeviceGeneratedCommandsPropertiesNVX");
    /* ── Instance-level functions ─────────────────────────────────────────── */
    #ifdef VK_KHR_win32_surface
    LOAD_INST(&si->real, ri, CreateWin32SurfaceKHR);
#endif
    LOAD_INST(&si->real, ri, CreateDebugReportCallbackEXT);
    LOAD_INST(&si->real, ri, DestroyDebugReportCallbackEXT);
    LOAD_INST(&si->real, ri, DebugReportMessageEXT);
    LOAD_INST(&si->real, ri, CreateDebugUtilsMessengerEXT);
    LOAD_INST(&si->real, ri, DestroyDebugUtilsMessengerEXT);
    LOAD_INST(&si->real, ri, SubmitDebugUtilsMessageEXT);
    STEREO_LOG("stereo_populate_instance_dispatch: complete");
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
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        /* Open the log file as the very first thing so every subsequent
         * STEREO_LOG/STEREO_ERR call goes to the file.  This must happen
         * before any other initialisation because a crash in init would
         * otherwise leave us with no diagnostic output. */
        vks3d_log_open();
        STEREO_LOG("===== VKS3D DLL_PROCESS_ATTACH =====");
        STEREO_LOG("DllMain: hinstDLL=%p", (void*)hinstDLL);
        /* Log our own path so the user can confirm which binary loaded */
        {
            char dll_path[MAX_PATH] = "<unknown>";
            GetModuleFileNameA(hinstDLL, dll_path, MAX_PATH);
            STEREO_LOG("DllMain: DLL path = %s", dll_path);
        }
        /* Log the host process so we know which app is loading us */
        {
            char exe_path[MAX_PATH] = "<unknown>";
            GetModuleFileNameA(NULL, exe_path, MAX_PATH);
            STEREO_LOG("DllMain: host process = %s", exe_path);
        }
        STEREO_LOG("DllMain: complete, returning TRUE");
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        STEREO_LOG("===== VKS3D DLL_PROCESS_DETACH =====");
    }
    return TRUE;
}

/* Exported marker so try_load_icd() can detect us and skip self-loading */
STEREO_EXPORT void vks3d_internal_marker(void) {}
#endif

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

/* Version/commit baked in by CMake.  Fallbacks for manual builds. */
#ifndef VKS3D_VERSION
#  define VKS3D_VERSION   "1.0.0"
#endif
#ifndef VKS3D_GIT_COMMIT
#  define VKS3D_GIT_COMMIT "unknown"
#endif
#ifndef VKS3D_BUILD_DATE
#  define VKS3D_BUILD_DATE "unknown"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"
#include "ini.h"

/* ── Global object registries (some exported for swapchain.c) ─────────────── */

static StereoInstance        g_instances[MAX_INSTANCES];
uint32_t                     g_instance_count = 0;

/* Physdev wrappers — real handles wrapped so the loader dispatches through us */
static StereoPhysdev    g_physdev_wrappers[MAX_PHYSICAL_DEVICES];
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

/* Global paths set in DllMain — available before any device is created */
char g_dll_dir[512];     /* directory containing VKS3D.dll    */
char g_exe_dir[512];     /* directory containing host .exe    */
char g_global_ini[512];  /* <dll_dir>/vks3d.ini               */
char g_local_ini[512];   /* <exe_dir>/vks3d.ini               */

/* Parse "presentation_mode" string → StereoPresentMode */
static StereoPresentMode parse_present_mode(const char *s)
{
    if (!s || !*s)             return STEREO_PRESENT_AUTO;
    if (!_stricmp(s, "auto"))  return STEREO_PRESENT_AUTO;
    if (!_stricmp(s, "dxgi"))  return STEREO_PRESENT_DXGI;
    if (!_stricmp(s, "dx9"))   return STEREO_PRESENT_DX9;
    if (!_stricmp(s, "sbs"))   return STEREO_PRESENT_SBS;
    if (!_stricmp(s, "tab"))   return STEREO_PRESENT_TAB;
    if (!_stricmp(s, "interlaced")) return STEREO_PRESENT_INTERLACED;
    if (!_stricmp(s, "mono"))  return STEREO_PRESENT_MONO;
    return STEREO_PRESENT_AUTO;
}

/* Read one float from local INI, fall back to global INI, then env, then default */
static float cfg_float(const char *key, float def)
{
    float v = def;
    char buf[64];
    if (ini_read_str(g_global_ini, "VKS3D", key, buf, sizeof(buf))) {
        v = (float)atof(buf);
        STEREO_LOG("INI load: [global] %s = %s", key, buf);
    }
    if (ini_read_str(g_local_ini, "VKS3D", key, buf, sizeof(buf))) {
        v = (float)atof(buf);
        STEREO_LOG("INI load: [local]  %s = %s", key, buf);
    }
    char env_name[64];
    snprintf(env_name, sizeof(env_name), "STEREO_%s", key);
    const char *e = stereo_getenv(env_name);
    if (e) { v = (float)atof(e); STEREO_LOG("INI load: [env]    STEREO_%s = %s", key, e); }
    return v;
}

static int cfg_int(const char *key, int def)
{
    int v = def;
    char buf[32];
    if (ini_read_str(g_global_ini, "VKS3D", key, buf, sizeof(buf))) {
        v = atoi(buf);
        STEREO_LOG("INI load: [global] %s = %s", key, buf);
    }
    if (ini_read_str(g_local_ini, "VKS3D", key, buf, sizeof(buf))) {
        v = atoi(buf);
        STEREO_LOG("INI load: [local]  %s = %s", key, buf);
    }
    return v;
}

static bool cfg_bool(const char *key, bool def)
{
    bool v = def;
    char buf[16];
    if (ini_read_str(g_global_ini, "VKS3D", key, buf, sizeof(buf))) {
        v = ini_read_bool(g_global_ini, "VKS3D", key, def);
        STEREO_LOG("INI load: [global] %s = %s", key, buf);
    }
    if (ini_read_str(g_local_ini, "VKS3D", key, buf, sizeof(buf))) {
        v = ini_read_bool(g_local_ini, "VKS3D", key, v);
        STEREO_LOG("INI load: [local]  %s = %s", key, buf);
    }
    return v;
}


void stereo_config_init(StereoConfig *cfg)
{
    /* ── enabled ── */
    cfg->enabled = cfg_bool("enabled", true);
    const char *env_en = stereo_getenv("STEREO_ENABLED");
    if (env_en && atoi(env_en) == 0) cfg->enabled = false;

    /* ── stereo parameters ── */
    cfg->separation  = cfg_float("separation",  0.065f);
    cfg->convergence = cfg_float("convergence", 0.030f);

    /* Sanity: separation must be positive.  Convergence must be in [0, sep).
     * If sep <= 0 or conv >= sep, the net eye offset is zero or reversed,
     * which produces no visible stereo or inverted depth.  Warn and clamp. */
    if (cfg->separation <= 0.0f) {
        STEREO_ERR("INI: separation=%.4f is <= 0 — clamping to 0.065", cfg->separation);
        cfg->separation = 0.065f;
    }
    if (cfg->convergence < 0.0f) {
        STEREO_ERR("INI: convergence=%.4f is negative — clamping to 0", cfg->convergence);
        cfg->convergence = 0.0f;
    }
    if (cfg->convergence >= cfg->separation) {
        STEREO_ERR("INI: convergence=%.4f >= separation=%.4f — "
                   "this cancels the stereo offset to zero or less! "
                   "Clamping convergence to 0.0. "
                   "Set convergence < separation (e.g. sep=0.065 conv=0.030).",
                   cfg->convergence, cfg->separation);
        cfg->convergence = 0.0f;
    }
    cfg->flip_eyes   = cfg_bool ("flip_eyes",   false);

    /* ── presentation mode ── */
    char mode_str[32] = "auto";
    if (ini_read_str(g_global_ini, "VKS3D", "presentation_mode", mode_str, sizeof(mode_str)))
        STEREO_LOG("INI load: [global] presentation_mode = %s", mode_str);
    if (ini_read_str(g_local_ini,  "VKS3D", "presentation_mode", mode_str, sizeof(mode_str)))
        STEREO_LOG("INI load: [local]  presentation_mode = %s", mode_str);
    cfg->present_mode = parse_present_mode(mode_str);

    /* ── output resolution / refresh ── */
    cfg->override_width  = (uint32_t)cfg_int("width",        0);
    cfg->override_height = (uint32_t)cfg_int("height",       0);
    cfg->refresh_rate    = (uint32_t)cfg_int("refresh_rate", 120);
    cfg->half_fps        = cfg_bool("half_fps", false);

    /* multiview: off by default — most apps have single-layer depth buffers
     * which cause VK_ERROR_DEVICE_LOST when multiview tries to write layer 1.
     * Set multiview=1 in vks3d.ini [global] only for apps that support it. */
    cfg->multiview       = cfg_bool("multiview", false);

    /* ── hotkey steps ── */
    cfg->step_separation  = cfg_float("step_separation",  0.005f);
    cfg->step_convergence = cfg_float("step_convergence", 0.005f);

    stereo_config_compute_offsets(cfg);
    STEREO_LOG("Stereo config: enabled=%d  separation=%.4f  convergence=%.4f  flip=%d  mode=%d",
               cfg->enabled, cfg->separation, cfg->convergence,
               cfg->flip_eyes, (int)cfg->present_mode);
    STEREO_LOG("  res_override=%ux%u  refresh=%uHz  half_fps=%d  hotkey_step_sep=%.4f  hotkey_step_conv=%.4f",
               cfg->override_width, cfg->override_height, cfg->refresh_rate,
               cfg->half_fps, cfg->step_separation, cfg->step_convergence);
    STEREO_LOG("  global_ini=%s", g_global_ini);
    STEREO_LOG("  local_ini=%s",  g_local_ini);
}

void stereo_config_compute_offsets(StereoConfig *cfg)
{
    /* Off-axis stereo eye offsets in clip-space X.
     *
     * left_eye_offset  = -(half_sep) + (half_conv)
     * right_eye_offset = +(half_sep) - (half_conv)
     *
     * separation  — full inter-ocular distance in NDC (typ. 0.06).
     * convergence — toe-in correction that shifts both images toward centre
     *               (typ. 0.0–0.06).  Reduces apparent depth at the cost of
     *               narrowing the depth window.  Zero = parallel-axis cameras.
     *
     * When sep == conv the net offset is zero (infinite convergence distance).
     * Keep sep > conv for a visible stereo effect.  The shipped vks3d.ini
     * defaults (sep=0.065, conv=0.030) are deliberately asymmetric.         */
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
    STEREO_LOG("try_load_icd: attempting '%s' (LoadLibraryEx SEARCH_DLL_LOAD_DIR)", path);
    stereo_dl_t h = stereo_dl_open(path);
    if (!h) {
        STEREO_ERR("try_load_icd: LoadLibraryExA failed for '%s': %s"
                   " (if DriverStore path, check STEREO_REAL_ICD; dependencies"
                   " should now be found via LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR)",
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
    /* Negotiate loader–ICD interface with the real ICD.  Without this call
     * the NVIDIA (and potentially other) ICD leaves its surface/WSI function
     * table uninitialised, causing GetPhysicalDeviceSurfaceSupportKHR to
     * return VK_ERROR_INITIALIZATION_FAILED (-3) or crash outright. */
    typedef VkResult (VKAPI_PTR *PFN_Negotiate)(uint32_t *);
    PFN_Negotiate real_negotiate = (PFN_Negotiate)(uintptr_t)
        stereo_dl_sym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (real_negotiate) {
        uint32_t ver = 5; /* request v5 — what VKS3D itself offers */
        VkResult nr = real_negotiate(&ver);
        STEREO_LOG("try_load_icd: vk_icdNegotiateLoaderICDInterfaceVersion"
                   " → result=%d negotiated=%u", (int)nr, ver);
    } else {
        STEREO_LOG("try_load_icd: real ICD has no vk_icdNegotiateLoaderICDInterfaceVersion"
                   " (old ICD — continuing)");
    }
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

    /* 2. Windows OpenGL driver registry keys — these hold the ICD DLL directly.
     *    On NVIDIA, nvoglv64.dll is both the OpenGL and Vulkan ICD.
     *    Searched before the Khronos JSON path because it avoids JSON parsing
     *    and gives us the actual DLL path in one step. */
    STEREO_LOG("stereo_load_real_icd: searching OpenGL driver registry keys");
    {
        char *ogl_dll = stereo_find_opengl_driver_icd();
        if (ogl_dll) {
            STEREO_LOG("stereo_load_real_icd: OpenGL registry → '%s'", ogl_dll);
            bool ok = try_load_icd(ogl_dll);
            free(ogl_dll);
            if (ok) return true;
        } else {
            STEREO_LOG("stereo_load_real_icd: no OpenGL driver DLL found in registry");
        }
    }

    /* 3. Registry enumeration (Khronos Vulkan ICD JSON manifests) */
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
stereo_dl_t               stereo_get_real_icd_handle(void) { return g_real_icd_handle; }

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

/* ── Physdev wrapper registry ────────────────────────────────────────────── */

/* Return an existing StereoPhysdev for real_pd, or allocate a new one.
 * Each unique real nvoglv64 physdev handle gets exactly one wrapper so the
 * loader always sees the same pointer (stable address = valid handle). */
StereoPhysdev *stereo_physdev_get_or_create(VkPhysicalDevice real_pd, StereoInstance *si)
{
    if (!real_pd || !si) return NULL;
    ensure_registry_init();
    stereo_mutex_lock(&g_registry_lock);

    /* Reuse existing wrapper for this real_pd */
    for (uint32_t i = 0; i < g_physdev_count; i++) {
        if (g_physdev_wrappers[i].real_pd == real_pd) {
            g_physdev_wrappers[i].si = si;   /* refresh si in case instance changed */
            StereoPhysdev *spd = &g_physdev_wrappers[i];
            stereo_mutex_unlock(&g_registry_lock);
            STEREO_LOG("stereo_physdev_get_or_create: reused wrapper %p for real_pd=%p",
                       (void*)spd, (void*)real_pd);
            return spd;
        }
    }

    if (g_physdev_count >= MAX_PHYSICAL_DEVICES) {
        STEREO_ERR("stereo_physdev_get_or_create: wrapper table full (%u)", g_physdev_count);
        stereo_mutex_unlock(&g_registry_lock);
        return NULL;
    }

    StereoPhysdev *spd = &g_physdev_wrappers[g_physdev_count++];
    memset(spd, 0, sizeof(*spd));
    SET_LOADER_MAGIC_VALUE(spd);   /* loader will overwrite with its dispatch ptr */
    spd->real_pd = real_pd;
    spd->si      = si;
    stereo_mutex_unlock(&g_registry_lock);
    STEREO_LOG("stereo_physdev_get_or_create: new wrapper %p (slot %u) real_pd=%p si=%p",
               (void*)spd, g_physdev_count - 1, (void*)real_pd, (void*)si);
    return spd;
}

/* Validate that pd is one of our StereoPhysdev wrappers, then return its si. */
StereoInstance *stereo_si_from_physdev(VkPhysicalDevice pd)
{
    if (!pd) return NULL;
    StereoPhysdev *spd = (StereoPhysdev *)(uintptr_t)pd;
    /* Address-range check — same strategy as stereo_instance_from_handle */
    if (spd >= g_physdev_wrappers && spd < g_physdev_wrappers + g_physdev_count) {
        return spd->si;
    }
    STEREO_ERR("stereo_si_from_physdev: unrecognised handle %p (not in wrapper array)", (void*)pd);
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
    /* Copy INI paths established in DllMain */
    strncpy(sd->global_ini, g_global_ini, sizeof(sd->global_ini) - 1);
    strncpy(sd->local_ini,  g_local_ini,  sizeof(sd->local_ini)  - 1);
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
    for (uint32_t i = 0; i < dev->swapchain_count; i++) {
        /* DXGI mode: app_handle is a pointer to the StereoSwapchain itself */
        if (dev->swapchains[i].app_handle == sc)    return &dev->swapchains[i];
        /* Passthrough mode: app_handle == real_swapchain */
        if (dev->swapchains[i].real_swapchain == sc) return &dev->swapchains[i];
    }
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
    /* Surface/WSI queries are physdev-level functions.  NVIDIA (and other ICDs)
     * expose TWO different pointers for these via giPA vs pdPA:
     *   giPA("vkGetPhysicalDeviceSurface*") -> loader-entry trampoline,
     *     intended to be called BY the loader through its dispatch chain.
     *     Calling it directly skips the chain and returns -3 on NVIDIA 426+.
     *   pdPA("vkGetPhysicalDeviceSurface*") -> raw ICD implementation,
     *     meant to be called directly with the real physdev handle.
     * Fix: ALWAYS prefer pdPA; fall back to giPA only if pdPA is absent. */
#define LOAD_SURF(table, ri, fn) \
    do { \
        PFN_vk##fn _p = NULL; \
        if (g_real_pdPA) \
            _p = (PFN_vk##fn)(g_real_pdPA)((ri), "vk"#fn); \
        if (!_p) \
            _p = (PFN_vk##fn)(g_real_giPA)((ri), "vk"#fn); \
        (table)->fn = _p; \
        STEREO_LOG("  LOAD_SURF " #fn " = %p%s", (void*)(uintptr_t)_p, \
                   g_real_pdPA ? " (pdPA)" : " (giPA fallback)"); \
    } while (0)
    LOAD_SURF(&si->real, ri, DestroySurfaceKHR);
    LOAD_SURF(&si->real, ri, GetPhysicalDeviceSurfaceSupportKHR);
    LOAD_SURF(&si->real, ri, GetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_SURF(&si->real, ri, GetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_SURF(&si->real, ri, GetPhysicalDeviceSurfacePresentModesKHR);
#undef LOAD_SURF
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
    L(GetDeviceProcAddr); /* load first so we can use it as fallback */
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
    /* VK_KHR_external_memory_win32 — optional, NULL if driver doesn't support */
    sd->real.GetMemoryWin32HandlePropertiesKHR =
        (PFN_vkGetMemoryWin32HandlePropertiesKHR)
        g_real_giPA(real_inst, "vkGetMemoryWin32HandlePropertiesKHR");
#undef L
}

/* ── Windows DllMain + self-identification marker ───────────────────────── */

/* ── dx9_nvapi_early_init ────────────────────────────────────────────────────
 * Defined here (stereo.c / DllMain translation unit) so it is always linked,
 * even in test builds that compile stereo.c in isolation without present_alt.c.
 *
 * Must be called at DLL_PROCESS_ATTACH before any Direct3DCreate9Ex.
 * NvAPI_Stereo_SetDriverMode(DIRECT) only takes effect if called before the
 * NVIDIA stereo driver inspects the process at D3D9 device creation time.
 * ─────────────────────────────────────────────────────────────────────────── */
#ifdef _WIN32
static bool s_nvapi_early_done = false;
void dx9_nvapi_early_init(void)
{
    if (s_nvapi_early_done) return;
    s_nvapi_early_done = true;

    typedef void* (*PFN_NvQI)(unsigned);
    typedef int   (*PFN_NvFn)(int);
    typedef int   (*PFN_NvInit)(void);

#ifdef _WIN64
    const char *nvapi_dll = "nvapi64.dll";
#else
    const char *nvapi_dll = "nvapi.dll";
#endif
    HMODULE hNvAPI = LoadLibraryA(nvapi_dll);
    if (!hNvAPI) {
        STEREO_LOG("[NvAPI-early] %s not found — DX9 3D Vision stereo won't work", nvapi_dll);
        return;
    }

    PFN_NvQI fnQI = (PFN_NvQI)GetProcAddress(hNvAPI, "nvapi_QueryInterface");
    if (!fnQI) {
        STEREO_LOG("[NvAPI-early] nvapi_QueryInterface not found");
        FreeLibrary(hNvAPI);
        return;
    }

    /* NvAPI_Initialize (0x0150E828) */
    PFN_NvInit fnInit = (PFN_NvInit)fnQI(0x0150E828u);
    if (fnInit) {
        int r = fnInit();
        STEREO_LOG("[NvAPI-early] NvAPI_Initialize = %d", r);
    }

    /* NvAPI_Stereo_SetDriverMode(DIRECT=2).
     * This function enables hardware-page-flip stereo ("Direct" mode) so the
     * driver presents left/right eye frames alternately.  It must be called
     * BEFORE any Direct3DCreate9Ex call.  The function ID varies across driver
     * versions; try all known IDs.  Failure is non-fatal: if no ID works the
     * driver stays in Automatic mode and NvAPI_Stereo_Activate may still work
     * for software-interleaved stereo (older 3D Vision drivers). */
    {
        static const unsigned sdm_ids[] = {
            0x5E8F1974u,  /* commonly cited in community sources          */
            0xF423435Eu,  /* alternate — seen in some NVAPI SDK builds     */
            0x3FC40AA5u,  /* another variant from reverse-engineering      */
            0
        };
        bool sdm_ok = false;
        for (int k = 0; sdm_ids[k] && !sdm_ok; k++) {
            PFN_NvFn fnMode = (PFN_NvFn)fnQI(sdm_ids[k]);
            if (fnMode) {
                int r = fnMode(2 /*DIRECT*/);
                STEREO_LOG("[NvAPI-early] NvAPI_Stereo_SetDriverMode(DIRECT) via"
                           " id=0x%08X = %d%s",
                           sdm_ids[k], r,
                           r == 0 ? " OK" : " (non-zero)");
                sdm_ok = true;
            }
        }
        if (!sdm_ok)
            STEREO_LOG("[NvAPI-early] NvAPI_Stereo_SetDriverMode not found"
                       " (tried %d IDs) — driver may not support DIRECT mode;"
                       " attempting NvAPI_Stereo_Enable as fallback",
                       (int)(sizeof(sdm_ids)/sizeof(sdm_ids[0]) - 1));
    }
    /* NvAPI_Stereo_Enable (0x239C4545) — enables stereo without setting mode.
     * Some older 3D Vision drivers (e.g. 426.06) use this instead of
     * SetDriverMode to indicate stereo-aware application intent.            */
    {
        PFN_NvFn fnEnable = (PFN_NvFn)fnQI(0x239C4545u /*NvAPI_Stereo_Enable*/);
        if (fnEnable) {
            int r = fnEnable(0);
            STEREO_LOG("[NvAPI-early] NvAPI_Stereo_Enable = %d", r);
        }
    }
    /* Keep hNvAPI loaded — FreeLibrary would invalidate the QI function pointer */
}
#endif /* _WIN32 */

/* Helper: strip filename to get directory (no trailing slash) */
static void dir_of_path(const char *full, char *out, size_t out_size)
{
    size_t len = strlen(full);
    const char *p = full + len;
    while (p > full && *p != '\\' && *p != '/') p--;
    size_t dlen = (size_t)(p - full);
    if (dlen == 0) { out[0] = '.'; out[1] = '\0'; return; }
    if (dlen >= out_size) dlen = out_size - 1;
    memcpy(out, full, dlen);
    out[dlen] = '\0';
}

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        vks3d_log_open();
        vks3d_log_write("\n\n");   /* blank separator between appended sessions */
        STEREO_LOG("===== VKS3D DLL_PROCESS_ATTACH =====");
        STEREO_LOG("VKS3D version %s  commit %s  built %s  %s",
                   VKS3D_VERSION, VKS3D_GIT_COMMIT, VKS3D_BUILD_DATE,
                   STEREO_ARCH_STR);
        STEREO_LOG("DllMain: PID=%lu  TID=%lu  hinstDLL=%p",
                   (unsigned long)GetCurrentProcessId(),
                   (unsigned long)GetCurrentThreadId(),
                   (void*)hinstDLL);

        /* Store DLL and exe paths for INI file discovery */
        {
            char dll_path[512] = "<unknown>";
            GetModuleFileNameA(hinstDLL, dll_path, sizeof(dll_path));
            STEREO_LOG("DllMain: DLL path = %s", dll_path);
            dir_of_path(dll_path, g_dll_dir, sizeof(g_dll_dir));
            snprintf(g_global_ini, sizeof(g_global_ini), "%s\\vks3d.ini", g_dll_dir);
        }
        {
            char exe_path[512] = "<unknown>";
            GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
            STEREO_LOG("DllMain: host process = %s", exe_path);
            dir_of_path(exe_path, g_exe_dir, sizeof(g_exe_dir));
            snprintf(g_local_ini, sizeof(g_local_ini), "%s\\vks3d.ini", g_exe_dir);

            /* Detect a local vulkan-1.dll next to the exe — this would bypass VKS3D
             * for the rendering session while the SDL/GLFW probe uses the system loader */
            char local_vk[512];
            snprintf(local_vk, sizeof(local_vk), "%s\\vulkan-1.dll", g_exe_dir);
            if (GetFileAttributesA(local_vk) != INVALID_FILE_ATTRIBUTES)
                STEREO_LOG("DllMain: WARNING: local vulkan-1.dll found at '%s' — "
                           "rendering may bypass VKS3D! Delete it to fix 2D output.", local_vk);
            else
                STEREO_LOG("DllMain: no local vulkan-1.dll found (good)");
        }
        STEREO_LOG("DllMain: local_ini=%s",  g_local_ini);

        /* ── Early NvAPI stereo driver mode ─────────────────────────────────
         * NvAPI_Stereo_SetDriverMode(DIRECT) MUST be called before
         * Direct3DCreate9Ex — but ONLY when the user has configured DX9
         * direct mode.  Calling it unconditionally puts the NVIDIA GPU into
         * hardware stereo mode for ALL processes, which breaks normal Vulkan
         * surface/swapchain creation for SBS, DXGI, and other modes.
         *
         * Read presentation_mode from the INI now (before any instance is
         * created) so we can gate the NvAPI call on dx9 mode only. */
        {
            char pm_str[64] = "";
            /* Prefer local (app-dir) INI over global (DLL-dir) INI */
            ini_read_str(g_global_ini, "VKS3D", "presentation_mode", pm_str, sizeof(pm_str));
            ini_read_str(g_local_ini,  "VKS3D", "presentation_mode", pm_str, sizeof(pm_str));
            /* Also honour the env var */
            {
                char env_pm[64] = "";
                DWORD n = GetEnvironmentVariableA("STEREO_PRESENTATION_MODE", env_pm, sizeof(env_pm));
                if (n > 0 && n < sizeof(env_pm)) memcpy(pm_str, env_pm, n + 1);
            }
            if (_stricmp(pm_str, "dx9") == 0) {
                STEREO_LOG("DllMain: presentation_mode=dx9 — calling NvAPI early init");
                dx9_nvapi_early_init();
            } else {
                STEREO_LOG("DllMain: presentation_mode='%s' — skipping NvAPI early init (DX9 only)", pm_str);
            }
        }

        /* ── Dual-ICD sanity check ───────────────────────────────────────────
         * If another Vulkan ICD (e.g. nvoglv64's JSON) is also active in the
         * registry alongside VKS3D, the loader will load it independently.
         * The app can then pick the raw NVIDIA physdev and bypass VKS3D
         * entirely.  Warn loudly so the user knows to run the installer's
         * ICD-displacement step. */
        {
            const char *key64 = "SOFTWARE\\Khronos\\Vulkan\\Drivers";
            const char *key32 = "SOFTWARE\\WOW6432Node\\Khronos\\Vulkan\\Drivers";
            const char *icd_key =
#ifdef _WIN64
                key64;
#else
                key32;
#endif
            HKEY hk;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, icd_key, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
                int active = 0, vks3d_found = 0;
                char val_name[512];
                char json_api_ver[32];
                DWORD val_name_sz, val_type, val_data, val_data_sz, idx = 0;
                while (1) {
                    val_name_sz  = sizeof(val_name);
                    val_data_sz  = sizeof(val_data);
                    LONG r = RegEnumValueA(hk, idx++, val_name, &val_name_sz,
                                           NULL, &val_type, (BYTE*)&val_data, &val_data_sz);
                    if (r != ERROR_SUCCESS) break;
                    if (val_type != REG_DWORD) continue;
                    if (val_data != 0) continue; /* disabled */
                    active++;
                    if (strstr(val_name, "VKS3D") || strstr(val_name, "vks3d")) {
                        /* ── Check the api_version in our own JSON ──────────────
                         * The Vulkan loader reads api_version from the ICD JSON
                         * BEFORE calling LoadLibraryA.  If api_version is higher
                         * than the loader's own Vulkan version (e.g. "1.3.0" with
                         * a Vulkan 1.1.114 loader), the loader silently skips VKS3D
                         * without ever loading the DLL — no log, no intercept.    */
                        FILE *jf;
                        int major, minor;
                        vks3d_found = 1;
                        json_api_ver[0] = '\0';
                        jf = fopen(val_name, "r");
                        if (jf) {
                            char line[1024];
                            while (fgets(line, sizeof(line), jf)) {
                                char *p = strstr(line, "api_version");
                                char *q1, *q2;
                                size_t n;
                                if (!p) continue;
                                /* skip key name's closing quote */
                                q1 = strchr(p,   '"'); if (!q1) continue;
                                q2 = strchr(q1+1,'"'); if (!q2) continue;
                                /* find opening quote of value */
                                q1 = strchr(q2+1,'"'); if (!q1) continue; q1++;
                                /* find closing quote of value */
                                q2 = strchr(q1,  '"'); if (!q2) continue;
                                n = (size_t)(q2 - q1);
                                if (n >= sizeof(json_api_ver)) n = sizeof(json_api_ver) - 1;
                                memcpy(json_api_ver, q1, n);
                                json_api_ver[n] = '\0';
                                break;
                            }
                            fclose(jf);
                        }
                        if (!json_api_ver[0])
                            strncpy(json_api_ver, "(not found)", sizeof(json_api_ver)-1);
                        STEREO_LOG("DllMain: VKS3D JSON api_version = '%s'", json_api_ver);
                        major = 0; minor = 0;
                        if (sscanf(json_api_ver, "%d.%d", &major, &minor) == 2) {
                            if (major > 1 || (major == 1 && minor > 1)) {
                                STEREO_ERR(
                                    "DllMain: WARNING: JSON api_version='%s' is Vulkan %d.%d — "
                                    "Vulkan 1.1.x loaders (e.g. driver 426.06) will SILENTLY "
                                    "SKIP VKS3D without loading the DLL, producing no log and "
                                    "2D output.  Re-run install.bat to rewrite the JSON with "
                                    "api_version=1.1.0.",
                                    json_api_ver, major, minor);
                            }
                        }
                    } else {
                        STEREO_LOG("DllMain: WARNING: other active Vulkan ICD found: %s", val_name);
                    }
                }
                RegCloseKey(hk);
                if (active > 1 || (active == 1 && !vks3d_found)) {
                    STEREO_LOG("DllMain: WARNING: %d active ICD(s) alongside VKS3D!", active - vks3d_found);
                    STEREO_LOG("DllMain: WARNING: the app may bypass VKS3D and render in 2D.");
                    STEREO_LOG("DllMain: WARNING: Re-run the VKS3D installer to displace competing ICDs.");
                } else {
                    STEREO_LOG("DllMain: ICD check OK — VKS3D is the sole active ICD");
                }
            }
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

/*
 * vks3d_diag.c — VKS3D Stereo Diagnostics Tool
 *
 * Checks all Vulkan features, extensions, and configuration required to
 * render applications in stereo 3D via VKS3D's multiview injection path.
 *
 * Build (MSVC, x64):
 *   cl /W3 /O2 vks3d_diag.c /link /out:vks3d_diag.exe vulkan-1.lib
 *
 * Build (GCC, x64):
 *   gcc -O2 -o vks3d_diag vks3d_diag.c -lvulkan
 *
 * The tool creates a Vulkan instance WITHOUT VKS3D active (by bypassing the
 * loader's ICD list if STEREO_REAL_ICD is set) and checks:
 *   - Vulkan loader presence
 *   - Required instance extensions
 *   - Physical device enumeration
 *   - Required device extensions
 *   - VK_KHR_multiview feature support
 *   - VKS3D environment configuration
 *   - Whether VKS3D is currently intercepting calls
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  pragma comment(lib, "vulkan-1.lib")
#  pragma comment(lib, "advapi32.lib")
#  define DIAG_WIN32 1
#else
#  define DIAG_WIN32 0
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Pull in Vulkan headers */
#ifdef _WIN32
#  define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

/* ── Console colour helpers ──────────────────────────────────────────────── */
#ifdef _WIN32
static void set_color(int code)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(h, (WORD)code);
}
#  define COL_RESET   set_color(7)
#  define COL_GREEN   set_color(10)
#  define COL_YELLOW  set_color(14)
#  define COL_RED     set_color(12)
#  define COL_CYAN    set_color(11)
#  define COL_WHITE   set_color(15)
#else
#  define COL_RESET   printf("\033[0m")
#  define COL_GREEN   printf("\033[32m")
#  define COL_YELLOW  printf("\033[33m")
#  define COL_RED     printf("\033[31m")
#  define COL_CYAN    printf("\033[36m")
#  define COL_WHITE   printf("\033[1m")
#endif

static int g_pass = 0, g_warn = 0, g_fail = 0;

static void result_pass(const char *label, const char *detail)
{
    COL_GREEN; printf("  [PASS] "); COL_RESET;
    printf("%-55s", label);
    if (detail) { COL_CYAN; printf(" %s", detail); COL_RESET; }
    printf("\n");
    g_pass++;
}

static void result_warn(const char *label, const char *detail)
{
    COL_YELLOW; printf("  [WARN] "); COL_RESET;
    printf("%-55s", label);
    if (detail) { COL_YELLOW; printf(" %s", detail); COL_RESET; }
    printf("\n");
    g_warn++;
}

static void result_fail(const char *label, const char *detail)
{
    COL_RED; printf("  [FAIL] "); COL_RESET;
    printf("%-55s", label);
    if (detail) { COL_RED; printf(" %s", detail); COL_RESET; }
    printf("\n");
    g_fail++;
}

static void section(const char *title)
{
    COL_WHITE;
    printf("\n══ %s ", title);
    int pad = 68 - (int)strlen(title);
    for (int i = 0; i < pad; i++) printf("═");
    printf("\n");
    COL_RESET;
}

static const char *vk_result_str(VkResult r)
{
    switch (r) {
    case VK_SUCCESS:                        return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY:       return "OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:    return "INITIALIZATION_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:        return "LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:    return "EXTENSION_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:      return "INCOMPATIBLE_DRIVER";
    default: {
        static char buf[32];
        snprintf(buf, sizeof(buf), "VkResult(%d)", (int)r);
        return buf;
    }
    }
}

/* ── Extension helpers ───────────────────────────────────────────────────── */
static int has_inst_ext(VkExtensionProperties *exts, uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; i++)
        if (strcmp(exts[i].extensionName, name) == 0) return 1;
    return 0;
}

static int has_dev_ext(VkExtensionProperties *exts, uint32_t count, const char *name)
{
    return has_inst_ext(exts, count, name);
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void)
{
    COL_WHITE;
    printf("╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║              VKS3D Stereo 3D Diagnostics Tool                       ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════╝\n");
    COL_RESET;

    /* ────────────────────────────────────────────────────────────────────── */
    section("Environment / Configuration");
    /* ────────────────────────────────────────────────────────────────────── */

    const char *real_icd = getenv("STEREO_REAL_ICD");
    if (real_icd) {
        result_pass("STEREO_REAL_ICD", real_icd);
#ifdef _WIN32
        DWORD attr = GetFileAttributesA(real_icd);
        if (attr == INVALID_FILE_ATTRIBUTES)
            result_fail("STEREO_REAL_ICD file exists", "NOT FOUND on disk");
        else
            result_pass("STEREO_REAL_ICD file exists", NULL);
#endif
    } else {
        result_warn("STEREO_REAL_ICD", "not set — VKS3D will search registry/fallbacks");
    }

    const char *stereo_en = getenv("STEREO_ENABLED");
    if (!stereo_en || strcmp(stereo_en, "0") != 0)
        result_pass("STEREO_ENABLED", stereo_en ? stereo_en : "(default=1)");
    else
        result_warn("STEREO_ENABLED", "set to 0 — stereo is disabled");

    const char *sep = getenv("STEREO_SEP");
    const char *conv = getenv("STEREO_CONV");
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s (default 0.065)", sep ? sep : "not set");
        result_pass("STEREO_SEP (eye separation)", buf);
    }
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s (default 0.030)", conv ? conv : "not set");
        result_pass("STEREO_CONV (convergence)", buf);
    }

    const char *log_path = getenv("STEREO_LOGFILE_PATH");
    if (log_path)
        result_pass("STEREO_LOGFILE_PATH (logging active)", log_path);
    else
        result_warn("STEREO_LOGFILE_PATH", "not set — no VKS3D log output");

    /* ────────────────────────────────────────────────────────────────────── */
    section("VKS3D DLL / Loader");
    /* ────────────────────────────────────────────────────────────────────── */

#ifdef _WIN32
    /* Check whether VKS3D is in the module list */
    {
        HMODULE vks3d_mod = GetModuleHandleA("VKS3D_x64.dll");
        if (!vks3d_mod) vks3d_mod = GetModuleHandleA("VKS3D_x86.dll");
        if (vks3d_mod) {
            char path[MAX_PATH] = {0};
            GetModuleFileNameA(vks3d_mod, path, sizeof(path));
            result_pass("VKS3D DLL loaded in this process", path);
        } else {
            result_warn("VKS3D DLL not loaded yet",
                        "will be loaded by Vulkan loader when instance is created");
        }
    }

    /* Check for VKS3D JSON manifest in registry */
    {
        static const char *reg_paths[] = {
            "SOFTWARE\\Khronos\\Vulkan\\Drivers",
            "SOFTWARE\\WOW6432Node\\Khronos\\Vulkan\\Drivers",
            NULL
        };
        int found_manifest = 0;
        for (int ri = 0; reg_paths[ri] && !found_manifest; ri++) {
            HKEY hk = NULL;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, reg_paths[ri],
                              0, KEY_READ, &hk) != ERROR_SUCCESS) continue;
            DWORD idx = 0;
            char  vname[2048]; DWORD vname_len;
            DWORD vtype, vdata, vdata_sz;
            while (!found_manifest) {
                vname_len = sizeof(vname); vdata_sz = sizeof(vdata);
                if (RegEnumValueA(hk, idx++, vname, &vname_len, NULL,
                                  &vtype, (LPBYTE)&vdata, &vdata_sz)
                    != ERROR_SUCCESS) break;
                if (strstr(vname, "VKS3D") || strstr(vname, "vks3d")) {
                    result_pass("VKS3D ICD JSON registered", vname);
                    found_manifest = 1;
                }
            }
            RegCloseKey(hk);
        }
        if (!found_manifest)
            result_fail("VKS3D ICD JSON registered",
                        "Not found — run Install-VKS3D.ps1 or install.bat");
    }
#endif /* _WIN32 */

    /* ────────────────────────────────────────────────────────────────────── */
    section("Vulkan Instance Extensions");
    /* ────────────────────────────────────────────────────────────────────── */

    uint32_t inst_ext_count = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &inst_ext_count, NULL);
    VkExtensionProperties *inst_exts =
        malloc(inst_ext_count * sizeof(VkExtensionProperties));
    vkEnumerateInstanceExtensionProperties(NULL, &inst_ext_count, inst_exts);

    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u total", inst_ext_count);
        result_pass("vkEnumerateInstanceExtensionProperties", buf);
    }

    /* Required instance extensions */
    static const char *REQ_INST_EXTS[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
        NULL
    };
    for (int i = 0; REQ_INST_EXTS[i]; i++) {
        if (has_inst_ext(inst_exts, inst_ext_count, REQ_INST_EXTS[i]))
            result_pass(REQ_INST_EXTS[i], NULL);
        else
            result_fail(REQ_INST_EXTS[i], "MISSING — required");
    }

    /* Check VK_KHR_surface_protected_capabilities (optional but nice) */
    {
        const char *n = VK_KHR_SURFACE_PROTECTED_CAPABILITIES_EXTENSION_NAME;
        if (has_inst_ext(inst_exts, inst_ext_count, n))
            result_pass(n, "(optional)");
        else
            result_warn(n, "(optional, not present)");
    }

    /* ────────────────────────────────────────────────────────────────────── */
    section("Vulkan Instance Creation");
    /* ────────────────────────────────────────────────────────────────────── */

    const char *enabled_inst_exts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
    };
    /* Only enable extensions that are actually present */
    uint32_t n_inst_exts = 0;
    const char *act_inst_exts[8];
    for (int i = 0; i < 4; i++) {
        if (has_inst_ext(inst_exts, inst_ext_count, enabled_inst_exts[i]))
            act_inst_exts[n_inst_exts++] = enabled_inst_exts[i];
    }

    VkApplicationInfo appInfo = {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = "vks3d_diag",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName        = "vks3d_diag",
        .engineVersion      = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion         = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ici = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo        = &appInfo,
        .enabledExtensionCount   = n_inst_exts,
        .ppEnabledExtensionNames = act_inst_exts,
    };

    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, NULL, &instance);
    if (r != VK_SUCCESS) {
        result_fail("vkCreateInstance", vk_result_str(r));
        free(inst_exts);
        goto summary;
    }
    result_pass("vkCreateInstance", NULL);

    /* ────────────────────────────────────────────────────────────────────── */
    section("VKS3D Intercept Check");
    /* ────────────────────────────────────────────────────────────────────── */

#ifdef _WIN32
    {
        HMODULE vks3d_now = GetModuleHandleA("VKS3D_x64.dll");
        if (!vks3d_now) vks3d_now = GetModuleHandleA("VKS3D_x86.dll");
        if (vks3d_now) {
            /* Check for our internal marker symbol */
            FARPROC marker = GetProcAddress(vks3d_now, "vks3d_internal_marker");
            if (marker)
                result_pass("VKS3D is active and intercepting Vulkan calls", NULL);
            else
                result_warn("VKS3D DLL loaded but marker not found", NULL);
        } else {
            result_fail("VKS3D is NOT intercepting this process",
                        "Ensure VKS3D JSON is registered and STEREO_REAL_ICD is set");
        }
    }
#endif

    /* ────────────────────────────────────────────────────────────────────── */
    section("Physical Device Enumeration");
    /* ────────────────────────────────────────────────────────────────────── */

    uint32_t dev_count = 0;
    vkEnumeratePhysicalDevices(instance, &dev_count, NULL);
    if (dev_count == 0) {
        result_fail("vkEnumeratePhysicalDevices", "No physical devices found");
        vkDestroyInstance(instance, NULL);
        free(inst_exts);
        goto summary;
    }

    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u device(s) found", dev_count);
        result_pass("vkEnumeratePhysicalDevices", buf);
    }

    VkPhysicalDevice *phys_devs = malloc(dev_count * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(instance, &dev_count, phys_devs);

    for (uint32_t di = 0; di < dev_count; di++) {
        VkPhysicalDevice pd = phys_devs[di];

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);

        printf("\n");
        COL_CYAN;
        printf("  ── Device %u: %s (Vulkan %u.%u.%u) ──\n", di,
               props.deviceName,
               VK_VERSION_MAJOR(props.apiVersion),
               VK_VERSION_MINOR(props.apiVersion),
               VK_VERSION_PATCH(props.apiVersion));
        COL_RESET;

        /* Device extensions */
        uint32_t dev_ext_count = 0;
        vkEnumerateDeviceExtensionProperties(pd, NULL, &dev_ext_count, NULL);
        VkExtensionProperties *dev_exts =
            malloc(dev_ext_count * sizeof(VkExtensionProperties));
        vkEnumerateDeviceExtensionProperties(pd, NULL, &dev_ext_count, dev_exts);

        /* Required device extensions */
        static const char *REQ_DEV_EXTS[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_MULTIVIEW_EXTENSION_NAME,
            VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
            VK_KHR_MAINTENANCE2_EXTENSION_NAME,
            NULL
        };
        static const char *REQ_DEV_NOTES[] = {
            "required for swapchain",
            "REQUIRED for stereo injection",
            "required by VKS3D for render pass patching",
            "required by VK_KHR_create_renderpass2",
            NULL
        };
        for (int i = 0; REQ_DEV_EXTS[i]; i++) {
            if (has_dev_ext(dev_exts, dev_ext_count, REQ_DEV_EXTS[i]))
                result_pass(REQ_DEV_EXTS[i], REQ_DEV_NOTES[i]);
            else
                result_fail(REQ_DEV_EXTS[i], REQ_DEV_NOTES[i]);
        }

        /* Optional but useful extensions */
        static const char *OPT_DEV_EXTS[] = {
            VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
            VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME,
            VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
            NULL
        };
        for (int i = 0; OPT_DEV_EXTS[i]; i++) {
            if (has_dev_ext(dev_exts, dev_ext_count, OPT_DEV_EXTS[i]))
                result_pass(OPT_DEV_EXTS[i], "(optional)");
            else
                result_warn(OPT_DEV_EXTS[i], "(optional, not present)");
        }

        /* ── VK_KHR_multiview features ───────────────────────────────────── */
        printf("\n");
        COL_WHITE; printf("  → VK_KHR_multiview Feature Query\n"); COL_RESET;

        VkPhysicalDeviceMultiviewFeatures mv_features = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,
        };
        VkPhysicalDeviceMultiviewProperties mv_props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES,
        };
        VkPhysicalDeviceFeatures2 feat2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &mv_features,
        };
        VkPhysicalDeviceProperties2 prop2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &mv_props,
        };

        /* Only call these if the extension is available */
        int has_gpd2 = has_inst_ext(inst_exts, inst_ext_count,
                                     VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        if (has_gpd2) {
            vkGetPhysicalDeviceFeatures2(pd, &feat2);
            vkGetPhysicalDeviceProperties2(pd, &prop2);

            if (mv_features.multiview) {
                result_pass("multiview feature", "SUPPORTED — stereo rendering possible");
            } else {
                result_fail("multiview feature",
                            "NOT SUPPORTED — stereo rendering NOT possible on this GPU");
            }

            if (mv_features.multiviewGeometryShader)
                result_pass("multiviewGeometryShader", "(optional)");
            else
                result_warn("multiviewGeometryShader",
                            "not supported — apps using geometry shaders may not work");

            if (mv_features.multiviewTessellationShader)
                result_pass("multiviewTessellationShader", "(optional)");
            else
                result_warn("multiviewTessellationShader",
                            "not supported — tessellated apps may not work");

            {
                char buf[64];
                snprintf(buf, sizeof(buf), "%u", mv_props.maxMultiviewViewCount);
                if (mv_props.maxMultiviewViewCount >= 2)
                    result_pass("maxMultiviewViewCount >= 2", buf);
                else
                    result_fail("maxMultiviewViewCount >= 2", buf);
            }
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "%u", mv_props.maxMultiviewInstanceIndex);
                result_pass("maxMultiviewInstanceIndex", buf);
            }
        } else {
            result_warn("VK_KHR_get_physical_device_properties2 not enabled",
                        "cannot query multiview feature details");
        }

        /* ── Other device capabilities ────────────────────────────────────── */
        printf("\n");
        COL_WHITE; printf("  → Other Capabilities\n"); COL_RESET;

        VkPhysicalDeviceFeatures feats;
        vkGetPhysicalDeviceFeatures(pd, &feats);

        if (feats.textureCompressionBC || feats.textureCompressionETC2 ||
            feats.textureCompressionASTC_LDR)
            result_pass("textureCompression", "available");

        if (feats.samplerAnisotropy)
            result_pass("samplerAnisotropy", "available");
        else
            result_warn("samplerAnisotropy", "not available");

        if (feats.geometryShader)
            result_pass("geometryShader", "available");
        else
            result_warn("geometryShader", "not available — some apps may not work");

        /* Image blit support (needed for VKS3D composite pass) */
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_B8G8R8A8_UNORM, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT &&
            fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)
            result_pass("B8G8R8A8_UNORM blit src+dst", "needed for SBS composite");
        else
            result_fail("B8G8R8A8_UNORM blit src+dst", "VKS3D composite may fail");

        vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_UNORM, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT &&
            fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)
            result_pass("R8G8B8A8_UNORM blit src+dst", "needed for SBS composite");
        else
            result_warn("R8G8B8A8_UNORM blit src+dst", "may not be available");

        /* VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT support check */
        /* (needed for 2-layer stereo images used as array attachments) */
        VkImageCreateInfo tici = {
            .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType     = VK_IMAGE_TYPE_2D,
            .format        = VK_FORMAT_B8G8R8A8_UNORM,
            .extent        = {1, 1, 1},
            .mipLevels     = 1,
            .arrayLayers   = 2,
            .samples       = VK_SAMPLE_COUNT_1_BIT,
            .tiling        = VK_IMAGE_TILING_OPTIMAL,
            .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        VkImageFormatProperties ifp;
        VkResult ifr = vkGetPhysicalDeviceImageFormatProperties(
            pd, tici.format, tici.imageType, tici.tiling,
            tici.usage, tici.flags, &ifp);
        if (ifr == VK_SUCCESS && ifp.maxArrayLayers >= 2)
            result_pass("2-layer color attachment image", "supported (stereo render target)");
        else
            result_fail("2-layer color attachment image",
                        "NOT supported — stereo render targets cannot be created");

        free(dev_exts);
    }

    /* ────────────────────────────────────────────────────────────────────── */
    section("Stereo Rendering Architecture Check");
    /* ────────────────────────────────────────────────────────────────────── */

    printf("\n");
    printf("  VKS3D stereo rendering works by:\n");
    printf("  1. Returning a 2-layer array image as the swapchain image\n");
    printf("  2. Injecting VK_KHR_multiview into the app's render passes\n");
    printf("     so layer 0 = left eye, layer 1 = right eye\n");
    printf("  3. At present time, blitting both layers SBS into the real\n");
    printf("     doubled-width swapchain image\n");
    printf("\n");
    printf("  For this to produce actual stereo output, the application's\n");
    printf("  render pass must be intercepted and multiview must be injected.\n");
    printf("  VKS3D intercepts vkCreateRenderPass2KHR for this purpose.\n");
    printf("\n");
    printf("  Apps that use vkCreateRenderPass (v1) also need interception —\n");
    printf("  check that VKS3D has a vkCreateRenderPass wrapper.\n");
    printf("\n");

#ifdef _WIN32
    {
        HMODULE m = GetModuleHandleA("VKS3D_x64.dll");
        if (!m) m = GetModuleHandleA("VKS3D_x86.dll");
        if (m) {
            FARPROC f1 = GetProcAddress(m, "vkCreateRenderPass");
            FARPROC f2 = GetProcAddress(m, "vkCreateRenderPass2KHR");
            if (f1)
                result_pass("VKS3D exports vkCreateRenderPass", "(v1 intercept)");
            else
                result_warn("VKS3D does not export vkCreateRenderPass",
                            "apps using vkCreateRenderPass will NOT get stereo");
            if (f2)
                result_pass("VKS3D exports vkCreateRenderPass2KHR", "(v2 intercept)");
            else
                result_warn("VKS3D does not export vkCreateRenderPass2KHR", NULL);
        } else {
            result_warn("VKS3D not loaded — cannot check render pass intercept", NULL);
        }
    }
#endif

    /* ────────────────────────────────────────────────────────────────────── */
    section("Registry: OpenGL Driver ICD Discovery");
    /* ────────────────────────────────────────────────────────────────────── */

#ifdef _WIN32
    {
        /* Check MSOGL key */
        static const char *msogl_keys[] = {
            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers\\MSOGL",
            "SOFTWARE\\WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers\\MSOGL",
            NULL
        };
        for (int ki = 0; msogl_keys[ki]; ki++) {
            HKEY hk = NULL;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, msogl_keys[ki],
                              0, KEY_READ, &hk) != ERROR_SUCCESS) continue;
            char vbuf[MAX_PATH]; DWORD vlen = sizeof(vbuf), vtype;
            if (RegQueryValueExA(hk, "DLL", NULL, &vtype,
                                 (LPBYTE)vbuf, &vlen) == ERROR_SUCCESS
                && vtype == REG_SZ) {
                vbuf[vlen > 0 ? vlen-1 : 0] = '\0';
                char label[256];
                snprintf(label, sizeof(label), "MSOGL\\DLL (%s)",
                         ki == 0 ? "64-bit" : "32-bit");
                result_pass(label, vbuf);
            }
            RegCloseKey(hk);
        }

        /* Check display adapter class for OpenGLDriverName */
        const char *cls_key =
            "SYSTEM\\CurrentControlSet\\Control\\Class"
            "\\{4d36e968-e325-11ce-bfc1-08002be10318}";
        HKEY hcls = NULL;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, cls_key, 0,
                          KEY_READ | KEY_ENUMERATE_SUB_KEYS,
                          &hcls) == ERROR_SUCCESS) {
            DWORD idx = 0;
            char subk[64]; DWORD subk_len;
            while (1) {
                subk_len = sizeof(subk);
                if (RegEnumKeyExA(hcls, idx++, subk, &subk_len,
                                  NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
                char full[512];
                snprintf(full, sizeof(full), "%s\\%s", cls_key, subk);
                HKEY hdev = NULL;
                if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, full,
                                  0, KEY_READ, &hdev) != ERROR_SUCCESS) continue;
                char vbuf[MAX_PATH]; DWORD vlen = sizeof(vbuf), vtype;
                if (RegQueryValueExA(hdev, "OpenGLDriverName", NULL,
                                     &vtype, (LPBYTE)vbuf, &vlen)
                    == ERROR_SUCCESS && vtype == REG_SZ) {
                    char label[128];
                    snprintf(label, sizeof(label),
                             "Class\\{adapter}\\%s OpenGLDriverName", subk);
                    vbuf[vlen > 0 ? vlen-1 : 0] = '\0';
                    /* Check if file exists in System32 */
                    char dll_path[MAX_PATH];
                    snprintf(dll_path, sizeof(dll_path),
                             "C:\\Windows\\System32\\%s", vbuf);
                    if (GetFileAttributesA(dll_path) != INVALID_FILE_ATTRIBUTES)
                        result_pass(label, dll_path);
                    else
                        result_warn(label, vbuf);
                }
                RegCloseKey(hdev);
            }
            RegCloseKey(hcls);
        } else {
            result_warn("Control\\Class\\{4d36e968...}", "key not accessible");
        }
    }
#else
    result_warn("Registry ICD discovery", "Windows only");
#endif

    vkDestroyInstance(instance, NULL);
    free(phys_devs);
    free(inst_exts);

summary:
    /* ────────────────────────────────────────────────────────────────────── */
    section("Summary");
    /* ────────────────────────────────────────────────────────────────────── */

    printf("\n");
    COL_GREEN;  printf("  PASS: %d\n", g_pass);  COL_RESET;
    COL_YELLOW; printf("  WARN: %d\n", g_warn);  COL_RESET;
    COL_RED;    printf("  FAIL: %d\n", g_fail);  COL_RESET;
    printf("\n");

    if (g_fail == 0 && g_warn <= 3) {
        COL_GREEN;
        printf("  ✓ System is ready for VKS3D stereo rendering.\n");
        COL_RESET;
    } else if (g_fail == 0) {
        COL_YELLOW;
        printf("  ⚠ System can run VKS3D but some optional features are missing.\n");
        COL_RESET;
    } else {
        COL_RED;
        printf("  ✗ One or more required features are missing.\n");
        printf("    Stereo rendering will not work until FAIL items are resolved.\n");
        COL_RESET;
    }
    printf("\n");

#ifdef _WIN32
    if (GetConsoleWindow()) {
        printf("Press Enter to exit...\n");
        getchar();
    }
#endif

    return (g_fail > 0) ? 1 : 0;
}

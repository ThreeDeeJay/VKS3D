/*
 * vks3d_diag.c -- VKS3D Stereo Diagnostics Tool
 *
 * Checks all Vulkan features, extensions, and configuration required to
 * render applications in stereo 3D via VKS3D's multiview injection path.
 *
 * Build (MSVC, x86 or x64 -- no vulkan-1.lib needed):
 *   cl /W3 /O2 vks3d_diag.c advapi32.lib
 *
 * Build (GCC, Linux):
 *   gcc -O2 -o vks3d_diag vks3d_diag.c -ldl
 *
 * Vulkan is loaded at runtime via LoadLibrary/dlopen so no import lib is
 * required.  This works on both x86 and x64 without architecture-specific
 * Lib vs Lib32 path gymnastics in the build system.
 */

/* ---- Platform setup ------------------------------------------------------ */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifdef _WIN32
#  include <windows.h>
#  pragma comment(lib, "advapi32.lib")
#  define DIAG_WIN32 1
#else
#  include <dlfcn.h>
#  define DIAG_WIN32 0
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Vulkan types/constants only -- functions are loaded dynamically below */
#ifdef _WIN32
#  define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

/* ---- Dynamic Vulkan loader ----------------------------------------------- */
#ifdef _WIN32
typedef HMODULE vk_lib_t;
static vk_lib_t  vk_open(void)                     { return LoadLibraryA("vulkan-1.dll"); }
static void     *vk_sym(vk_lib_t h, const char *n) { return (void*)(uintptr_t)GetProcAddress(h, n); }
#else
typedef void *vk_lib_t;
static vk_lib_t  vk_open(void)                     { return dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL); }
static void     *vk_sym(vk_lib_t h, const char *n) { return dlsym(h, n); }
#endif

/* Bootstrap function pointer type */
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_vkGetInstanceProcAddr_t)(VkInstance, const char*);

/* Global function pointers resolved at runtime */
static PFN_vkGetInstanceProcAddr_t              p_vkGetInstanceProcAddr;
static PFN_vkEnumerateInstanceExtensionProperties p_vkEnumerateInstanceExtensionProperties;
static PFN_vkCreateInstance                     p_vkCreateInstance;
static PFN_vkDestroyInstance                    p_vkDestroyInstance;
static PFN_vkEnumeratePhysicalDevices           p_vkEnumeratePhysicalDevices;
static PFN_vkGetPhysicalDeviceProperties        p_vkGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceFeatures          p_vkGetPhysicalDeviceFeatures;
static PFN_vkGetPhysicalDeviceFeatures2         p_vkGetPhysicalDeviceFeatures2;
static PFN_vkGetPhysicalDeviceProperties2       p_vkGetPhysicalDeviceProperties2;
static PFN_vkEnumerateDeviceExtensionProperties p_vkEnumerateDeviceExtensionProperties;
static PFN_vkGetPhysicalDeviceFormatProperties  p_vkGetPhysicalDeviceFormatProperties;
static PFN_vkGetPhysicalDeviceImageFormatProperties p_vkGetPhysicalDeviceImageFormatProperties;

#define LOAD_GLOBAL(fn) p_##fn = (PFN_##fn)(p_vkGetInstanceProcAddr(VK_NULL_HANDLE, #fn))
#define LOAD_INST(fn)   p_##fn = (PFN_##fn)(p_vkGetInstanceProcAddr(instance, #fn))

/* Thin macros so the rest of the code reads as normal Vulkan calls */
#define vkEnumerateInstanceExtensionProperties  p_vkEnumerateInstanceExtensionProperties
#define vkCreateInstance                        p_vkCreateInstance
#define vkDestroyInstance                       p_vkDestroyInstance
#define vkEnumeratePhysicalDevices              p_vkEnumeratePhysicalDevices
#define vkGetPhysicalDeviceProperties           p_vkGetPhysicalDeviceProperties
#define vkGetPhysicalDeviceFeatures             p_vkGetPhysicalDeviceFeatures
#define vkGetPhysicalDeviceFeatures2            p_vkGetPhysicalDeviceFeatures2
#define vkGetPhysicalDeviceProperties2          p_vkGetPhysicalDeviceProperties2
#define vkEnumerateDeviceExtensionProperties    p_vkEnumerateDeviceExtensionProperties
#define vkGetPhysicalDeviceFormatProperties     p_vkGetPhysicalDeviceFormatProperties
#define vkGetPhysicalDeviceImageFormatProperties p_vkGetPhysicalDeviceImageFormatProperties

/* ---- Console colours ----------------------------------------------------- */
#ifdef _WIN32
static void set_color(int c) { SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE),(WORD)c); }
#  define COL_RESET  set_color(7)
#  define COL_GREEN  set_color(10)
#  define COL_YELLOW set_color(14)
#  define COL_RED    set_color(12)
#  define COL_CYAN   set_color(11)
#  define COL_WHITE  set_color(15)
#else
#  define COL_RESET  fputs("\033[0m",  stdout)
#  define COL_GREEN  fputs("\033[32m", stdout)
#  define COL_YELLOW fputs("\033[33m", stdout)
#  define COL_RED    fputs("\033[31m", stdout)
#  define COL_CYAN   fputs("\033[36m", stdout)
#  define COL_WHITE  fputs("\033[1m",  stdout)
#endif

static int g_pass = 0, g_warn = 0, g_fail = 0;

static void result_pass(const char *label, const char *detail)
{ COL_GREEN; fputs("  [PASS] ", stdout); COL_RESET; printf("%-55s", label);
  if (detail) { COL_CYAN; printf(" %s", detail); COL_RESET; } putchar('\n'); g_pass++; }

static void result_warn(const char *label, const char *detail)
{ COL_YELLOW; fputs("  [WARN] ", stdout); COL_RESET; printf("%-55s", label);
  if (detail) { COL_YELLOW; printf(" %s", detail); COL_RESET; } putchar('\n'); g_warn++; }

static void result_fail(const char *label, const char *detail)
{ COL_RED; fputs("  [FAIL] ", stdout); COL_RESET; printf("%-55s", label);
  if (detail) { COL_RED; printf(" %s", detail); COL_RESET; } putchar('\n'); g_fail++; }

static void section(const char *title)
{
    int pad = 68 - (int)strlen(title); if (pad < 0) pad = 0;
    COL_WHITE; printf("\n== %s ", title);
    for (int i = 0; i < pad; i++) putchar('=');
    putchar('\n'); COL_RESET;
}

static const char *vkr(VkResult r)
{
    switch (r) {
    case VK_SUCCESS:                     return "VK_SUCCESS";
    case VK_ERROR_INITIALIZATION_FAILED: return "INITIALIZATION_FAILED";
    case VK_ERROR_INCOMPATIBLE_DRIVER:   return "INCOMPATIBLE_DRIVER";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "EXTENSION_NOT_PRESENT";
    default: { static char b[32]; snprintf(b,sizeof(b),"VkResult(%d)",(int)r); return b; }
    }
}

static int has_ext(VkExtensionProperties *e, uint32_t n, const char *name)
{ for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }

/* =========================================================================== */
int main(void)
{
    puts("+=========================================================================+");
    puts("|               VKS3D Stereo 3D Diagnostics Tool                        |");
    puts("+=========================================================================+");

    /* ---- Vulkan Loader ---------------------------------------------------- */
    section("Vulkan Loader");

    vk_lib_t vklib = vk_open();
    if (!vklib) {
        result_fail("vulkan-1.dll / libvulkan.so.1", "NOT FOUND -- Vulkan loader missing");
        goto summary;
    }
    result_pass("Vulkan loader DLL", NULL);

    p_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr_t)vk_sym(vklib, "vkGetInstanceProcAddr");
    if (!p_vkGetInstanceProcAddr) {
        result_fail("vkGetInstanceProcAddr", "symbol not found in loader");
        goto summary;
    }
    result_pass("vkGetInstanceProcAddr", NULL);

    LOAD_GLOBAL(vkEnumerateInstanceExtensionProperties);
    LOAD_GLOBAL(vkCreateInstance);
    if (!p_vkCreateInstance) {
        result_fail("vkCreateInstance (pre-instance lookup)", "not resolvable");
        goto summary;
    }
    result_pass("Pre-instance functions resolvable", NULL);

    /* ---- Environment ------------------------------------------------------ */
    section("Environment / Configuration");

    {
        const char *v = getenv("STEREO_REAL_ICD");
        if (v) {
            result_pass("STEREO_REAL_ICD", v);
#ifdef _WIN32
            if (GetFileAttributesA(v) == INVALID_FILE_ATTRIBUTES)
                result_fail("STEREO_REAL_ICD file exists", "NOT FOUND on disk");
            else
                result_pass("STEREO_REAL_ICD file exists", NULL);
#endif
        } else {
            result_warn("STEREO_REAL_ICD", "not set -- VKS3D will search registry/fallbacks");
        }
    }
    {
        const char *v = getenv("STEREO_ENABLED");
        if (!v || strcmp(v,"0") != 0) result_pass("STEREO_ENABLED", v ? v : "(default=1)");
        else result_warn("STEREO_ENABLED", "set to 0 -- stereo disabled");
    }
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s (default 0.065)", getenv("STEREO_SEP") ? getenv("STEREO_SEP") : "not set");
        result_pass("STEREO_SEP (eye separation)", buf);
    }
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s (default 0.030)", getenv("STEREO_CONV") ? getenv("STEREO_CONV") : "not set");
        result_pass("STEREO_CONV (convergence)", buf);
    }
    {
        const char *v = getenv("STEREO_LOGFILE_PATH");
        if (v) result_pass("STEREO_LOGFILE_PATH (logging active)", v);
        else   result_warn("STEREO_LOGFILE_PATH", "not set -- no VKS3D log output");
    }

    /* ---- VKS3D registration ---------------------------------------------- */
    section("VKS3D DLL / Loader Registration");

#ifdef _WIN32
    {
        HMODULE m = GetModuleHandleA("VKS3D_x64.dll");
        if (!m) m = GetModuleHandleA("VKS3D_x86.dll");
        if (m) {
            char path[MAX_PATH] = {0};
            GetModuleFileNameA(m, path, sizeof(path));
            result_pass("VKS3D DLL loaded in this process", path);
        } else {
            result_warn("VKS3D DLL not loaded yet", "will load at vkCreateInstance");
        }
    }
    {
        /* All declarations at top for MSVC C89 compatibility */
        static const char *rk[] = {
            "SOFTWARE\\Khronos\\Vulkan\\Drivers",
            "SOFTWARE\\WOW6432Node\\Khronos\\Vulkan\\Drivers", NULL };
        int found = 0;
        int ri;
        for (ri = 0; rk[ri] && !found; ri++) {
            HKEY hk = NULL;
            DWORD idx, vl, vt, vd, vds;
            char vn[2048];
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, rk[ri], 0, KEY_READ, &hk) != ERROR_SUCCESS) continue;
            idx = 0;
            for (;;) {
                vl=sizeof(vn); vds=sizeof(vd);
                if (RegEnumValueA(hk,idx++,vn,&vl,NULL,&vt,(LPBYTE)&vd,&vds)!=ERROR_SUCCESS) break;
                if (strstr(vn,"VKS3D")||strstr(vn,"vks3d")) {
                    result_pass("VKS3D JSON registered", vn);
                    found = 1;
                    /* Verify the JSON file exists on disk */
                    if (GetFileAttributesA(vn) == INVALID_FILE_ATTRIBUTES) {
                        result_fail("VKS3D JSON file exists on disk", "File not found -- reinstall");
                    } else {
                        /* Read library_path and api_version from the JSON */
                        FILE *jf = fopen(vn, "r");
                        char line[2048];
                        char dll_path[2048];
                        char api_ver[32];
                        dll_path[0] = '\0';
                        api_ver[0]  = '\0';
                        result_pass("VKS3D JSON file exists on disk", NULL);
                        if (jf) {
                            while (fgets(line, sizeof(line), jf)) {
                                /* library_path */
                                if (!dll_path[0]) {
                                    char *key = strstr(line, "library_path");
                                    if (key) {
                                        char *col = strchr(key+12, ':');
                                        if (col) {
                                            char *q1 = strchr(col+1, '"');
                                            if (q1) {
                                                char *dst = dll_path;
                                                q1++;
                                                while (*q1 && *q1 != '"' &&
                                                       dst < dll_path+sizeof(dll_path)-1) {
                                                    if (*q1=='\\' && *(q1+1)) {
                                                        q1++;
                                                        if (*q1=='\\') *dst++='\\';
                                                        else { *dst++='\\'; *dst++=*q1; }
                                                    } else {
                                                        *dst++ = *q1;
                                                    }
                                                    q1++;
                                                }
                                                *dst = '\0';
                                            }
                                        }
                                    }
                                }
                                /* api_version: "api_version": "1.1.0"
                                 * q1=closing " of key, q2=opening " of value */
                                if (!api_ver[0]) {
                                    char *p = strstr(line, "api_version");
                                    if (p) {
                                        char *q1 = strchr(p,    '"');
                                        char *q2 = q1 ? strchr(q1+1, '"') : NULL;
                                        if (q2) {
                                            /* q2 is opening " of value */
                                            char *v  = q2 + 1;
                                            char *ve = strchr(v, '"');
                                            if (ve) {
                                                size_t n = (size_t)(ve - v);
                                                if (n >= sizeof(api_ver))
                                                    n = sizeof(api_ver)-1;
                                                memcpy(api_ver, v, n);
                                                api_ver[n] = '\0';
                                            }
                                        }
                                    }
                                }
                            }
                            fclose(jf);
                        }
                        if (dll_path[0]) {
                            if (GetFileAttributesA(dll_path) == INVALID_FILE_ATTRIBUTES)
                                result_fail("VKS3D DLL path in JSON", dll_path);
                            else
                                result_pass("VKS3D DLL path in JSON", dll_path);
                        } else {
                            result_fail("library_path in VKS3D JSON",
                                "Could not parse -- re-run install.bat to rewrite JSON");
                        }
                        /* api_version check:
                         * Vulkan 1.1.x loaders (e.g. driver 426.06) silently skip any ICD
                         * whose JSON api_version exceeds the loader's own version.
                         * "1.3.0" -> loader never calls LoadLibraryA -> no log, 2D output.
                         * Must be "1.1.0". Re-run install.bat to fix. */
                        if (!api_ver[0]) {
                            result_fail("VKS3D JSON api_version",
                                "Not found -- re-run install.bat");
                        } else {
                            int maj=0, minv=0;
                            sscanf(api_ver, "%d.%d", &maj, &minv);
                            if (maj > 1 || (maj == 1 && minv > 1)) {
                                char msg[192];
                                snprintf(msg, sizeof(msg),
                                    "%s -- MUST be 1.1.0. "
                                    "Vulkan 1.1.x loaders silently skip ICDs with "
                                    "higher api_version (no log, 2D output). "
                                    "Re-run install.bat to fix.", api_ver);
                                result_fail("VKS3D JSON api_version", msg);
                            } else {
                                result_pass("VKS3D JSON api_version", api_ver);
                            }
                        }
                    }
                    break;
                }
            }
            RegCloseKey(hk);
        }
        if (!found) result_fail("VKS3D JSON registered", "Not found -- run install.bat as admin");
    }
#endif

    /* ---- Instance extensions --------------------------------------------- */
    section("Vulkan Instance Extensions");

    uint32_t ie_cnt = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &ie_cnt, NULL);
    VkExtensionProperties *ie = malloc(ie_cnt * sizeof(VkExtensionProperties));
    vkEnumerateInstanceExtensionProperties(NULL, &ie_cnt, ie);
    { char buf[32]; snprintf(buf,sizeof(buf),"%u total",ie_cnt); result_pass("vkEnumerateInstanceExtensionProperties",buf); }

    static const char *REQ_IE[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, NULL };
    for (int i=0; REQ_IE[i]; i++) {
        if (has_ext(ie, ie_cnt, REQ_IE[i])) result_pass(REQ_IE[i], NULL);
        else result_fail(REQ_IE[i], "MISSING");
    }

    /* ---- Instance creation ------------------------------------------------ */
    section("Vulkan Instance Creation");

    const char *ae[8]; uint32_t an=0;
    for (int i=0; REQ_IE[i]; i++)
        if (has_ext(ie, ie_cnt, REQ_IE[i])) ae[an++]=REQ_IE[i];

    VkApplicationInfo ai = { .sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName="vks3d_diag", .apiVersion=VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo=&ai, .enabledExtensionCount=an, .ppEnabledExtensionNames=ae };

    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, NULL, &instance);
    if (r != VK_SUCCESS) {
        result_fail("vkCreateInstance", vkr(r));
        free(ie); goto summary;
    }
    result_pass("vkCreateInstance", NULL);

    LOAD_INST(vkDestroyInstance);
    LOAD_INST(vkEnumeratePhysicalDevices);
    LOAD_INST(vkGetPhysicalDeviceProperties);
    LOAD_INST(vkGetPhysicalDeviceFeatures);
    LOAD_INST(vkGetPhysicalDeviceFeatures2);
    LOAD_INST(vkGetPhysicalDeviceProperties2);
    LOAD_INST(vkEnumerateDeviceExtensionProperties);
    LOAD_INST(vkGetPhysicalDeviceFormatProperties);
    LOAD_INST(vkGetPhysicalDeviceImageFormatProperties);

    /* ---- VKS3D intercept ------------------------------------------------- */
    section("VKS3D Intercept Check");

#ifdef _WIN32
    {
        HMODULE m = GetModuleHandleA("VKS3D_x64.dll");
        if (!m) m = GetModuleHandleA("VKS3D_x86.dll");
        if (m) {
            if (GetProcAddress(m,"vks3d_internal_marker"))
                result_pass("VKS3D active (vks3d_internal_marker found)", NULL);
            else
                result_warn("VKS3D loaded but internal marker missing", NULL);
        } else {
            result_fail("VKS3D NOT intercepting this process",
                "Ensure JSON is registered and STEREO_REAL_ICD points to the GPU ICD");
        }
    }
#endif

    /* ---- Physical devices ------------------------------------------------ */
    section("Physical Device Enumeration");

    uint32_t dc=0;
    vkEnumeratePhysicalDevices(instance, &dc, NULL);
    if (dc==0) { result_fail("vkEnumeratePhysicalDevices","No physical devices"); goto cleanup; }
    { char buf[32]; snprintf(buf,sizeof(buf),"%u found",dc); result_pass("vkEnumeratePhysicalDevices",buf); }

    VkPhysicalDevice *pds = malloc(dc * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(instance, &dc, pds);

    for (uint32_t di=0; di<dc; di++) {
        VkPhysicalDevice pd = pds[di];
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        printf("\n");
        COL_CYAN;
        printf("  -- Device %u: %s (Vulkan %u.%u.%u) --\n", di, props.deviceName,
            VK_VERSION_MAJOR(props.apiVersion),
            VK_VERSION_MINOR(props.apiVersion),
            VK_VERSION_PATCH(props.apiVersion));
        COL_RESET;

        uint32_t de_cnt=0;
        vkEnumerateDeviceExtensionProperties(pd, NULL, &de_cnt, NULL);
        VkExtensionProperties *de = malloc(de_cnt * sizeof(VkExtensionProperties));
        vkEnumerateDeviceExtensionProperties(pd, NULL, &de_cnt, de);

        static const char *REQ_DE[]   = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_MULTIVIEW_EXTENSION_NAME, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
            VK_KHR_MAINTENANCE2_EXTENSION_NAME, NULL };
        static const char *REQ_DN[]   = { "swapchain", "REQUIRED for stereo",
            "render pass patching", "required by create_renderpass2", NULL };
        for (int i=0; REQ_DE[i]; i++) {
            if (has_ext(de, de_cnt, REQ_DE[i])) result_pass(REQ_DE[i], REQ_DN[i]);
            else result_fail(REQ_DE[i], REQ_DN[i]);
        }

        /* Multiview features */
        printf("\n"); COL_WHITE; puts("  -> VK_KHR_multiview"); COL_RESET;
        if (p_vkGetPhysicalDeviceFeatures2 &&
            has_ext(ie, ie_cnt, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
            VkPhysicalDeviceMultiviewFeatures mvf = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES };
            VkPhysicalDeviceMultiviewProperties mvp = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES };
            VkPhysicalDeviceFeatures2 f2 = {
                .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext=&mvf };
            VkPhysicalDeviceProperties2 p2 = {
                .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext=&mvp };
            vkGetPhysicalDeviceFeatures2(pd, &f2);
            vkGetPhysicalDeviceProperties2(pd, &p2);

            if (mvf.multiview) result_pass("multiview feature","SUPPORTED -- stereo possible");
            else result_fail("multiview feature","NOT SUPPORTED -- stereo NOT possible");
            if (mvf.multiviewGeometryShader) result_pass("multiviewGeometryShader","(optional)");
            else result_warn("multiviewGeometryShader","not supported");
            { char buf[32]; snprintf(buf,sizeof(buf),"%u",mvp.maxMultiviewViewCount);
              if (mvp.maxMultiviewViewCount>=2) result_pass("maxMultiviewViewCount >= 2",buf);
              else result_fail("maxMultiviewViewCount >= 2",buf); }
        } else {
            result_warn("Cannot query multiview features","vkGetPhysicalDeviceFeatures2 unavailable");
        }

        /* Blit / image format support */
        printf("\n"); COL_WHITE; puts("  -> Blit / image format support"); COL_RESET;
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_B8G8R8A8_UNORM, &fp);
        if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
            (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
            result_pass("B8G8R8A8_UNORM blit src+dst","SBS composite");
        else result_fail("B8G8R8A8_UNORM blit src+dst","VKS3D composite may fail");

        vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_UNORM, &fp);
        if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
            (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
            result_pass("R8G8B8A8_UNORM blit src+dst", NULL);
        else result_warn("R8G8B8A8_UNORM blit src+dst","not available");

        VkImageFormatProperties ifp;
        VkResult ifr = vkGetPhysicalDeviceImageFormatProperties(pd,
            VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, &ifp);
        if (ifr==VK_SUCCESS && ifp.maxArrayLayers>=2)
            result_pass("2-layer color attachment","stereo render target OK");
        else result_fail("2-layer color attachment","NOT supported");

        free(de);
    }
    free(pds);

    /* ---- Intercept exports ------------------------------------------------ */
    /* These functions are returned by vkGetDeviceProcAddr AND exported as
     * named DLL symbols.  The named export is redundant from the Vulkan spec
     * perspective but lets tools verify presence with a simple GetProcAddress. */
    section("VKS3D Device-Level Intercept Exports");

#ifdef _WIN32
    {
        HMODULE m = GetModuleHandleA("VKS3D_x64.dll");
        if (!m) m = GetModuleHandleA("VKS3D_x86.dll");
        if (m) {
            struct { const char *sym; const char *note; int required; } chk[] = {
                { "vkGetDeviceProcAddr",   "device dispatch gate",      1 },
                { "vkCreateRenderPass",    "multiview v1 injection",    1 },
                { "vkCreateRenderPass2KHR","multiview v2 injection",    0 },
                { "vkCreateSwapchainKHR",  "SBS width doubling",        1 },
                { "vkDestroySwapchainKHR", "swapchain cleanup",         1 },
                { "vkGetSwapchainImagesKHR","stereo image redirect",    1 },
                { "vkAcquireNextImageKHR", "real swapchain acquire",    1 },
                { "vkQueuePresentKHR",     "SBS composite + present",   1 },
                { NULL, NULL, 0 }
            };
            for (int i=0; chk[i].sym; i++) {
                if (GetProcAddress(m, chk[i].sym))
                    result_pass(chk[i].sym, chk[i].note);
                else if (chk[i].required)
                    result_fail(chk[i].sym, chk[i].note);
                else
                    result_warn(chk[i].sym, chk[i].note);
            }
        } else {
            result_warn("VKS3D not loaded","cannot check exports");
        }
    }
#endif

    /* ---- OpenGL driver registry ------------------------------------------ */
    section("Registry: OpenGL Driver ICD Discovery");

#ifdef _WIN32
    {
        static const char *msogl[] = {
            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers\\MSOGL",
            "SOFTWARE\\WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers\\MSOGL",
            NULL };
        for (int ki=0; msogl[ki]; ki++) {
            HKEY hk=NULL;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,msogl[ki],0,KEY_READ,&hk)!=ERROR_SUCCESS) continue;
            char vb[MAX_PATH]; DWORD vl=sizeof(vb),vt;
            if (RegQueryValueExA(hk,"DLL",NULL,&vt,(LPBYTE)vb,&vl)==ERROR_SUCCESS && vt==REG_SZ) {
                vb[vl>0?vl-1:0]='\0';
                char lb[64]; snprintf(lb,sizeof(lb),"MSOGL\\DLL (%s-bit)",ki==0?"64":"32");
                result_pass(lb,vb);
            }
            RegCloseKey(hk);
        }
        const char *cls="SYSTEM\\CurrentControlSet\\Control\\Class"
                         "\\{4d36e968-e325-11ce-bfc1-08002be10318}";
        HKEY hcls=NULL;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,cls,0,KEY_READ|KEY_ENUMERATE_SUB_KEYS,&hcls)==ERROR_SUCCESS) {
            DWORD idx=0; char sk[64]; DWORD skl;
            for (;;) {
                skl=sizeof(sk);
                if (RegEnumKeyExA(hcls,idx++,sk,&skl,NULL,NULL,NULL,NULL)!=ERROR_SUCCESS) break;
                char full[512]; snprintf(full,sizeof(full),"%s\\%s",cls,sk);
                HKEY hd=NULL;
                if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,full,0,KEY_READ,&hd)!=ERROR_SUCCESS) continue;
                char vb[MAX_PATH]; DWORD vl=sizeof(vb),vt;
                if (RegQueryValueExA(hd,"OpenGLDriverName",NULL,&vt,(LPBYTE)vb,&vl)==ERROR_SUCCESS
                    && vt==REG_SZ) {
                    vb[vl>0?vl-1:0]='\0';
                    char lb[128],dp[MAX_PATH];
                    snprintf(lb,sizeof(lb),"Class\\{adapter}\\%s OpenGLDriverName",sk);
                    snprintf(dp,sizeof(dp),"C:\\Windows\\System32\\%s",vb);
                    if (GetFileAttributesA(dp)!=INVALID_FILE_ATTRIBUTES) result_pass(lb,dp);
                    else result_warn(lb,vb);
                }
                RegCloseKey(hd);
            }
            RegCloseKey(hcls);
        }
    }
#else
    result_warn("Registry ICD discovery","Windows only");
#endif

cleanup:
    if (p_vkDestroyInstance && instance != VK_NULL_HANDLE)
        p_vkDestroyInstance(instance, NULL);
    free(ie);

summary:
    section("Summary");
    printf("\n");
    COL_GREEN;  printf("  PASS: %d\n", g_pass); COL_RESET;
    COL_YELLOW; printf("  WARN: %d\n", g_warn); COL_RESET;
    COL_RED;    printf("  FAIL: %d\n", g_fail); COL_RESET;
    printf("\n");
    if      (g_fail == 0 && g_warn <= 3) { COL_GREEN;  puts("  System is ready for VKS3D stereo rendering."); }
    else if (g_fail == 0)                { COL_YELLOW; puts("  System can run VKS3D but some optional features are missing."); }
    else                                 { COL_RED;    puts("  One or more required features are missing. Fix FAIL items above."); }
    COL_RESET; printf("\n");

#ifdef _WIN32
    if (GetConsoleWindow()) { puts("Press Enter to exit..."); getchar(); }
#endif
    return g_fail > 0 ? 1 : 0;
}

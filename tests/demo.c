\
/*
 * demo.c -- Minimal stereo ICD demo / integration test
 *
 * Headless verification that the ICD initialises, advertises
 * VK_KHR_multiview, and reports correct stereo configuration.
 *
 * Build:
 *   cmake -DVKS3D_BUILD_DEMO=ON ..
 *   cmake --build .
 *
 * Run (Linux):
 *   export STEREO_REAL_ICD=/usr/lib/x86_64-linux-gnu/libvulkan_intel.so
 *   ./VKS3D_demo
 *
 * Run (Windows):
 *   set STEREO_REAL_ICD=C:\Windows\System32\...\nvoglv64.dll
 *   VKS3D_demo.exe
 */

/* _putenv_s / _putenv are in <stdlib.h> on MSVC */
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>  /* SetEnvironmentVariableA */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

/* Cross-platform setenv -- only sets if value is not already in the environment */
static void setenv_if_unset(const char *name, const char *value)
{
    if (getenv(name)) return;  /* already set */
#ifdef _WIN32
    SetEnvironmentVariableA(name, value);
#else
    setenv(name, value, 0);  /* 0 = don't overwrite, but we already checked */
#endif
}

#define VK_CHECK(expr)                                                    \
    do {                                                                   \
        VkResult _r = (expr);                                              \
        if (_r != VK_SUCCESS) {                                            \
            fprintf(stderr, "Vulkan error %d at %s:%d -- " #expr "\n",    \
                    (int)_r, __FILE__, __LINE__);                          \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

static const char *device_type_str(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "Discrete GPU";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "Integrated GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "Virtual GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU (software)";
    default:                                     return "Unknown";
    }
}

/* Helper: return env var value or fallback string */
static const char *getenv_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return v ? v : fallback;
}

int main(void)
{
    printf("\n");
    printf("+==============================================================+\n");
    printf("|       Stereo VK ICD (VKS3D) -- Integration Demo             |\n");
    printf("+==============================================================+\n\n");

    /* Point the loader at our ICD JSON if the caller hasn't specified one */
#ifdef STEREO_ICD_JSON_PATH
    setenv_if_unset("VK_ICD_FILENAMES", STEREO_ICD_JSON_PATH);
#endif

    /* Print active stereo config */
    printf("Stereo config:\n");
    printf("  STEREO_ENABLED     = %s\n", getenv_or("STEREO_ENABLED",     "1 (default)"));
    printf("  STEREO_SEP         = %s\n", getenv_or("STEREO_SEP",         "0.065 (default)"));
    printf("  STEREO_CONV        = %s\n", getenv_or("STEREO_CONV",        "0.030 (default)"));
    printf("  STEREO_REAL_ICD    = %s\n\n", getenv_or("STEREO_REAL_ICD", "(auto-detect)"));

    /* Enumerate instance version */
    uint32_t api_ver = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&api_ver);
    printf("Vulkan API version: %u.%u.%u\n\n",
           VK_VERSION_MAJOR(api_ver),
           VK_VERSION_MINOR(api_ver),
           VK_VERSION_PATCH(api_ver));

    /* Enumerate instance extensions */
    uint32_t ext_count = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &ext_count, NULL);
    VkExtensionProperties *exts = malloc(ext_count * sizeof(*exts));
    vkEnumerateInstanceExtensionProperties(NULL, &ext_count, exts);

    int has_gpd2 = 0;
    printf("Instance extensions (%u):\n", ext_count);
    for (uint32_t i = 0; i < ext_count; i++) {
        printf("  %s\n", exts[i].extensionName);
        if (strcmp(exts[i].extensionName,
                   "VK_KHR_get_physical_device_properties2") == 0)
            has_gpd2 = 1;
    }
    free(exts);
    printf("\n");

    /* Create instance */
    VkApplicationInfo app = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL,
        "VKS3D Demo", VK_MAKE_VERSION(1, 0, 0),
        "VKS3D",      VK_MAKE_VERSION(1, 0, 0),
        VK_API_VERSION_1_1
    };

    const char *inst_exts[1];
    uint32_t    inst_ext_count = 0;
    if (has_gpd2)
        inst_exts[inst_ext_count++] = "VK_KHR_get_physical_device_properties2";

    VkInstanceCreateInfo ici = {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0,
        &app, 0, NULL, inst_ext_count, inst_exts
    };

    VkInstance instance;
    VkResult res = vkCreateInstance(&ici, NULL, &instance);
    if (res != VK_SUCCESS) {
        fprintf(stderr,
            "\n[FAIL] vkCreateInstance returned %d.\n"
            "  Ensure STEREO_REAL_ICD points to a valid GPU ICD, e.g.:\n"
            "  export STEREO_REAL_ICD=/usr/lib/x86_64-linux-gnu/libvulkan_intel.so\n\n",
            (int)res);
        return 1;
    }
    printf("[PASS] vkCreateInstance -- stereo ICD loaded\n\n");

    /* Load extension functions */
    PFN_vkGetPhysicalDeviceProperties2 pfnGetProps2 =
        (PFN_vkGetPhysicalDeviceProperties2)
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2");

    PFN_vkGetPhysicalDeviceFeatures2 pfnGetFeats2 =
        (PFN_vkGetPhysicalDeviceFeatures2)
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2");

    /* Enumerate physical devices */
    uint32_t pd_count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &pd_count, NULL));
    if (pd_count == 0) {
        fprintf(stderr, "No physical devices found.\n");
        return 1;
    }

    VkPhysicalDevice *pdevs = malloc(pd_count * sizeof(*pdevs));
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &pd_count, pdevs));
    printf("Physical devices (%u):\n\n", pd_count);

    for (uint32_t i = 0; i < pd_count; i++) {
        VkPhysicalDevice pd = pdevs[i];

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);

        printf("  -- Device %u: %s --\n", i, props.deviceName);
        printf("     Type   : %s\n", device_type_str(props.deviceType));
        printf("     API    : %u.%u.%u\n",
               VK_VERSION_MAJOR(props.apiVersion),
               VK_VERSION_MINOR(props.apiVersion),
               VK_VERSION_PATCH(props.apiVersion));

        /* Multiview properties */
        if (pfnGetProps2) {
            VkPhysicalDeviceMultiviewProperties mv = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES,
                NULL, 0, 0
            };
            VkPhysicalDeviceProperties2 p2 = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                &mv, {{0}}
            };
            pfnGetProps2(pd, &p2);
            printf("     maxMultiviewViewCount     : %u\n", mv.maxMultiviewViewCount);
            printf("     maxMultiviewInstanceIndex : %u\n", mv.maxMultiviewInstanceIndex);
            printf("     Stereo (2 views) ready    : %s\n\n",
                   mv.maxMultiviewViewCount >= 2 ? "[PASS] YES" : "[FAIL] NO (need >= 2)");
        }

        /* Multiview features */
        if (pfnGetFeats2) {
            VkPhysicalDeviceMultiviewFeatures mvf = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,
                NULL, VK_FALSE, VK_FALSE, VK_FALSE
            };
            VkPhysicalDeviceFeatures2 f2 = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                &mvf, {{0}}
            };
            pfnGetFeats2(pd, &f2);
            printf("     multiview                 : %s\n", mvf.multiview ? "[PASS]" : "[FAIL]");
            printf("     multiviewGeometryShader   : %s\n", mvf.multiviewGeometryShader ? "[PASS]" : "[WARN]");
            printf("     multiviewTessellation     : %s\n\n", mvf.multiviewTessellationShader ? "[PASS]" : "[WARN]");
        }

        /* Device extensions */
        uint32_t de_count = 0;
        vkEnumerateDeviceExtensionProperties(pd, NULL, &de_count, NULL);
        VkExtensionProperties *de = malloc(de_count * sizeof(*de));
        vkEnumerateDeviceExtensionProperties(pd, NULL, &de_count, de);

        int has_multiview = 0, has_swapchain = 0;
        for (uint32_t j = 0; j < de_count; j++) {
            if (strcmp(de[j].extensionName, "VK_KHR_multiview") == 0) has_multiview = 1;
            if (strcmp(de[j].extensionName, "VK_KHR_swapchain")  == 0) has_swapchain = 1;
        }
        free(de);
        printf("     VK_KHR_multiview : %s\n", has_multiview ? "[PASS]" : "[FAIL]");
        printf("     VK_KHR_swapchain : %s\n\n", has_swapchain ? "[PASS]" : "[FAIL]");

        /* Create device with multiview */
        VkPhysicalDeviceMultiviewFeatures mv_enable = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,
            NULL, VK_TRUE, VK_FALSE, VK_FALSE
        };
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci = {
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0,
            0, 1, &prio
        };
        const char *dev_ext_names[] = { "VK_KHR_multiview" };
        VkDeviceCreateInfo dci = {
            VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            has_multiview ? (const void*)&mv_enable : NULL,
            0, 1, &qci, 0, NULL,
            has_multiview ? 1u : 0u,
            has_multiview ? dev_ext_names : NULL,
            NULL
        };

        VkDevice device;
        res = vkCreateDevice(pd, &dci, NULL, &device);
        if (res != VK_SUCCESS) {
            printf("     [FAIL] vkCreateDevice returned %d -- skipping\n\n", (int)res);
            continue;
        }
        printf("     [PASS] vkCreateDevice with VK_KHR_multiview\n");

        /* Test render pass multiview injection */
        VkAttachmentDescription att = {
            0,
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_SAMPLE_COUNT_1_BIT,
            VK_ATTACHMENT_LOAD_OP_CLEAR,
            VK_ATTACHMENT_STORE_OP_STORE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        };
        VkAttachmentReference att_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass = {
            0, VK_PIPELINE_BIND_POINT_GRAPHICS,
            0, NULL, 1, &att_ref, NULL, NULL, 0, NULL
        };
        VkRenderPassCreateInfo rpci = {
            VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0,
            1, &att, 1, &subpass, 0, NULL
        };
        VkRenderPass rp;
        res = vkCreateRenderPass(device, &rpci, NULL, &rp);
        if (res == VK_SUCCESS) {
            printf("     [PASS] vkCreateRenderPass with multiview injection\n");
            vkDestroyRenderPass(device, rp, NULL);
        } else {
            printf("     [FAIL] vkCreateRenderPass returned %d\n", (int)res);
        }

        vkDestroyDevice(device, NULL);
        printf("\n");
    }

    free(pdevs);
    vkDestroyInstance(instance, NULL);

    printf("==============================================================\n");
    printf("  VKS3D stereo ICD demo complete.\n");
    printf("  SBS: app renders to 2-layer W*H image; ICD blits to 2W*H.\n");
    printf("  Left eye  -> pixels [0,   W)\n");
    printf("  Right eye -> pixels [W, 2W)\n");
    printf("==============================================================\n\n");
    return 0;
}

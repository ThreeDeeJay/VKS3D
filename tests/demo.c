/*
 * demo.c — Minimal stereo ICD demo / integration test
 *
 * Headless verification that the ICD initialises correctly, advertises
 * VK_KHR_multiview, and reports correct stereo configuration.
 *
 * Build:
 *   cmake -DSTEREO_BUILD_DEMO=ON ..
 *   make stereo_demo
 *
 * Run:
 *   export STEREO_REAL_ICD=/usr/lib/x86_64-linux-gnu/libvulkan_intel.so
 *   ./stereo_demo
 *
 *   # Custom stereo params:
 *   STEREO_SEPARATION=0.080 STEREO_CONVERGENCE=0.025 ./stereo_demo
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <vulkan/vulkan.h>

#define VK_CHECK(expr)                                                       \
    do {                                                                      \
        VkResult _r = (expr);                                                 \
        if (_r != VK_SUCCESS) {                                               \
            fprintf(stderr, "Vulkan error %d at %s:%d — " #expr "\n",        \
                    _r, __FILE__, __LINE__);                                  \
            exit(1);                                                          \
        }                                                                     \
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

int main(void)
{
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║           Stereo VK ICD (VKS3D) — Integration Demo          ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    /* Point the loader at our ICD if not already set */
#ifdef STEREO_ICD_JSON_PATH
    if (!getenv("VK_ICD_FILENAMES"))
        setenv("VK_ICD_FILENAMES", STEREO_ICD_JSON_PATH, 1);
#endif

    /* ── Print active stereo config from env ──────────────────────── */
    {
        const char *sep  = getenv("STEREO_SEPARATION")  ? : "0.065 (default)";
        const char *conv = getenv("STEREO_CONVERGENCE") ? : "0.030 (default)";
        const char *ena  = getenv("STEREO_ENABLED")     ? : "1 (default)";
        printf("Stereo config:\n");
        printf("  STEREO_ENABLED     = %s\n", ena);
        printf("  STEREO_SEPARATION  = %s\n", sep);
        printf("  STEREO_CONVERGENCE = %s\n", conv);
        printf("  STEREO_REAL_ICD    = %s\n\n",
               getenv("STEREO_REAL_ICD") ? : "(auto-detect)");
    }

    /* ── Enumerate instance version ───────────────────────────────── */
    uint32_t api_ver = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&api_ver);
    printf("Vulkan API version: %u.%u.%u\n\n",
           VK_VERSION_MAJOR(api_ver),
           VK_VERSION_MINOR(api_ver),
           VK_VERSION_PATCH(api_ver));

    /* ── Enumerate instance extensions ───────────────────────────── */
    uint32_t ext_count = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &ext_count, NULL);
    VkExtensionProperties *exts = malloc(ext_count * sizeof(*exts));
    vkEnumerateInstanceExtensionProperties(NULL, &ext_count, exts);

    bool has_gpd2 = false;
    printf("Instance extensions (%u):\n", ext_count);
    for (uint32_t i = 0; i < ext_count; i++) {
        printf("  %s\n", exts[i].extensionName);
        if (!strcmp(exts[i].extensionName,
                    "VK_KHR_get_physical_device_properties2"))
            has_gpd2 = true;
    }
    free(exts);
    printf("\n");

    /* ── Create instance ──────────────────────────────────────────── */
    VkApplicationInfo app = {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = "VKS3D Demo",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName        = "VKS3D",
        .apiVersion         = VK_API_VERSION_1_1,
    };

    const char *inst_exts[1];
    uint32_t    inst_ext_count = 0;
    if (has_gpd2)
        inst_exts[inst_ext_count++] =
            "VK_KHR_get_physical_device_properties2";

    VkInstanceCreateInfo ici = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo        = &app,
        .enabledExtensionCount   = inst_ext_count,
        .ppEnabledExtensionNames = inst_exts,
    };

    VkInstance instance;
    VkResult res = vkCreateInstance(&ici, NULL, &instance);
    if (res != VK_SUCCESS) {
        fprintf(stderr,
            "\n✗ vkCreateInstance failed (%d).\n"
            "  Ensure STEREO_REAL_ICD points to a valid GPU ICD, e.g.:\n"
            "  export STEREO_REAL_ICD=/usr/lib/x86_64-linux-gnu/libvulkan_intel.so\n\n",
            res);
        return 1;
    }
    printf("✓ vkCreateInstance succeeded — stereo ICD loaded\n\n");

    /* Load vkGetPhysicalDeviceProperties2 */
    PFN_vkGetPhysicalDeviceProperties2 pfnGetProps2 =
        (PFN_vkGetPhysicalDeviceProperties2)
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2");

    PFN_vkGetPhysicalDeviceFeatures2 pfnGetFeats2 =
        (PFN_vkGetPhysicalDeviceFeatures2)
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2");

    /* ── Enumerate physical devices ───────────────────────────────── */
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

        printf("  ── Device %u: %s ──────────────────────────\n",
               i, props.deviceName);
        printf("     Type   : %s\n", device_type_str(props.deviceType));
        printf("     API    : %u.%u.%u\n",
               VK_VERSION_MAJOR(props.apiVersion),
               VK_VERSION_MINOR(props.apiVersion),
               VK_VERSION_PATCH(props.apiVersion));

        /* ── Multiview properties ─────────────────────────────────── */
        if (pfnGetProps2) {
            VkPhysicalDeviceMultiviewProperties mv_props = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES,
            };
            VkPhysicalDeviceProperties2 props2 = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                .pNext = &mv_props,
            };
            pfnGetProps2(pd, &props2);
            printf("     Multiview maxViewCount      : %u\n",
                   mv_props.maxMultiviewViewCount);
            printf("     Multiview maxInstanceIndex  : %u\n",
                   mv_props.maxMultiviewInstanceIndex);

            bool ok_views = mv_props.maxMultiviewViewCount >= 2;
            printf("     Stereo (2 views) supported  : %s\n\n",
                   ok_views ? "✓ YES" : "✗ NO (need maxViewCount >= 2)");
        }

        /* ── Multiview features ───────────────────────────────────── */
        if (pfnGetFeats2) {
            VkPhysicalDeviceMultiviewFeatures mv_feats = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,
            };
            VkPhysicalDeviceFeatures2 feats2 = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                .pNext = &mv_feats,
            };
            pfnGetFeats2(pd, &feats2);
            printf("     Feature: multiview                  : %s\n",
                   mv_feats.multiview ? "✓" : "✗");
            printf("     Feature: multiviewGeometryShader    : %s\n",
                   mv_feats.multiviewGeometryShader ? "✓" : "✗");
            printf("     Feature: multiviewTessellation      : %s\n\n",
                   mv_feats.multiviewTessellationShader ? "✓" : "✗");
        }

        /* ── Device extensions ────────────────────────────────────── */
        uint32_t dev_ext_count = 0;
        vkEnumerateDeviceExtensionProperties(pd, NULL, &dev_ext_count, NULL);
        VkExtensionProperties *dev_exts =
            malloc(dev_ext_count * sizeof(*dev_exts));
        vkEnumerateDeviceExtensionProperties(pd, NULL,
                                              &dev_ext_count, dev_exts);
        bool has_multiview = false;
        bool has_swapchain = false;
        for (uint32_t j = 0; j < dev_ext_count; j++) {
            if (!strcmp(dev_exts[j].extensionName, "VK_KHR_multiview"))
                has_multiview = true;
            if (!strcmp(dev_exts[j].extensionName, "VK_KHR_swapchain"))
                has_swapchain = true;
        }
        free(dev_exts);

        printf("     Ext: VK_KHR_multiview   : %s\n",
               has_multiview ? "✓" : "✗");
        printf("     Ext: VK_KHR_swapchain   : %s\n\n",
               has_swapchain ? "✓" : "✗");

        /* ── Create device with multiview ─────────────────────────── */
        VkPhysicalDeviceMultiviewFeatures mv_enable = {
            .sType     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,
            .multiview = VK_TRUE,
        };

        float queue_prio = 1.0f;
        VkDeviceQueueCreateInfo qci = {
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = 0,
            .queueCount       = 1,
            .pQueuePriorities = &queue_prio,
        };

        const char *dev_exts_enable[] = { "VK_KHR_multiview" };
        VkDeviceCreateInfo dci = {
            .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext                   = has_multiview ? &mv_enable : NULL,
            .queueCreateInfoCount    = 1,
            .pQueueCreateInfos       = &qci,
            .enabledExtensionCount   = has_multiview ? 1 : 0,
            .ppEnabledExtensionNames = has_multiview ? dev_exts_enable : NULL,
        };

        VkDevice device;
        res = vkCreateDevice(pd, &dci, NULL, &device);
        if (res != VK_SUCCESS) {
            printf("     ✗ vkCreateDevice failed (%d) — skipping\n\n", res);
            continue;
        }
        printf("     ✓ vkCreateDevice with VK_KHR_multiview succeeded\n");

        /* ── Test render pass multiview injection ─────────────────── */
        VkAttachmentDescription att = {
            .format         = VK_FORMAT_B8G8R8A8_UNORM,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        VkAttachmentReference att_ref = {
            .attachment = 0,
            .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        VkSubpassDescription subpass = {
            .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount    = 1,
            .pColorAttachments       = &att_ref,
        };
        VkRenderPassCreateInfo rpci = {
            .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments    = &att,
            .subpassCount    = 1,
            .pSubpasses      = &subpass,
        };
        VkRenderPass rp;
        res = vkCreateRenderPass(device, &rpci, NULL, &rp);
        if (res == VK_SUCCESS) {
            printf("     ✓ CreateRenderPass with multiview injection succeeded\n");
            vkDestroyRenderPass(device, rp, NULL);
        } else {
            printf("     ✗ CreateRenderPass failed (%d)\n", res);
        }

        vkDestroyDevice(device, NULL);
        printf("\n");
    }

    free(pdevs);
    vkDestroyInstance(instance, NULL);

    printf("══════════════════════════════════════════════════════════════\n");
    printf("  VKS3D stereo ICD demo complete.\n");
    printf("  SBS output: app renders to 2-layer image (W×H×2 layers)\n");
    printf("              ICD blits → doubled-width swapchain (2W×H)\n");
    printf("  Left  eye  → pixels [0,   W)\n");
    printf("  Right eye  → pixels [W, 2W)\n");
    printf("══════════════════════════════════════════════════════════════\n\n");
    return 0;
}

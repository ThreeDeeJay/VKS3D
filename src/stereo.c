/*
 * stereo.c — Stereo configuration + global object registry
 *
 * Stereo math: off-axis asymmetric frustum projection
 * ─────────────────────────────────────────────────────
 *
 * For a camera with inter-pupillary distance IPD and convergence plane
 * at distance D, the correct stereo offset in CLIP SPACE is:
 *
 *   Left eye clip offset  = -(IPD/2) * (1/D) * gl_Position.w
 *   Right eye clip offset = +(IPD/2) * (1/D) * gl_Position.w
 *
 * Because we multiply by w (the homogeneous coordinate), the offset is
 * proportional to depth, which is exactly what asymmetric frustum stereo
 * requires — objects at the convergence plane (w ≈ D) shift by IPD/2,
 * objects nearer shift less, objects farther shift more.
 *
 * In our SPIR-V patch we inject:
 *   gl_Position.x += sign * separation * gl_Position.w
 *                  - sign * convergence * gl_Position.w
 *
 * where sign = -1 for left eye (view 0), +1 for right eye (view 1).
 * The convergence term pulls the frustum back toward center at depth D.
 *
 * Environment variables:
 *   STEREO_SEPARATION    — IPD in metres / clip-space (default: 0.065)
 *   STEREO_CONVERGENCE   — Convergence shift (default: 0.030)
 *   STEREO_ENABLED       — 0 to pass-through without stereo (default: 1)
 *   STEREO_REAL_ICD      — Path to real GPU ICD .so (required)
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include "stereo_icd.h"

/* ── Global object registries ────────────────────────────────────────────── */

static StereoInstance        g_instances[MAX_INSTANCES];
static uint32_t              g_instance_count = 0;

static StereoPhysicalDevice  g_physdevs[MAX_PHYSICAL_DEVICES];
static uint32_t              g_physdev_count = 0;

static StereoDevice          g_devices[MAX_DEVICES];
static uint32_t              g_device_count = 0;

static pthread_mutex_t       g_registry_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── Real ICD handle ─────────────────────────────────────────────────────── */
static void                       *g_real_icd_handle   = NULL;
static PFN_vkGetInstanceProcAddr   g_real_giPA          = NULL;
static PFN_vkGetDeviceProcAddr     g_real_gdPA          = NULL;

/* ── Stereo config ───────────────────────────────────────────────────────── */

void stereo_config_init(StereoConfig *cfg)
{
    const char *env;

    cfg->enabled = true;
    env = getenv("STEREO_ENABLED");
    if (env && atoi(env) == 0)
        cfg->enabled = false;

    cfg->separation = 0.065f;
    env = getenv("STEREO_SEPARATION");
    if (env) cfg->separation = (float)atof(env);

    cfg->convergence = 0.030f;
    env = getenv("STEREO_CONVERGENCE");
    if (env) cfg->convergence = (float)atof(env);

    cfg->flip_eyes = false;
    env = getenv("STEREO_FLIP_EYES");
    if (env && atoi(env) == 1)
        cfg->flip_eyes = true;

    stereo_config_compute_offsets(cfg);

    STEREO_LOG("Stereo config: enabled=%d separation=%.4f convergence=%.4f flip=%d",
               cfg->enabled, cfg->separation, cfg->convergence, cfg->flip_eyes);
}

void stereo_config_compute_offsets(StereoConfig *cfg)
{
    /* Net clip-space x-offset per eye:
     *   offset = ±(separation/2) - ±(convergence/2)
     * The convergence term shifts the frustum inward to reduce diplopia. */
    float half_sep  = cfg->separation  / 2.0f;
    float half_conv = cfg->convergence / 2.0f;

    if (!cfg->flip_eyes) {
        cfg->left_eye_offset  = -half_sep + half_conv;   /* view 0 */
        cfg->right_eye_offset = +half_sep - half_conv;   /* view 1 */
    } else {
        cfg->left_eye_offset  = +half_sep - half_conv;
        cfg->right_eye_offset = -half_sep + half_conv;
    }
}

/* ── Real ICD loading ────────────────────────────────────────────────────── */

bool stereo_load_real_icd(void)
{
    if (g_real_icd_handle)
        return true;

    const char *icd_path = getenv("STEREO_REAL_ICD");
    if (!icd_path) {
        /* Fallback: try common paths */
        static const char *fallbacks[] = {
            "/usr/lib/x86_64-linux-gnu/libvulkan_intel.so",
            "/usr/lib/x86_64-linux-gnu/libvulkan_radeon.so",
            "/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so",   /* lavapipe */
            "/usr/lib64/libvulkan_intel.so",
            "/usr/lib64/libvulkan_radeon.so",
            NULL
        };
        for (int i = 0; fallbacks[i]; i++) {
            g_real_icd_handle = dlopen(fallbacks[i], RTLD_NOW | RTLD_LOCAL);
            if (g_real_icd_handle) {
                STEREO_LOG("Loaded real ICD: %s", fallbacks[i]);
                break;
            }
        }
    } else {
        g_real_icd_handle = dlopen(icd_path, RTLD_NOW | RTLD_LOCAL);
        if (g_real_icd_handle)
            STEREO_LOG("Loaded real ICD: %s", icd_path);
    }

    if (!g_real_icd_handle) {
        STEREO_ERR("Failed to load real ICD. Set STEREO_REAL_ICD=/path/to/libvulkan_xxx.so");
        return false;
    }

    /* Prefer the ICD-specific entry point */
    g_real_giPA = (PFN_vkGetInstanceProcAddr)
        dlsym(g_real_icd_handle, "vk_icdGetInstanceProcAddr");
    if (!g_real_giPA) {
        g_real_giPA = (PFN_vkGetInstanceProcAddr)
            dlsym(g_real_icd_handle, "vkGetInstanceProcAddr");
    }

    if (!g_real_giPA) {
        STEREO_ERR("Real ICD has no vkGetInstanceProcAddr");
        dlclose(g_real_icd_handle);
        g_real_icd_handle = NULL;
        return false;
    }

    return true;
}

PFN_vkGetInstanceProcAddr stereo_get_real_giPA(void)
{
    return g_real_giPA;
}

/* ── Instance registry ───────────────────────────────────────────────────── */

StereoInstance *stereo_instance_alloc(void)
{
    pthread_mutex_lock(&g_registry_lock);
    if (g_instance_count >= MAX_INSTANCES) {
        pthread_mutex_unlock(&g_registry_lock);
        STEREO_ERR("Too many instances");
        return NULL;
    }
    StereoInstance *si = &g_instances[g_instance_count++];
    memset(si, 0, sizeof(*si));
    pthread_mutex_unlock(&g_registry_lock);
    return si;
}

StereoInstance *stereo_instance_from_handle(VkInstance h)
{
    pthread_mutex_lock(&g_registry_lock);
    for (uint32_t i = 0; i < g_instance_count; i++) {
        if (g_instances[i].real_instance == h) {
            pthread_mutex_unlock(&g_registry_lock);
            return &g_instances[i];
        }
    }
    pthread_mutex_unlock(&g_registry_lock);
    return NULL;
}

void stereo_instance_free(VkInstance h)
{
    pthread_mutex_lock(&g_registry_lock);
    for (uint32_t i = 0; i < g_instance_count; i++) {
        if (g_instances[i].real_instance == h) {
            g_instances[i] = g_instances[--g_instance_count];
            break;
        }
    }
    pthread_mutex_unlock(&g_registry_lock);
}

/* ── Physical device registry ────────────────────────────────────────────── */

StereoPhysicalDevice *stereo_physdev_alloc(void)
{
    pthread_mutex_lock(&g_registry_lock);
    if (g_physdev_count >= MAX_PHYSICAL_DEVICES) {
        pthread_mutex_unlock(&g_registry_lock);
        return NULL;
    }
    StereoPhysicalDevice *sp = &g_physdevs[g_physdev_count++];
    memset(sp, 0, sizeof(*sp));
    pthread_mutex_unlock(&g_registry_lock);
    return sp;
}

StereoPhysicalDevice *stereo_physdev_from_handle(VkPhysicalDevice h)
{
    pthread_mutex_lock(&g_registry_lock);
    for (uint32_t i = 0; i < g_physdev_count; i++) {
        if (g_physdevs[i].real == h) {
            pthread_mutex_unlock(&g_registry_lock);
            return &g_physdevs[i];
        }
    }
    pthread_mutex_unlock(&g_registry_lock);
    return NULL;
}

/* ── Device registry ─────────────────────────────────────────────────────── */

StereoDevice *stereo_device_alloc(void)
{
    pthread_mutex_lock(&g_registry_lock);
    if (g_device_count >= MAX_DEVICES) {
        pthread_mutex_unlock(&g_registry_lock);
        return NULL;
    }
    StereoDevice *sd = &g_devices[g_device_count++];
    memset(sd, 0, sizeof(*sd));
    pthread_mutex_init(&sd->lock, NULL);
    pthread_mutex_unlock(&g_registry_lock);
    return sd;
}

StereoDevice *stereo_device_from_handle(VkDevice h)
{
    pthread_mutex_lock(&g_registry_lock);
    for (uint32_t i = 0; i < g_device_count; i++) {
        if (g_devices[i].real_device == h) {
            pthread_mutex_unlock(&g_registry_lock);
            return &g_devices[i];
        }
    }
    pthread_mutex_unlock(&g_registry_lock);
    return NULL;
}

void stereo_device_free(VkDevice h)
{
    pthread_mutex_lock(&g_registry_lock);
    for (uint32_t i = 0; i < g_device_count; i++) {
        if (g_devices[i].real_device == h) {
            pthread_mutex_destroy(&g_devices[i].lock);
            g_devices[i] = g_devices[--g_device_count];
            break;
        }
    }
    pthread_mutex_unlock(&g_registry_lock);
}

/* ── Swapchain / RenderPass lookup helpers ─────────────────────────────── */

StereoSwapchain *stereo_swapchain_lookup(StereoDevice *dev, VkSwapchainKHR sc)
{
    for (uint32_t i = 0; i < dev->swapchain_count; i++)
        if (dev->swapchains[i].real_swapchain == sc)
            return &dev->swapchains[i];
    return NULL;
}

StereoRenderPassInfo *stereo_rp_lookup(StereoDevice *dev, VkRenderPass rp)
{
    for (uint32_t i = 0; i < dev->render_pass_count; i++)
        if (dev->render_passes[i].handle == rp)
            return &dev->render_passes[i];
    return NULL;
}

/* ── Populate real-ICD dispatch tables ──────────────────────────────────── */

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
    /* Optional debug utils */
    si->real.CreateDebugUtilsMessengerEXT  = (PFN_vkCreateDebugUtilsMessengerEXT)
        g_real_giPA(ri, "vkCreateDebugUtilsMessengerEXT");
    si->real.DestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)
        g_real_giPA(ri, "vkDestroyDebugUtilsMessengerEXT");
}

#define LOAD_DEV(table, inst, fn) \
    (table)->fn = (PFN_vk##fn)(g_real_giPA)((inst), "vk"#fn)

void stereo_populate_device_dispatch(StereoDevice *sd, VkInstance real_inst)
{
    /* Load everything through vkGetInstanceProcAddr(real_instance, ...) */
#define L(fn) LOAD_DEV(&sd->real, real_inst, fn)
    L(DestroyDevice); L(GetDeviceQueue); L(QueueSubmit); L(QueueWaitIdle);
    L(DeviceWaitIdle); L(AllocateMemory); L(FreeMemory); L(MapMemory);
    L(UnmapMemory); L(FlushMappedMemoryRanges); L(InvalidateMappedMemoryRanges);
    L(BindBufferMemory); L(BindImageMemory);
    L(GetBufferMemoryRequirements); L(GetImageMemoryRequirements);
    L(CreateFence); L(DestroyFence); L(ResetFences);
    L(GetFenceStatus); L(WaitForFences);
    L(CreateSemaphore); L(DestroySemaphore);
    L(CreateEvent); L(DestroyEvent); L(GetEventStatus); L(SetEvent); L(ResetEvent);
    L(CreateQueryPool); L(DestroyQueryPool); L(GetQueryPoolResults);
    L(CreateBuffer); L(DestroyBuffer);
    L(CreateBufferView); L(DestroyBufferView);
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
    /* Swapchain */
    L(CreateSwapchainKHR); L(DestroySwapchainKHR);
    L(GetSwapchainImagesKHR); L(AcquireNextImageKHR); L(QueuePresentKHR);
    /* RenderPass2 (optional) */
    sd->real.CreateRenderPass2KHR = (PFN_vkCreateRenderPass2KHR)
        g_real_giPA(real_inst, "vkCreateRenderPass2KHR");
#undef L
}

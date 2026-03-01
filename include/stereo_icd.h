#pragma once
/*
 * stereo_icd.h — Vulkan 1.1 Stereoscopic ICD (Installable Client Driver)
 *
 * This ICD wraps a real GPU ICD and injects:
 *   - VK_KHR_multiview (viewMask=0b11) into all render passes
 *   - SPIR-V vertex shader patching for per-eye clip-space stereo offset
 *   - Swapchain width doubling for Side-By-Side output
 *   - SBS compositing blit pass at present time
 *
 * Configuration (environment variables):
 *   STEREO_SEPARATION   — IPD/clip-space separation  (default: 0.065)
 *   STEREO_CONVERGENCE  — Convergence distance shift  (default: 0.030)
 *   STEREO_ENABLED      — 0 to disable stereo         (default: 1)
 *   STEREO_REAL_ICD     — Path to real GPU ICD .so    (required)
 *
 * Architecture:
 *   Vulkan Loader → stereo_icd.so → real GPU ICD (e.g. libvulkan_intel.so)
 */

#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

/* ── Loader/ICD interface version ─────────────────────────────────────────── */
#define STEREO_ICD_INTERFACE_VERSION 5

/* ── Maximum tracked objects ─────────────────────────────────────────────── */
#define MAX_INSTANCES           8
#define MAX_PHYSICAL_DEVICES    16
#define MAX_DEVICES             32
#define MAX_RENDER_PASSES       4096
#define MAX_IMAGES              16384
#define MAX_IMAGE_VIEWS         16384
#define MAX_FRAMEBUFFERS        4096
#define MAX_PIPELINES           4096
#define MAX_SWAPCHAINS          64
#define MAX_SHADER_MODULES      8192
#define MAX_DESCRIPTOR_SETS     16384
#define MAX_CMD_BUFFERS         65536

/* ── Stereo configuration ─────────────────────────────────────────────────── */
typedef struct StereoConfig {
    bool    enabled;
    float   separation;          /* clip-space IPD offset, half applied per eye */
    float   convergence;         /* clip-space convergence correction            */
    bool    flip_eyes;           /* swap left/right if display is mirrored       */
    /* Derived per-eye clip offsets, recomputed on config change */
    float   left_eye_offset;     /* applied to gl_Position.x for view 0         */
    float   right_eye_offset;    /* applied to gl_Position.x for view 1         */
} StereoConfig;

void stereo_config_init(StereoConfig *cfg);
void stereo_config_compute_offsets(StereoConfig *cfg);

/* ── Dispatch tables ─────────────────────────────────────────────────────── */

/* Real ICD instance-level functions */
typedef struct RealInstanceDispatch {
    PFN_vkDestroyInstance                     DestroyInstance;
    PFN_vkEnumeratePhysicalDevices            EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties         GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceProperties2        GetPhysicalDeviceProperties2;
    PFN_vkGetPhysicalDeviceFeatures           GetPhysicalDeviceFeatures;
    PFN_vkGetPhysicalDeviceFeatures2          GetPhysicalDeviceFeatures2;
    PFN_vkGetPhysicalDeviceMemoryProperties   GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties2  GetPhysicalDeviceMemoryProperties2;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties  GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 GetPhysicalDeviceQueueFamilyProperties2;
    PFN_vkGetPhysicalDeviceFormatProperties   GetPhysicalDeviceFormatProperties;
    PFN_vkGetPhysicalDeviceFormatProperties2  GetPhysicalDeviceFormatProperties2;
    PFN_vkGetPhysicalDeviceImageFormatProperties  GetPhysicalDeviceImageFormatProperties;
    PFN_vkGetPhysicalDeviceSparseImageFormatProperties GetPhysicalDeviceSparseImageFormatProperties;
    PFN_vkEnumerateDeviceExtensionProperties  EnumerateDeviceExtensionProperties;
    PFN_vkEnumerateDeviceLayerProperties      EnumerateDeviceLayerProperties;
    PFN_vkCreateDevice                        CreateDevice;
    /* Surface / WSI */
    PFN_vkDestroySurfaceKHR                   DestroySurfaceKHR;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR  GetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR  GetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR;
    /* Debug utils */
    PFN_vkCreateDebugUtilsMessengerEXT        CreateDebugUtilsMessengerEXT;
    PFN_vkDestroyDebugUtilsMessengerEXT       DestroyDebugUtilsMessengerEXT;
} RealInstanceDispatch;

/* Real ICD device-level functions */
typedef struct RealDeviceDispatch {
    PFN_vkDestroyDevice                       DestroyDevice;
    PFN_vkGetDeviceQueue                      GetDeviceQueue;
    PFN_vkQueueSubmit                         QueueSubmit;
    PFN_vkQueueWaitIdle                       QueueWaitIdle;
    PFN_vkDeviceWaitIdle                      DeviceWaitIdle;
    PFN_vkAllocateMemory                      AllocateMemory;
    PFN_vkFreeMemory                          FreeMemory;
    PFN_vkMapMemory                           MapMemory;
    PFN_vkUnmapMemory                         UnmapMemory;
    PFN_vkFlushMappedMemoryRanges             FlushMappedMemoryRanges;
    PFN_vkInvalidateMappedMemoryRanges        InvalidateMappedMemoryRanges;
    PFN_vkBindBufferMemory                    BindBufferMemory;
    PFN_vkBindImageMemory                     BindImageMemory;
    PFN_vkGetBufferMemoryRequirements         GetBufferMemoryRequirements;
    PFN_vkGetImageMemoryRequirements          GetImageMemoryRequirements;
    PFN_vkCreateFence                         CreateFence;
    PFN_vkDestroyFence                        DestroyFence;
    PFN_vkResetFences                         ResetFences;
    PFN_vkGetFenceStatus                      GetFenceStatus;
    PFN_vkWaitForFences                       WaitForFences;
    PFN_vkCreateSemaphore                     CreateSemaphore;
    PFN_vkDestroySemaphore                    DestroySemaphore;
    PFN_vkCreateEvent                         CreateEvent;
    PFN_vkDestroyEvent                        DestroyEvent;
    PFN_vkGetEventStatus                      GetEventStatus;
    PFN_vkSetEvent                            SetEvent;
    PFN_vkResetEvent                          ResetEvent;
    PFN_vkCreateQueryPool                     CreateQueryPool;
    PFN_vkDestroyQueryPool                    DestroyQueryPool;
    PFN_vkGetQueryPoolResults                 GetQueryPoolResults;
    PFN_vkCreateBuffer                        CreateBuffer;
    PFN_vkDestroyBuffer                       DestroyBuffer;
    PFN_vkCreateBufferView                    CreateBufferView;
    PFN_vkDestroyBufferView                   DestroyBufferView;
    PFN_vkCreateImage                         CreateImage;
    PFN_vkDestroyImage                        DestroyImage;
    PFN_vkGetImageSubresourceLayout           GetImageSubresourceLayout;
    PFN_vkCreateImageView                     CreateImageView;
    PFN_vkDestroyImageView                    DestroyImageView;
    PFN_vkCreateShaderModule                  CreateShaderModule;
    PFN_vkDestroyShaderModule                 DestroyShaderModule;
    PFN_vkCreatePipelineCache                 CreatePipelineCache;
    PFN_vkDestroyPipelineCache                DestroyPipelineCache;
    PFN_vkGetPipelineCacheData                GetPipelineCacheData;
    PFN_vkMergePipelineCaches                 MergePipelineCaches;
    PFN_vkCreateGraphicsPipelines             CreateGraphicsPipelines;
    PFN_vkCreateComputePipelines              CreateComputePipelines;
    PFN_vkDestroyPipeline                     DestroyPipeline;
    PFN_vkCreatePipelineLayout                CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout               DestroyPipelineLayout;
    PFN_vkCreateSampler                       CreateSampler;
    PFN_vkDestroySampler                      DestroySampler;
    PFN_vkCreateDescriptorSetLayout          CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout         DestroyDescriptorSetLayout;
    PFN_vkCreateDescriptorPool               CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool              DestroyDescriptorPool;
    PFN_vkResetDescriptorPool                ResetDescriptorPool;
    PFN_vkAllocateDescriptorSets             AllocateDescriptorSets;
    PFN_vkFreeDescriptorSets                 FreeDescriptorSets;
    PFN_vkUpdateDescriptorSets               UpdateDescriptorSets;
    PFN_vkCreateFramebuffer                  CreateFramebuffer;
    PFN_vkDestroyFramebuffer                 DestroyFramebuffer;
    PFN_vkCreateRenderPass                   CreateRenderPass;
    PFN_vkCreateRenderPass2KHR               CreateRenderPass2KHR;
    PFN_vkDestroyRenderPass                  DestroyRenderPass;
    PFN_vkGetRenderAreaGranularity           GetRenderAreaGranularity;
    PFN_vkCreateCommandPool                  CreateCommandPool;
    PFN_vkDestroyCommandPool                 DestroyCommandPool;
    PFN_vkResetCommandPool                   ResetCommandPool;
    PFN_vkAllocateCommandBuffers             AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers                 FreeCommandBuffers;
    PFN_vkBeginCommandBuffer                 BeginCommandBuffer;
    PFN_vkEndCommandBuffer                   EndCommandBuffer;
    PFN_vkResetCommandBuffer                 ResetCommandBuffer;
    /* Draw commands */
    PFN_vkCmdBindPipeline                    CmdBindPipeline;
    PFN_vkCmdSetViewport                     CmdSetViewport;
    PFN_vkCmdSetScissor                      CmdSetScissor;
    PFN_vkCmdSetLineWidth                    CmdSetLineWidth;
    PFN_vkCmdSetDepthBias                    CmdSetDepthBias;
    PFN_vkCmdSetBlendConstants               CmdSetBlendConstants;
    PFN_vkCmdSetDepthBounds                  CmdSetDepthBounds;
    PFN_vkCmdSetStencilCompareMask           CmdSetStencilCompareMask;
    PFN_vkCmdSetStencilWriteMask             CmdSetStencilWriteMask;
    PFN_vkCmdSetStencilReference             CmdSetStencilReference;
    PFN_vkCmdBindDescriptorSets              CmdBindDescriptorSets;
    PFN_vkCmdBindIndexBuffer                 CmdBindIndexBuffer;
    PFN_vkCmdBindVertexBuffers               CmdBindVertexBuffers;
    PFN_vkCmdDraw                            CmdDraw;
    PFN_vkCmdDrawIndexed                     CmdDrawIndexed;
    PFN_vkCmdDrawIndirect                    CmdDrawIndirect;
    PFN_vkCmdDrawIndexedIndirect             CmdDrawIndexedIndirect;
    PFN_vkCmdDispatch                        CmdDispatch;
    PFN_vkCmdDispatchIndirect                CmdDispatchIndirect;
    PFN_vkCmdCopyBuffer                      CmdCopyBuffer;
    PFN_vkCmdCopyImage                       CmdCopyImage;
    PFN_vkCmdBlitImage                       CmdBlitImage;
    PFN_vkCmdCopyBufferToImage               CmdCopyBufferToImage;
    PFN_vkCmdCopyImageToBuffer               CmdCopyImageToBuffer;
    PFN_vkCmdUpdateBuffer                    CmdUpdateBuffer;
    PFN_vkCmdFillBuffer                      CmdFillBuffer;
    PFN_vkCmdClearColorImage                 CmdClearColorImage;
    PFN_vkCmdClearDepthStencilImage          CmdClearDepthStencilImage;
    PFN_vkCmdClearAttachments                CmdClearAttachments;
    PFN_vkCmdResolveImage                    CmdResolveImage;
    PFN_vkCmdSetEvent                        CmdSetEvent;
    PFN_vkCmdResetEvent                      CmdResetEvent;
    PFN_vkCmdWaitEvents                      CmdWaitEvents;
    PFN_vkCmdPipelineBarrier                 CmdPipelineBarrier;
    PFN_vkCmdBeginQuery                      CmdBeginQuery;
    PFN_vkCmdEndQuery                        CmdEndQuery;
    PFN_vkCmdResetQueryPool                  CmdResetQueryPool;
    PFN_vkCmdWriteTimestamp                  CmdWriteTimestamp;
    PFN_vkCmdCopyQueryPoolResults            CmdCopyQueryPoolResults;
    PFN_vkCmdPushConstants                   CmdPushConstants;
    PFN_vkCmdBeginRenderPass                 CmdBeginRenderPass;
    PFN_vkCmdNextSubpass                     CmdNextSubpass;
    PFN_vkCmdEndRenderPass                   CmdEndRenderPass;
    PFN_vkCmdExecuteCommands                 CmdExecuteCommands;
    /* Swapchain */
    PFN_vkCreateSwapchainKHR                 CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR                DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR              GetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR                AcquireNextImageKHR;
    PFN_vkQueuePresentKHR                    QueuePresentKHR;
} RealDeviceDispatch;

/* ── Object wrappers ─────────────────────────────────────────────────────── */

/* Per-instance state */
typedef struct StereoInstance {
    VkInstance                real_instance;
    RealInstanceDispatch      real;
    StereoConfig              stereo;
    PFN_vkGetInstanceProcAddr real_get_instance_proc_addr;
} StereoInstance;

/* Per-physical-device state */
typedef struct StereoPhysicalDevice {
    VkPhysicalDevice   real;
    StereoInstance    *instance;
} StereoPhysicalDevice;

/* SBS swapchain state */
typedef struct StereoSwapchain {
    VkSwapchainKHR    real_swapchain;
    VkDevice          device;           /* wrapped device handle */
    uint32_t          app_width;        /* original app-requested width  */
    uint32_t          app_height;
    uint32_t          sbs_width;        /* actual swapchain width (2*app) */
    VkFormat          format;
    VkImage          *sbs_images;       /* the real doubled-width images  */
    uint32_t          image_count;
    /* Per-frame multiview render targets: 2-layer image arrays */
    VkImage          *stereo_images;    /* arrayLayers=2, app_width x app_height */
    VkDeviceMemory   *stereo_memory;
    VkImageView      *stereo_views_l;   /* layer 0 view (left eye)  */
    VkImageView      *stereo_views_r;   /* layer 1 view (right eye) */
    VkImageView      *stereo_views_arr; /* full array view for rendering */
    /* Composite pass resources */
    VkRenderPass      composite_renderpass;
    VkFramebuffer    *composite_framebuffers;
    VkPipeline        composite_pipeline;
    VkPipelineLayout  composite_layout;
    VkDescriptorSetLayout composite_dsl;
    VkDescriptorPool  composite_pool;
    VkDescriptorSet  *composite_desc_sets;
    VkCommandPool     composite_cmd_pool;
    VkCommandBuffer  *composite_cmds;
    VkSampler         composite_sampler;
} StereoSwapchain;

/* Per-render-pass multiview metadata */
typedef struct StereoRenderPassInfo {
    VkRenderPass  handle;
    bool          has_multiview;
    uint32_t      view_mask;            /* should be 0b11 for stereo */
    uint32_t      subpass_count;
} StereoRenderPassInfo;

/* Per-device state */
typedef struct StereoDevice {
    VkDevice               real_device;
    StereoPhysicalDevice  *phys_dev;
    RealDeviceDispatch     real;
    StereoConfig           stereo;      /* snapshot at device create time */
    /* Stereo UBO (per-eye matrices + offsets) */
    VkBuffer               stereo_ubo;
    VkDeviceMemory         stereo_ubo_mem;
    void                  *stereo_ubo_map;
    /* Tracked objects */
    StereoRenderPassInfo   render_passes[MAX_RENDER_PASSES];
    uint32_t               render_pass_count;
    StereoSwapchain        swapchains[MAX_SWAPCHAINS];
    uint32_t               swapchain_count;
    pthread_mutex_t        lock;
} StereoDevice;

/* ── Object lookup helpers ────────────────────────────────────────────────── */
StereoInstance         *stereo_instance_from_handle(VkInstance h);
StereoPhysicalDevice   *stereo_physdev_from_handle(VkPhysicalDevice h);
StereoDevice           *stereo_device_from_handle(VkDevice h);
StereoSwapchain        *stereo_swapchain_lookup(StereoDevice *dev, VkSwapchainKHR sc);
StereoRenderPassInfo   *stereo_rp_lookup(StereoDevice *dev, VkRenderPass rp);

/* ── Stereo UBO layout (matches GLSL) ─────────────────────────────────────── */
typedef struct StereoUBO {
    float eye_offset[2];      /* [0]=left offset, [1]=right offset in clip space */
    float convergence;        /* convergence term (frustum shift)                */
    float _pad;
} StereoUBO;

/* ── Forward declarations for sub-modules ────────────────────────────────── */

/* icd_main.c */
VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName);

/* instance.c */
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateInstance(
    const VkInstanceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkInstance *pInstance);
VKAPI_ATTR void VKAPI_CALL stereo_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator);
VKAPI_ATTR VkResult VKAPI_CALL stereo_EnumeratePhysicalDevices(
    VkInstance instance, uint32_t *pPhysicalDeviceCount, VkPhysicalDevice *pPhysicalDevices);
VKAPI_ATTR VkResult VKAPI_CALL stereo_EnumerateInstanceExtensionProperties(
    const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties);
VKAPI_ATTR VkResult VKAPI_CALL stereo_EnumerateInstanceVersion(uint32_t *pApiVersion);

/* device.c */
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkDevice *pDevice);
VKAPI_ATTR void VKAPI_CALL stereo_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator);

/* render_pass.c */
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateRenderPass(
    VkDevice device,
    const VkRenderPassCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkRenderPass *pRenderPass);
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateRenderPass2KHR(
    VkDevice device,
    const VkRenderPassCreateInfo2 *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkRenderPass *pRenderPass);

/* shader.c */
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateShaderModule(
    VkDevice device,
    const VkShaderModuleCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkShaderModule *pShaderModule);
VKAPI_ATTR void VKAPI_CALL stereo_DestroyShaderModule(
    VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks *pAllocator);

/* shader.c — SPIR-V patcher */
bool spirv_patch_stereo_vertex(
    const uint32_t *in_words, size_t in_count,
    uint32_t      **out_words, size_t *out_count,
    float left_offset, float right_offset, float convergence);
void spirv_patched_free(uint32_t *words);

/* swapchain.c */
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkSwapchainKHR *pSwapchain);
VKAPI_ATTR void VKAPI_CALL stereo_DestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR swapchain,
    uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages);
VKAPI_ATTR VkResult VKAPI_CALL stereo_AcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain,
    uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex);
VKAPI_ATTR VkResult VKAPI_CALL stereo_QueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR *pPresentInfo);

/* present.c — SBS composite */
VkResult stereo_composite_to_sbs(
    StereoDevice   *dev,
    VkQueue         queue,
    StereoSwapchain *sc,
    uint32_t        image_index);

/* Logging */
#define STEREO_LOG(fmt, ...) \
    fprintf(stderr, "[stereo-icd] " fmt "\n", ##__VA_ARGS__)

#define STEREO_ERR(fmt, ...) \
    fprintf(stderr, "[stereo-icd] ERROR: " fmt "\n", ##__VA_ARGS__)

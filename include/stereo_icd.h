#pragma once
/*
 * stereo_icd.h — Vulkan 1.1 Stereoscopic ICD (VKS3D)
 *
 * Supports: Windows x64, Windows x86 (WoW64), Linux x86_64, Linux x86
 *
 * This ICD wraps a real GPU ICD and injects:
 *   - VK_KHR_multiview (viewMask=0b11) into all render passes
 *   - SPIR-V vertex shader patching for per-eye clip-space stereo offset
 *   - Swapchain width doubling for Side-By-Side (SBS) output
 *   - VkCmdBlitImage composite pass at present time
 *
 * Configuration (environment variables):
 *   STEREO_SEPARATION   — IPD/clip-space separation  (default: 0.065)
 *   STEREO_CONVERGENCE  — Convergence distance shift  (default: 0.030)
 *   STEREO_ENABLED      — 0 to disable stereo         (default: 1)
 *   STEREO_FLIP_EYES    — 1 to swap left/right        (default: 0)
 *   STEREO_REAL_ICD     — Path to real GPU ICD .dll/.so  (auto-detected)
 *
 * Architecture:
 *   Vulkan Loader → VKS3D_x64.dll → real GPU ICD (nvoglv64.dll / amdvlk64.dll …)
 *   Vulkan Loader → VKS3D_x86.dll → real GPU ICD (nvoglv32.dll / amdvlk32.dll …)
 */

/* platform.h must come first — defines stereo_mutex_t, STEREO_LOG, etc. */
#include "platform.h"

/* Pull in platform-specific Vulkan extensions (Win32 surface, etc.).
 * This must be defined BEFORE vulkan.h so that vulkan.h includes
 * vulkan_win32.h, making PFN_vkCreateWin32SurfaceKHR etc. available. */
#ifdef _WIN32
#  ifndef VK_USE_PLATFORM_WIN32_KHR
#    define VK_USE_PLATFORM_WIN32_KHR
#  endif
#endif

#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

/*
 * Older Vulkan SDK releases do not define ICD_LOADER_MAGIC or
 * SET_LOADER_MAGIC_VALUE in vk_icd.h (VK_LOADER_DATA itself is present
 * in all SDK versions we care about, so we do NOT redefine that type).
 *
 * The values are part of the stable Vulkan loader ABI since Vulkan 1.0.
 */
#ifndef ICD_LOADER_MAGIC
#  define ICD_LOADER_MAGIC 0xCD1CDABA1DABADABULL
#endif

#ifndef SET_LOADER_MAGIC_VALUE
#  define SET_LOADER_MAGIC_VALUE(obj) \
    do { \
        VK_LOADER_DATA *_ld = (VK_LOADER_DATA *)(void *)(obj); \
        _ld->loaderMagic = ICD_LOADER_MAGIC; \
    } while (0)
#endif
#include <stdint.h>
#include <stdbool.h>

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
    float   left_eye_offset;     /* applied to gl_Position.x for view 0         */
    float   right_eye_offset;    /* applied to gl_Position.x for view 1         */
} StereoConfig;

void stereo_config_init(StereoConfig *cfg);
void stereo_config_compute_offsets(StereoConfig *cfg);

/* ── Dispatch tables ─────────────────────────────────────────────────────── */

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
    PFN_vkDestroySurfaceKHR                   DestroySurfaceKHR;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR  GetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR  GetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR;
    /* ── Vulkan 1.1 physdev functions ────────────────────────────────────── */
    PFN_vkGetPhysicalDeviceImageFormatProperties2   GetPhysicalDeviceImageFormatProperties2;
    PFN_vkGetPhysicalDeviceSparseImageFormatProperties2 GetPhysicalDeviceSparseImageFormatProperties2;
    PFN_vkGetPhysicalDeviceExternalBufferProperties GetPhysicalDeviceExternalBufferProperties;
    PFN_vkGetPhysicalDeviceExternalFenceProperties  GetPhysicalDeviceExternalFenceProperties;
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties GetPhysicalDeviceExternalSemaphoreProperties;
    PFN_vkEnumeratePhysicalDeviceGroups             EnumeratePhysicalDeviceGroups;
    /* ── KHR surface extensions ──────────────────────────────────────────── */
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR  GetPhysicalDeviceSurfaceCapabilities2KHR;
    PFN_vkGetPhysicalDeviceSurfaceFormats2KHR       GetPhysicalDeviceSurfaceFormats2KHR;
    PFN_vkGetPhysicalDevicePresentRectanglesKHR     GetPhysicalDevicePresentRectanglesKHR;
    /* ── Win32 ───────────────────────────────────────────────────────────── */
#ifdef VK_KHR_win32_surface
    PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR GetPhysicalDeviceWin32PresentationSupportKHR;
#endif
    /* ── EXT extensions ──────────────────────────────────────────────────── */
#ifdef VK_EXT_full_screen_exclusive
    PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT  GetPhysicalDeviceSurfacePresentModes2EXT;
#endif
#ifdef VK_EXT_calibrated_timestamps
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT GetPhysicalDeviceCalibrateableTimeDomainsEXT;
#endif
    PFN_vkGetPhysicalDeviceMultisamplePropertiesEXT GetPhysicalDeviceMultisamplePropertiesEXT;
    /* ── NV extensions ───────────────────────────────────────────────────── */
    PFN_vkGetPhysicalDeviceExternalImageFormatPropertiesNV GetPhysicalDeviceExternalImageFormatPropertiesNV;
#ifdef VK_NV_cooperative_matrix
    PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesNV GetPhysicalDeviceCooperativeMatrixPropertiesNV;
#endif
#ifdef VK_NV_coverage_reduction_mode
    PFN_vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV GetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV;
#endif
    /* NVX: stored as void* because VkDeviceGeneratedCommands* structs were
     * removed from newer SDK versions; we cast at the call site.           */
    PFN_vkVoidFunction GetPhysicalDeviceGeneratedCommandsPropertiesNVX;
    /* ── Instance-level functions (VkInstance first arg) ─────────────────── */
#ifdef VK_KHR_win32_surface
    PFN_vkCreateWin32SurfaceKHR                     CreateWin32SurfaceKHR;
#endif
    PFN_vkCreateDebugReportCallbackEXT              CreateDebugReportCallbackEXT;
    PFN_vkDestroyDebugReportCallbackEXT             DestroyDebugReportCallbackEXT;
    PFN_vkDebugReportMessageEXT                     DebugReportMessageEXT;
    PFN_vkCreateDebugUtilsMessengerEXT              CreateDebugUtilsMessengerEXT;
    PFN_vkDestroyDebugUtilsMessengerEXT             DestroyDebugUtilsMessengerEXT;
    PFN_vkSubmitDebugUtilsMessageEXT                SubmitDebugUtilsMessageEXT;
} RealInstanceDispatch;

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
    PFN_vkCreateSwapchainKHR                 CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR                DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR              GetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR                AcquireNextImageKHR;
    PFN_vkQueuePresentKHR                    QueuePresentKHR;
} RealDeviceDispatch;

/* ── Object wrappers ─────────────────────────────────────────────────────── */

typedef struct StereoInstance {
    /* VK_LOADER_DATA MUST be the very first field of every dispatchable handle
     * returned by an ICD.  The loader writes its dispatch pointer here.
     * Without it the loader corrupts whatever comes first (e.g. real_instance),
     * which makes the real ICD reject its own handle with INITIALIZATION_FAILED. */
    VK_LOADER_DATA            loader_data;
    VkInstance                real_instance;
    RealInstanceDispatch      real;
    StereoConfig              stereo;
    PFN_vkGetInstanceProcAddr real_get_instance_proc_addr;
} StereoInstance;

typedef struct StereoPhysicalDevice {
    /* VK_LOADER_DATA first — same requirement as StereoInstance */
    VK_LOADER_DATA     loader_data;
    VkPhysicalDevice   real;
    StereoInstance    *instance;
} StereoPhysicalDevice;

typedef struct StereoSwapchain {
    VkSwapchainKHR    real_swapchain;
    VkDevice          device;
    uint32_t          app_width;
    uint32_t          app_height;
    uint32_t          sbs_width;
    VkFormat          format;
    VkImage          *sbs_images;
    uint32_t          image_count;
    VkImage          *stereo_images;
    VkDeviceMemory   *stereo_memory;
    VkImageView      *stereo_views_l;
    VkImageView      *stereo_views_r;
    VkImageView      *stereo_views_arr;
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

typedef struct StereoRenderPassInfo {
    VkRenderPass  handle;
    bool          has_multiview;
    uint32_t      view_mask;
    uint32_t      subpass_count;
} StereoRenderPassInfo;

typedef struct StereoDevice {
    VkDevice               real_device;
    StereoPhysicalDevice  *phys_dev;
    RealDeviceDispatch     real;
    StereoConfig           stereo;
    VkBuffer               stereo_ubo;
    VkDeviceMemory         stereo_ubo_mem;
    void                  *stereo_ubo_map;
    StereoRenderPassInfo   render_passes[MAX_RENDER_PASSES];
    uint32_t               render_pass_count;
    StereoSwapchain        swapchains[MAX_SWAPCHAINS];
    uint32_t               swapchain_count;
    stereo_mutex_t         lock;
} StereoDevice;

/* ── Stereo UBO layout (matches GLSL std140) ─────────────────────────────── */
typedef struct StereoUBO {
    float eye_offset[2];
    float convergence;
    float _pad;
} StereoUBO;

/* ── Object lookup helpers ────────────────────────────────────────────────── */
StereoInstance         *stereo_instance_from_handle(VkInstance h);
StereoPhysicalDevice   *stereo_physdev_from_handle(VkPhysicalDevice h);
StereoPhysicalDevice   *stereo_physdev_from_real(VkPhysicalDevice real);
StereoDevice           *stereo_device_from_handle(VkDevice h);
StereoSwapchain        *stereo_swapchain_lookup(StereoDevice *dev, VkSwapchainKHR sc);
StereoRenderPassInfo   *stereo_rp_lookup(StereoDevice *dev, VkRenderPass rp);
/* Real ICD accessor functions */
PFN_vkGetInstanceProcAddr stereo_get_real_giPA(void);
PFN_vkGetInstanceProcAddr stereo_get_real_pdPA(void);
/* Object lifecycle helpers */
void                    stereo_instance_free(VkInstance h);
StereoPhysicalDevice   *stereo_physdev_alloc(void);
StereoDevice           *stereo_device_alloc(void);

/* ── Forward declarations ────────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateInstance(
    const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
VKAPI_ATTR void VKAPI_CALL stereo_DestroyInstance(VkInstance, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_EnumeratePhysicalDevices(
    VkInstance, uint32_t*, VkPhysicalDevice*);
/* Physical device wrappers (physdev.c) — translate StereoPhysicalDevice* → real handle */
VKAPI_ATTR void VKAPI_CALL stereo_GetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties*);
VKAPI_ATTR void VKAPI_CALL stereo_GetPhysicalDeviceProperties2(VkPhysicalDevice, VkPhysicalDeviceProperties2*);
VKAPI_ATTR void VKAPI_CALL stereo_GetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures*);
VKAPI_ATTR void VKAPI_CALL stereo_GetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2*);
VKAPI_ATTR void VKAPI_CALL stereo_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*);
VKAPI_ATTR void VKAPI_CALL stereo_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2*);
VKAPI_ATTR void VKAPI_CALL stereo_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*);
VKAPI_ATTR void VKAPI_CALL stereo_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties2*);
VKAPI_ATTR void VKAPI_CALL stereo_GetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat, VkFormatProperties*);
VKAPI_ATTR void VKAPI_CALL stereo_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice, VkFormat, VkFormatProperties2*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling, VkImageUsageFlags, VkImageCreateFlags, VkImageFormatProperties*);
VKAPI_ATTR void VKAPI_CALL stereo_GetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType, VkSampleCountFlagBits, VkImageUsageFlags, VkImageTiling, uint32_t*, VkSparseImageFormatProperties*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_EnumerateDeviceExtensionProperties(VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_EnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t*, VkLayerProperties*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkSurfaceFormatKHR*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkPresentModeKHR*);
/* physdev_ext.c — extension / KHR-alias wrappers */
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceFeatures2KHR(VkPhysicalDevice, VkPhysicalDeviceFeatures2*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceProperties2KHR(VkPhysicalDevice, VkPhysicalDeviceProperties2*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceFormatProperties2KHR(VkPhysicalDevice, VkFormat, VkFormatProperties2*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceMemoryProperties2KHR(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceQueueFamilyProperties2KHR(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties2*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice, const VkPhysicalDeviceImageFormatInfo2*, VkImageFormatProperties2*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceImageFormatProperties2KHR(VkPhysicalDevice, const VkPhysicalDeviceImageFormatInfo2*, VkImageFormatProperties2*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2*, uint32_t*, VkSparseImageFormatProperties2*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceSparseImageFormatProperties2KHR(VkPhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2*, uint32_t*, VkSparseImageFormatProperties2*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice, const VkPhysicalDeviceExternalBufferInfo*, VkExternalBufferProperties*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceExternalBufferPropertiesKHR(VkPhysicalDevice, const VkPhysicalDeviceExternalBufferInfo*, VkExternalBufferProperties*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice, const VkPhysicalDeviceExternalFenceInfo*, VkExternalFenceProperties*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceExternalFencePropertiesKHR(VkPhysicalDevice, const VkPhysicalDeviceExternalFenceInfo*, VkExternalFenceProperties*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo*, VkExternalSemaphoreProperties*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceExternalSemaphorePropertiesKHR(VkPhysicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo*, VkExternalSemaphoreProperties*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_EnumeratePhysicalDeviceGroups(VkInstance, uint32_t*, VkPhysicalDeviceGroupProperties*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_EnumeratePhysicalDeviceGroupsKHR(VkInstance, uint32_t*, VkPhysicalDeviceGroupProperties*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR*, VkSurfaceCapabilities2KHR*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR*, uint32_t*, VkSurfaceFormat2KHR*);
#ifdef VK_EXT_full_screen_exclusive
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceSurfacePresentModes2EXT(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR*, uint32_t*, VkPresentModeKHR*);
#endif
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkRect2D*);
#ifdef VK_KHR_win32_surface
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceDisplayPropertiesKHR(VkPhysicalDevice, uint32_t*, VkDisplayPropertiesKHR*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceDisplayPlanePropertiesKHR(VkPhysicalDevice, uint32_t*, VkDisplayPlanePropertiesKHR*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetDisplayPlaneSupportedDisplaysKHR(VkPhysicalDevice, uint32_t, uint32_t*, VkDisplayKHR*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetDisplayModePropertiesKHR(VkPhysicalDevice, VkDisplayKHR, uint32_t*, VkDisplayModePropertiesKHR*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateDisplayModeKHR(VkPhysicalDevice, VkDisplayKHR, const VkDisplayModeCreateInfoKHR*, const VkAllocationCallbacks*, VkDisplayModeKHR*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetDisplayPlaneCapabilitiesKHR(VkPhysicalDevice, VkDisplayModeKHR, uint32_t, VkDisplayPlaneCapabilitiesKHR*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceDisplayProperties2KHR(VkPhysicalDevice, uint32_t*, VkDisplayProperties2KHR*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceDisplayPlaneProperties2KHR(VkPhysicalDevice, uint32_t*, VkDisplayPlaneProperties2KHR*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetDisplayModeProperties2KHR(VkPhysicalDevice, VkDisplayKHR, uint32_t*, VkDisplayModeProperties2KHR*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetDisplayPlaneCapabilities2KHR(VkPhysicalDevice, const VkDisplayPlaneInfo2KHR*, VkDisplayPlaneCapabilities2KHR*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceSurfaceCapabilities2EXT(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilities2EXT*);
VKAPI_ATTR VkBool32 VKAPI_CALL stereo_GetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice, uint32_t);
#endif
#ifdef VK_EXT_calibrated_timestamps
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceCalibrateableTimeDomainsEXT(VkPhysicalDevice, uint32_t*, VkTimeDomainEXT*);
#endif
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceMultisamplePropertiesEXT(VkPhysicalDevice, VkSampleCountFlagBits, VkMultisamplePropertiesEXT*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceExternalImageFormatPropertiesNV(VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling, VkImageUsageFlags, VkImageCreateFlags, VkExternalMemoryHandleTypeFlagsNV, VkExternalImageFormatPropertiesNV*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceGeneratedCommandsPropertiesNVX(VkPhysicalDevice, void*, void*);
#ifdef VK_NV_cooperative_matrix
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceCooperativeMatrixPropertiesNV(VkPhysicalDevice, uint32_t*, VkCooperativeMatrixPropertiesNV*);
#endif
#ifdef VK_NV_coverage_reduction_mode
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV(VkPhysicalDevice, uint32_t*, VkFramebufferMixedSamplesCombinationNV*);
#endif
VKAPI_ATTR VkResult VKAPI_CALL stereo_EnumerateInstanceExtensionProperties(
    const char*, uint32_t*, VkExtensionProperties*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_EnumerateInstanceVersion(uint32_t*);
/* ── Instance-level surface / debug wrappers (translate VkInstance) ──── */
VKAPI_ATTR void     VKAPI_CALL stereo_DestroySurfaceKHR(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks*);
#ifdef VK_KHR_win32_surface
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateWin32SurfaceKHR(VkInstance, const VkWin32SurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR*);
#endif
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateDebugReportCallbackEXT(VkInstance, const VkDebugReportCallbackCreateInfoEXT*, const VkAllocationCallbacks*, VkDebugReportCallbackEXT*);
VKAPI_ATTR void     VKAPI_CALL stereo_DestroyDebugReportCallbackEXT(VkInstance, VkDebugReportCallbackEXT, const VkAllocationCallbacks*);
VKAPI_ATTR void     VKAPI_CALL stereo_DebugReportMessageEXT(VkInstance, VkDebugReportFlagsEXT, VkDebugReportObjectTypeEXT, uint64_t, size_t, int32_t, const char*, const char*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateDebugUtilsMessengerEXT(VkInstance, const VkDebugUtilsMessengerCreateInfoEXT*, const VkAllocationCallbacks*, VkDebugUtilsMessengerEXT*);
VKAPI_ATTR void     VKAPI_CALL stereo_DestroyDebugUtilsMessengerEXT(VkInstance, VkDebugUtilsMessengerEXT, const VkAllocationCallbacks*);
VKAPI_ATTR void     VKAPI_CALL stereo_SubmitDebugUtilsMessageEXT(VkInstance, VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateDevice(
    VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
VKAPI_ATTR void VKAPI_CALL stereo_DestroyDevice(VkDevice, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateRenderPass(
    VkDevice, const VkRenderPassCreateInfo*, const VkAllocationCallbacks*, VkRenderPass*);
#ifdef VK_KHR_create_renderpass2
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateRenderPass2KHR(
    VkDevice, const VkRenderPassCreateInfo2*, const VkAllocationCallbacks*, VkRenderPass*);
#endif
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateShaderModule(
    VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*);
VKAPI_ATTR void VKAPI_CALL stereo_DestroyShaderModule(
    VkDevice, VkShaderModule, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateSwapchainKHR(
    VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
VKAPI_ATTR void VKAPI_CALL stereo_DestroySwapchainKHR(
    VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetSwapchainImagesKHR(
    VkDevice, VkSwapchainKHR, uint32_t*, VkImage*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_AcquireNextImageKHR(
    VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_QueuePresentKHR(VkQueue, const VkPresentInfoKHR*);

bool spirv_patch_stereo_vertex(
    const uint32_t *in_words, size_t in_count,
    uint32_t **out_words, size_t *out_count,
    float left_offset, float right_offset, float convergence);
void spirv_patched_free(uint32_t *words);

VkResult stereo_composite_to_sbs(
    StereoDevice*, VkQueue, StereoSwapchain*, uint32_t image_index);

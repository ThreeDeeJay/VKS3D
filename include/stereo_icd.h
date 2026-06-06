#pragma once
/*
 * stereo_icd.h — Vulkan 1.1 Stereoscopic ICD (VKS3D)
 */

#include "platform.h"

#ifdef _WIN32
#  ifndef VK_USE_PLATFORM_WIN32_KHR
#    define VK_USE_PLATFORM_WIN32_KHR
#  endif
#endif

#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

/* ── Self-contained fallbacks for external memory extensions ─────────────── */
#if !defined(VK_KHR_external_memory)
#define VK_KHR_external_memory 1
typedef struct VkExternalMemoryImageCreateInfo {
    VkStructureType                       sType;
    const void                           *pNext;
    VkExternalMemoryHandleTypeFlags       handleTypes;
} VkExternalMemoryImageCreateInfo;
#define VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO ((VkStructureType)1000072001)
#endif

#if defined(_WIN32) && !defined(VK_KHR_external_memory_win32)
#define VK_KHR_external_memory_win32 1
#define VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME "VK_KHR_external_memory_win32"

#ifndef VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT
#define VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT \
    ((VkExternalMemoryHandleTypeFlagBits)0x00000008)
#endif

typedef struct VkImportMemoryWin32HandleInfoKHR {
    VkStructureType                          sType;
    const void                              *pNext;
    VkExternalMemoryHandleTypeFlagBits       handleType;
    HANDLE                                   handle;
    LPCWSTR                                  name;
} VkImportMemoryWin32HandleInfoKHR;

typedef struct VkMemoryWin32HandlePropertiesKHR {
    VkStructureType    sType;
    void              *pNext;
    uint32_t           memoryTypeBits;
} VkMemoryWin32HandlePropertiesKHR;

#define VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR  ((VkStructureType)1000073000)
#define VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR   ((VkStructureType)1000073002)

typedef VkResult (VKAPI_PTR *PFN_vkGetMemoryWin32HandlePropertiesKHR)(
    VkDevice                              device,
    VkExternalMemoryHandleTypeFlagBits    handleType,
    HANDLE                                handle,
    VkMemoryWin32HandlePropertiesKHR     *pMemoryWin32HandleProperties);
#endif /* VK_KHR_external_memory_win32 */

/* VkPhysicalDeviceToolProperties compat shim */
#ifndef VK_MAX_EXTENSION_NAME_SIZE
#  define VK_MAX_EXTENSION_NAME_SIZE 256
#endif
#ifndef VK_MAX_DESCRIPTION_SIZE
#  define VK_MAX_DESCRIPTION_SIZE 256
#endif
#if defined(VK_VERSION_1_3)
#elif defined(VK_EXT_tooling_info)
  typedef VkPhysicalDeviceToolPropertiesEXT VkPhysicalDeviceToolProperties;
# ifndef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TOOL_PROPERTIES
#   define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TOOL_PROPERTIES \
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TOOL_PROPERTIES_EXT
# endif
#else
  typedef VkFlags VkToolPurposeFlags;
  typedef struct VkPhysicalDeviceToolProperties {
      VkStructureType    sType; void *pNext;
      char name[VK_MAX_EXTENSION_NAME_SIZE];
      char version[VK_MAX_EXTENSION_NAME_SIZE];
      VkToolPurposeFlags purposes;
      char description[VK_MAX_DESCRIPTION_SIZE];
      char layer[VK_MAX_EXTENSION_NAME_SIZE];
  } VkPhysicalDeviceToolProperties;
# define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TOOL_PROPERTIES ((VkStructureType)1000245000)
#endif

#ifndef ICD_LOADER_MAGIC
#  define ICD_LOADER_MAGIC 0xCD1CDABA1DABADABULL
#endif
#ifndef SET_LOADER_MAGIC_VALUE
#  define SET_LOADER_MAGIC_VALUE(obj) \
    do { ((VK_LOADER_DATA*)(void*)(obj))->loaderMagic = ICD_LOADER_MAGIC; } while(0)
#endif

#include <stdint.h>
#include <stdbool.h>

/* ── Loader/ICD interface version ─────────────────────────────────────────── */
#define STEREO_ICD_INTERFACE_VERSION 7

/* ── Maximum tracked objects ─────────────────────────────────────────────── */
#define MAX_INSTANCES           8
#define MAX_PHYSICAL_DEVICES    16
#define MAX_DEVICES             32
#define MAX_RENDER_PASSES       4096
#define MAX_SWAPCHAINS          64
#define MAX_DEPTH_IMAGES        256

/* ── Shader module cache ──────────────────────────────────────────────────── *
 * Stores original (unpatched) SPIR-V for vertex/geometry/tesseval shaders.   *
 * Used by stereo_CreateGraphicsPipelines to patch the correct stage.          *
 * Fragment and compute shaders are never cached.                              */
#define MAX_SHADER_CACHE 2048
typedef struct {
    VkShaderModule  handle;
    uint32_t       *spv;    /* heap copy of original SPIR-V words */
    size_t          words;
} StereoShaderCache;

/* ── Stereo presentation mode ─────────────────────────────────────────────── */
typedef enum StereoPresentMode {
    STEREO_PRESENT_AUTO       = 0,
    STEREO_PRESENT_DXGI       = 1,
    STEREO_PRESENT_DX9        = 2,
    STEREO_PRESENT_SBS        = 3,
    STEREO_PRESENT_TAB        = 4,
    STEREO_PRESENT_INTERLACED = 5,
    STEREO_PRESENT_MONO       = 6,
} StereoPresentMode;

typedef struct StereoConfig {
    bool              enabled;
    float             separation;
    float             convergence;
    bool              flip_eyes;
    float             left_eye_offset;
    float             right_eye_offset;
    StereoPresentMode present_mode;
    uint32_t          override_width;
    uint32_t          override_height;
    uint32_t          refresh_rate;
    bool              half_fps;
    bool              multiview;
    float             step_separation;
    float             step_convergence;
} StereoConfig;

void stereo_config_init(StereoConfig *cfg);
void stereo_config_compute_offsets(StereoConfig *cfg);

extern char g_dll_dir[512];
extern char g_exe_dir[512];
extern char g_global_ini[512];
extern char g_local_ini[512];

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
    PFN_vkGetPhysicalDeviceImageFormatProperties2   GetPhysicalDeviceImageFormatProperties2;
    PFN_vkGetPhysicalDeviceSparseImageFormatProperties2 GetPhysicalDeviceSparseImageFormatProperties2;
    PFN_vkGetPhysicalDeviceExternalBufferProperties GetPhysicalDeviceExternalBufferProperties;
    PFN_vkGetPhysicalDeviceExternalFenceProperties  GetPhysicalDeviceExternalFenceProperties;
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties GetPhysicalDeviceExternalSemaphoreProperties;
    PFN_vkEnumeratePhysicalDeviceGroups             EnumeratePhysicalDeviceGroups;
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR  GetPhysicalDeviceSurfaceCapabilities2KHR;
    PFN_vkGetPhysicalDeviceSurfaceFormats2KHR       GetPhysicalDeviceSurfaceFormats2KHR;
    PFN_vkGetPhysicalDevicePresentRectanglesKHR     GetPhysicalDevicePresentRectanglesKHR;
#ifdef VK_KHR_win32_surface
    PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR GetPhysicalDeviceWin32PresentationSupportKHR;
#endif
#ifdef VK_EXT_full_screen_exclusive
    PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT  GetPhysicalDeviceSurfacePresentModes2EXT;
#endif
#ifdef VK_EXT_calibrated_timestamps
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT GetPhysicalDeviceCalibrateableTimeDomainsEXT;
#endif
    PFN_vkGetPhysicalDeviceMultisamplePropertiesEXT GetPhysicalDeviceMultisamplePropertiesEXT;
    PFN_vkGetPhysicalDeviceExternalImageFormatPropertiesNV GetPhysicalDeviceExternalImageFormatPropertiesNV;
#ifdef VK_NV_cooperative_matrix
    PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesNV GetPhysicalDeviceCooperativeMatrixPropertiesNV;
#endif
#ifdef VK_NV_coverage_reduction_mode
    PFN_vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV GetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV;
#endif
    PFN_vkVoidFunction GetPhysicalDeviceGeneratedCommandsPropertiesNVX;
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
    PFN_vkGetDeviceProcAddr          GetDeviceProcAddr;
    PFN_vkDestroyDevice              DestroyDevice;
    PFN_vkGetDeviceQueue             GetDeviceQueue;
    PFN_vkQueueSubmit                QueueSubmit;
    PFN_vkQueueWaitIdle              QueueWaitIdle;
    PFN_vkDeviceWaitIdle             DeviceWaitIdle;
    PFN_vkAllocateMemory             AllocateMemory;
    PFN_vkFreeMemory                 FreeMemory;
    PFN_vkMapMemory                  MapMemory;
    PFN_vkUnmapMemory                UnmapMemory;
    PFN_vkFlushMappedMemoryRanges    FlushMappedMemoryRanges;
    PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges;
    PFN_vkBindBufferMemory           BindBufferMemory;
    PFN_vkBindImageMemory            BindImageMemory;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkGetImageMemoryRequirements  GetImageMemoryRequirements;
    PFN_vkCreateFence                CreateFence;
    PFN_vkDestroyFence               DestroyFence;
    PFN_vkResetFences                ResetFences;
    PFN_vkGetFenceStatus             GetFenceStatus;
    PFN_vkWaitForFences              WaitForFences;
    PFN_vkCreateSemaphore            CreateSemaphore;
    PFN_vkDestroySemaphore           DestroySemaphore;
    PFN_vkCreateEvent                CreateEvent;
    PFN_vkDestroyEvent               DestroyEvent;
    PFN_vkGetEventStatus             GetEventStatus;
    PFN_vkSetEvent                   SetEvent;
    PFN_vkResetEvent                 ResetEvent;
    PFN_vkCreateQueryPool            CreateQueryPool;
    PFN_vkDestroyQueryPool           DestroyQueryPool;
    PFN_vkGetQueryPoolResults        GetQueryPoolResults;
    PFN_vkCreateBuffer               CreateBuffer;
    PFN_vkDestroyBuffer              DestroyBuffer;
    PFN_vkCreateBufferView           CreateBufferView;
    PFN_vkDestroyBufferView          DestroyBufferView;
    PFN_vkCreateImage                CreateImage;
    PFN_vkDestroyImage               DestroyImage;
    PFN_vkGetImageSubresourceLayout  GetImageSubresourceLayout;
    PFN_vkCreateImageView            CreateImageView;
    PFN_vkDestroyImageView           DestroyImageView;
    PFN_vkCreateShaderModule         CreateShaderModule;
    PFN_vkDestroyShaderModule        DestroyShaderModule;
    PFN_vkCreatePipelineCache        CreatePipelineCache;
    PFN_vkDestroyPipelineCache       DestroyPipelineCache;
    PFN_vkGetPipelineCacheData       GetPipelineCacheData;
    PFN_vkMergePipelineCaches        MergePipelineCaches;
    PFN_vkCreateGraphicsPipelines    CreateGraphicsPipelines;
    PFN_vkCreateComputePipelines     CreateComputePipelines;
    PFN_vkDestroyPipeline            DestroyPipeline;
    PFN_vkCreatePipelineLayout       CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout      DestroyPipelineLayout;
    PFN_vkCreateSampler              CreateSampler;
    PFN_vkDestroySampler             DestroySampler;
    PFN_vkCreateDescriptorSetLayout  CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkCreateDescriptorPool       CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool      DestroyDescriptorPool;
    PFN_vkResetDescriptorPool        ResetDescriptorPool;
    PFN_vkAllocateDescriptorSets     AllocateDescriptorSets;
    PFN_vkFreeDescriptorSets         FreeDescriptorSets;
    PFN_vkUpdateDescriptorSets       UpdateDescriptorSets;
    PFN_vkCreateFramebuffer          CreateFramebuffer;
    PFN_vkDestroyFramebuffer         DestroyFramebuffer;
    PFN_vkCreateRenderPass           CreateRenderPass;
    PFN_vkCreateRenderPass2KHR       CreateRenderPass2KHR;
    PFN_vkDestroyRenderPass          DestroyRenderPass;
    PFN_vkGetRenderAreaGranularity   GetRenderAreaGranularity;
    PFN_vkCreateCommandPool          CreateCommandPool;
    PFN_vkDestroyCommandPool         DestroyCommandPool;
    PFN_vkResetCommandPool           ResetCommandPool;
    PFN_vkAllocateCommandBuffers     AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers         FreeCommandBuffers;
    PFN_vkBeginCommandBuffer         BeginCommandBuffer;
    PFN_vkEndCommandBuffer           EndCommandBuffer;
    PFN_vkResetCommandBuffer         ResetCommandBuffer;
    PFN_vkCmdBindPipeline            CmdBindPipeline;
    PFN_vkCmdSetViewport             CmdSetViewport;
    PFN_vkCmdSetScissor              CmdSetScissor;
    PFN_vkCmdSetLineWidth            CmdSetLineWidth;
    PFN_vkCmdSetDepthBias            CmdSetDepthBias;
    PFN_vkCmdSetBlendConstants       CmdSetBlendConstants;
    PFN_vkCmdSetDepthBounds          CmdSetDepthBounds;
    PFN_vkCmdSetStencilCompareMask   CmdSetStencilCompareMask;
    PFN_vkCmdSetStencilWriteMask     CmdSetStencilWriteMask;
    PFN_vkCmdSetStencilReference     CmdSetStencilReference;
    PFN_vkCmdBindDescriptorSets      CmdBindDescriptorSets;
    PFN_vkCmdBindIndexBuffer         CmdBindIndexBuffer;
    PFN_vkCmdBindVertexBuffers       CmdBindVertexBuffers;
    PFN_vkCmdDraw                    CmdDraw;
    PFN_vkCmdDrawIndexed             CmdDrawIndexed;
    PFN_vkCmdDrawIndirect            CmdDrawIndirect;
    PFN_vkCmdDrawIndexedIndirect     CmdDrawIndexedIndirect;
    PFN_vkCmdDispatch                CmdDispatch;
    PFN_vkCmdDispatchIndirect        CmdDispatchIndirect;
    PFN_vkCmdCopyBuffer              CmdCopyBuffer;
    PFN_vkCmdCopyImage               CmdCopyImage;
    PFN_vkCmdBlitImage               CmdBlitImage;
    PFN_vkCmdCopyBufferToImage       CmdCopyBufferToImage;
    PFN_vkCmdCopyImageToBuffer       CmdCopyImageToBuffer;
    PFN_vkCmdUpdateBuffer            CmdUpdateBuffer;
    PFN_vkCmdFillBuffer              CmdFillBuffer;
    PFN_vkCmdClearColorImage         CmdClearColorImage;
    PFN_vkCmdClearDepthStencilImage  CmdClearDepthStencilImage;
    PFN_vkCmdClearAttachments        CmdClearAttachments;
    PFN_vkCmdResolveImage            CmdResolveImage;
    PFN_vkCmdSetEvent                CmdSetEvent;
    PFN_vkCmdResetEvent              CmdResetEvent;
    PFN_vkCmdWaitEvents              CmdWaitEvents;
    PFN_vkCmdPipelineBarrier         CmdPipelineBarrier;
    PFN_vkCmdBeginQuery              CmdBeginQuery;
    PFN_vkCmdEndQuery                CmdEndQuery;
    PFN_vkCmdResetQueryPool          CmdResetQueryPool;
    PFN_vkCmdWriteTimestamp          CmdWriteTimestamp;
    PFN_vkCmdCopyQueryPoolResults    CmdCopyQueryPoolResults;
    PFN_vkCmdPushConstants           CmdPushConstants;
    PFN_vkCmdBeginRenderPass         CmdBeginRenderPass;
    PFN_vkCmdNextSubpass             CmdNextSubpass;
    PFN_vkCmdEndRenderPass           CmdEndRenderPass;
    PFN_vkCmdExecuteCommands         CmdExecuteCommands;
    PFN_vkCreateSwapchainKHR         CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR        DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR      GetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR        AcquireNextImageKHR;
    PFN_vkQueuePresentKHR            QueuePresentKHR;
    PFN_vkGetMemoryWin32HandlePropertiesKHR GetMemoryWin32HandlePropertiesKHR;
} RealDeviceDispatch;

/* ── Object wrappers ─────────────────────────────────────────────────────── */

#define MAX_SURFACES 16
typedef struct StereoSurfaceHWND { VkSurfaceKHR surface; HWND hwnd; } StereoSurfaceHWND;

typedef struct StereoInstance {
    VK_LOADER_DATA            loader_data;
    VkInstance                real_instance;
    RealInstanceDispatch      real;
    StereoConfig              stereo;
    PFN_vkGetInstanceProcAddr real_get_instance_proc_addr;
    VkDebugUtilsMessengerEXT  debug_messenger;
    StereoSurfaceHWND         surface_hwnd[MAX_SURFACES];
    uint32_t                  surface_hwnd_count;
} StereoInstance;

static inline HWND stereo_si_hwnd_for_surface(StereoInstance *si, VkSurfaceKHR surface)
{
    for (uint32_t i=0; i<si->surface_hwnd_count; i++)
        if (si->surface_hwnd[i].surface==surface) return si->surface_hwnd[i].hwnd;
    return NULL;
}

typedef struct StereoPhysdev {
    VK_LOADER_DATA   loader_data;
    VkPhysicalDevice real_pd;
    StereoInstance  *si;
} StereoPhysdev;

typedef struct StereoSwapchain {
    VkSwapchainKHR    real_swapchain;
    VkSwapchainKHR    app_handle;
    VkDevice          device;
    uint32_t          app_width, app_height;
    VkFormat          format;
    bool              stereo_active, dxgi_mode;
    uint32_t          image_count;
    VkImage          *stereo_images;
    VkDeviceMemory   *stereo_memory;
    VkImageView      *stereo_views_arr;
    HWND              hwnd;
    void             *dxgi_sc, *shared_d3d11_tex;
    HANDLE            shared_nt_handle;
    VkCommandPool     barrier_pool;
    VkCommandBuffer  *barrier_cmds;
    VkFence          *barrier_fences;
    uint32_t          acquire_idx;
    /* GPU blit compose � replaces CPU readback + GDI for SBS/TAB */
    VkImage    *comp_sc_images;     /* real output swapchain images  */
    uint32_t    comp_sc_count;
    VkSemaphore comp_acquire_sem;   /* image-available semaphore     */
    VkSemaphore comp_blit_done_sem; /* blit-complete semaphore       */
    VkImage          *sbs_images;
    uint32_t          sbs_width;
    StereoPresentMode present_mode;
    bool              cpu_ok;
    VkCommandPool     cpu_pool;
    VkCommandBuffer   cpu_cmd;
    VkFence           cpu_fence;
    VkBuffer          cpu_buf;
    VkDeviceMemory    cpu_mem;
    void             *cpu_map;
    uint32_t          cpu_eye_bytes;
} StereoSwapchain;

typedef struct StereoRenderPassInfo {
    VkRenderPass  handle;
    bool          has_multiview;
    uint32_t      view_mask;
    uint32_t      subpass_count;
} StereoRenderPassInfo;

typedef struct StereoDevice {
    /* MUST be first: loader reads *(void**)device for dispatch table. */
    VK_LOADER_DATA         loader_data;
    VkDevice               real_device;
    StereoInstance        *si;
    VkPhysicalDevice       real_physdev;
    RealDeviceDispatch     real;
    StereoConfig           stereo;
    VkBuffer               stereo_ubo;
    VkDeviceMemory         stereo_ubo_mem;
    void                  *stereo_ubo_map;
    StereoRenderPassInfo   render_passes[MAX_RENDER_PASSES];
    uint32_t               render_pass_count;
    StereoSwapchain        swapchains[MAX_SWAPCHAINS];
    uint32_t               swapchain_count;

    VkImage                intercepted_depth[MAX_DEPTH_IMAGES];
    uint32_t               intercepted_depth_count;
    uint32_t               stereo_w, stereo_h;
    stereo_mutex_t         lock;

    /* ── Multiview render pass tracking ─────────────────────────────────── *
     * Set to true when stereo_CreateRenderPass successfully injects          *
     * viewMask=0x3 into a swapchain output render pass.                      *
     * alt_cpu_readback uses this to decide layer_count (1 vs 2):             *
     *   true  → both layers rendered → read layer_count=2                    *
     *   false → DXVK / app did not use PRESENT_SRC_KHR as finalLayout →     *
     *           layer 1 is UNDEFINED → read layer_count=1, show left for     *
     *           both eyes (2D, not black, not a GPU TDR).                    */
    bool                   multiview_pass_exists;

    /* ── Shader module cache for deferred SPIR-V patching ───────────────── *
     * stereo_CreateShaderModule stores original SPIR-V here for VS/GS/TES.  *
     * stereo_CreateGraphicsPipelines picks the last pre-rast stage, patches  *
     * it, creates a temporary VkShaderModule, builds the pipeline, then      *
     * destroys the temporary module — all in one call.                        *
     * stereo_DestroyShaderModule frees the cache entry.                       */
    StereoShaderCache      shader_cache[MAX_SHADER_CACHE];
    uint32_t               shader_cache_count;

    /* ── Temporary patched shader modules (alive until DestroyDevice) ───── *
     * Driver 426.06 holds a reference to module SPIR-V even after           *
     * CreateGraphicsPipelines returns, so we must not destroy the temp      *
     * module immediately.  Pool them here and destroy in stereo_DestroyDevice */
#define MAX_TMP_MODULES 512
    VkShaderModule         tmp_modules[MAX_TMP_MODULES];
    uint32_t               tmp_module_count;

    /* ── D3D11 / DXGI stereo output ──────────────────────────────────────── */
    bool                   d3d11_ok, dxgi_init_in_progress;
    void                  *d3d11_dev, *d3d11_ctx, *nvapi_stereo, *nvapi_lib, *d3d11_lib;

    /* ── Graphics queue ───────────────────────────────────────────────────── */
    VkQueue                gfx_queue;
    uint32_t               gfx_qf;

    /* ── INI file paths ───────────────────────────────────────────────────── */
    char                   global_ini[512];
    char                   local_ini[512];

    /* ── Hotkey debounce ──────────────────────────────────────────────────── */
    uint32_t               hotkey_prev;

    /* ── D3D9 direct-mode state ───────────────────────────────────────────── */
    bool                   dx9_ok;
    void                  *dx9_lib, *dx9_d3d, *dx9_dev, *dx9_surf, *dx9_nvstereo;

    /* ── Compose swap chain state ─────────────────────────────────────────── */
    bool                   comp_ok;
    HWND                   comp_hwnd;
    void                  *comp_composed;
    uint32_t               comp_w, comp_h;
} StereoDevice;

/* ── Stereo UBO layout ───────────────────────────────────────────────────── */
typedef struct StereoUBO {
    float eye_offset[2];
    float convergence;
    float _pad;
} StereoUBO;

/* ── Object lookup helpers ───────────────────────────────────────────────── */
StereoInstance  *stereo_instance_from_handle(VkInstance h);
StereoInstance  *stereo_si_from_physdev(VkPhysicalDevice pd);
StereoPhysdev   *stereo_physdev_get_or_create(VkPhysicalDevice real_pd, StereoInstance *si);
StereoDevice    *stereo_device_from_handle(VkDevice h);
StereoSwapchain *stereo_swapchain_lookup(StereoDevice *dev, VkSwapchainKHR sc);
StereoRenderPassInfo *stereo_rp_lookup(StereoDevice *dev, VkRenderPass rp);
PFN_vkGetInstanceProcAddr stereo_get_real_giPA(void);
PFN_vkGetInstanceProcAddr stereo_get_real_pdPA(void);
void             stereo_instance_free(VkInstance h);
StereoDevice    *stereo_device_alloc(void);

/* ── Forward declarations ────────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
VKAPI_ATTR void     VKAPI_CALL stereo_DestroyInstance(VkInstance, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_EnumeratePhysicalDevices(VkInstance, uint32_t*, VkPhysicalDevice*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceProperties2(VkPhysicalDevice, VkPhysicalDeviceProperties2*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties2*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat, VkFormatProperties*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice, VkFormat, VkFormatProperties2*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling, VkImageUsageFlags, VkImageCreateFlags, VkImageFormatProperties*);
VKAPI_ATTR void     VKAPI_CALL stereo_GetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType, VkSampleCountFlagBits, VkImageUsageFlags, VkImageTiling, uint32_t*, VkSparseImageFormatProperties*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_EnumerateDeviceExtensionProperties(VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_EnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t*, VkLayerProperties*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkSurfaceFormatKHR*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkPresentModeKHR*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceToolProperties(VkPhysicalDevice, uint32_t*, VkPhysicalDeviceToolProperties*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetPhysicalDeviceToolPropertiesEXT(VkPhysicalDevice, uint32_t*, VkPhysicalDeviceToolProperties*);
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
VKAPI_ATTR VkResult VKAPI_CALL stereo_EnumerateInstanceExtensionProperties(const char*, uint32_t*, VkExtensionProperties*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_EnumerateInstanceVersion(uint32_t*);
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
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
VKAPI_ATTR void     VKAPI_CALL stereo_DestroyDevice(VkDevice, const VkAllocationCallbacks*);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL stereo_GetDeviceProcAddr(VkDevice, const char*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateImageView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateRenderPass(VkDevice, const VkRenderPassCreateInfo*, const VkAllocationCallbacks*, VkRenderPass*);
#ifdef VK_KHR_create_renderpass2
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateRenderPass2KHR(VkDevice, const VkRenderPassCreateInfo2*, const VkAllocationCallbacks*, VkRenderPass*);
#endif
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateShaderModule(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*);
VKAPI_ATTR void     VKAPI_CALL stereo_DestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_CreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
VKAPI_ATTR void     VKAPI_CALL stereo_DestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_GetSwapchainImagesKHR(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_AcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
VKAPI_ATTR VkResult VKAPI_CALL stereo_QueuePresentKHR(VkQueue, const VkPresentInfoKHR*);

bool spirv_patch_stereo_vertex(const uint32_t *in, size_t in_c,
    uint32_t **out, size_t *out_c,
    float lo, float ro, float conv, bool inj_vi);
void spirv_patched_free(uint32_t *w);

VkResult stereo_dxgi_present(StereoDevice*, VkQueue, StereoSwapchain*,
    uint32_t, uint32_t, const VkSemaphore*);

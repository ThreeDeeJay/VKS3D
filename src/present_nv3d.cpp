/*
 * present_nv3d.cpp - NVIDIA 3D Vision presentation through NV3D-Lib
 *
 * Design (fullscreen-safe)
 * ------------------------
 *  * NV3D (popup window + D3D9Ex FSE device + shared texture/fence) is created ONCE and
 *    lives until the VkDevice is destroyed.  Creating/destroying it on every swapchain
 *    (re)creation hung inside D3D9Presenter::Shutdown / D3D9 FSE creation whenever the
 *    app toggled fullscreen.
 *  * The shared texture is always (2*W) x H where W x H is the display mode (per eye).
 *    Whatever size the app renders at is scaled into it with vkCmdBlitImage, so
 *    swapchain resizes / fullscreen switches never touch NV3D.
 *  * While NV3D is active the display mode belongs to NV3D: ChangeDisplaySettings*
 *    calls made by the app / DXVK (e.g. BasicHLSL10/11 forcing 60 Hz) are swallowed,
 *    otherwise they pull the mode out from under the D3D9 FSE device.
 *  * Before NV3D is created the desktop refresh rate is forced to a 3D Vision
 *    compatible one (90/100/110/120/144, preferring `refresh_rate` from vks3d.ini).
 *  * The popup is created in "tracked game" mode (stays topmost, never auto-minimizes
 *    on focus loss).  vks3d.ini [VKS3D] nv3d_tracked_game=0 restores auto-minimize.
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <cstring>
#include <malloc.h>

#include "stereo_icd.h"
#include "ini.h"
#include "NV3D.hpp"

/* Logging that works regardless of how STEREO_LOG is configured. */
#define NV3D_LOG(fmt, ...) \
    do { if (g_vks3d_log_enabled) vks3d_logf("[NV3D] ", fmt, ##__VA_ARGS__); } while (0)
#define NV3D_LIB_LOG(msg) \
    do { if (g_vks3d_log_enabled) vks3d_logf("", "%s", (msg)); } while (0)

/* ------------------------------------------------------------------------- */
/* Local helpers                                                             */
/* ------------------------------------------------------------------------- */

static void cmd_image_barrier(
StereoDevice        *sd,
VkCommandBuffer      cmd,
VkImage              image,
uint32_t             layer_count,
VkImageLayout        old_layout,
VkImageLayout        new_layout,
VkAccessFlags        src_access,
VkAccessFlags        dst_access,
VkPipelineStageFlags src_stage,
VkPipelineStageFlags dst_stage)
{
VkImageMemoryBarrier imb = {};
imb.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
imb.srcAccessMask       = src_access;
imb.dstAccessMask       = dst_access;
imb.oldLayout           = old_layout;
imb.newLayout           = new_layout;
imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
imb.image               = image;
imb.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
imb.subresourceRange.baseMipLevel   = 0;
imb.subresourceRange.levelCount     = 1;
imb.subresourceRange.baseArrayLayer = 0;
imb.subresourceRange.layerCount     = layer_count;

sd->real.CmdPipelineBarrier(
    cmd,
    src_stage,
    dst_stage,
    0,
    0, NULL,
    0, NULL,
    1, &imb);


static uint32_t find_memory_type(
    StereoDevice *sd,
    uint32_t type_bits,
    VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    sd->si->real.GetPhysicalDeviceMemoryProperties(
        sd->real_physdev,
        &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

static bool ini_flag(StereoDevice *sd, const char *key, bool def)
{
    bool v = ini_read_bool(sd->global_ini, "VKS3D", key, def);
    v      = ini_read_bool(sd->local_ini,  "VKS3D", key, v);
    return v;
}

static bool is_bgra_format(VkFormat f)
{
    return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
}

/* ------------------------------------------------------------------------- */
/* 3D Vision compatible refresh rate                                         */
/* ------------------------------------------------------------------------- */

static bool refresh_3dv_ok(DWORD f)
{
    return f == 90 || f == 100 || f == 110 || f == 120 || f == 144;
}

static void nv3d_force_3dv_refresh(uint32_t want)
{
    DEVMODEW cur;
    std::memset(&cur, 0, sizeof(cur));
    cur.dmSize = sizeof(cur);
    if (!EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &cur))
        return;

    if (refresh_3dv_ok(cur.dmDisplayFrequency))
    {
        NV3D_LOG("desktop mode %lux%lu@%luHz is 3D Vision compatible",
                 cur.dmPelsWidth, cur.dmPelsHeight, cur.dmDisplayFrequency);
        return;
    }

    DWORD cand[6];
    int n = 0;
    if (want && refresh_3dv_ok(want))
        cand[n++] = want;
    static const DWORD pref[] = { 144, 120, 110, 100, 90 };
    for (int i = 0; i < 5; i++)
        if (!(n && cand[0] == pref[i]))
            cand[n++] = pref[i];

    NV3D_LOG("desktop mode %lux%lu@%luHz is NOT 3D Vision compatible, trying to switch (wanted %u)",
             cur.dmPelsWidth, cur.dmPelsHeight, cur.dmDisplayFrequency, want);

    for (int c = 0; c < n; c++)
    {
        for (DWORD i = 0; ; i++)
        {
            DEVMODEW m;
            std::memset(&m, 0, sizeof(m));
            m.dmSize = sizeof(m);
            if (!EnumDisplaySettingsW(NULL, i, &m))
                break;
            if (m.dmPelsWidth        != cur.dmPelsWidth  ||
                m.dmPelsHeight       != cur.dmPelsHeight ||
                m.dmBitsPerPel       != cur.dmBitsPerPel ||
                m.dmDisplayFrequency != cand[c]          ||
                (m.dmDisplayFlags & DM_INTERLACED))
                continue;
            m.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
            LONG r = ChangeDisplaySettingsExW(NULL, &m, NULL, CDS_FULLSCREEN, NULL);
            NV3D_LOG("switch to %lux%lu@%luHz -> %ld",
                     m.dmPelsWidth, m.dmPelsHeight, m.dmDisplayFrequency, (long)r);
            if (r == DISP_CHANGE_SUCCESSFUL)
                return;
            break;
        }
    }
    NV3D_LOG("no 3D Vision compatible refresh rate available at %lux%lu",
             cur.dmPelsWidth, cur.dmPelsHeight);
}

static void nv3d_pick_eye_size(
    StereoDevice *sd,
    uint32_t app_w, uint32_t app_h,
    uint32_t *out_w, uint32_t *out_h)
{
    uint32_t w = app_w, h = app_h;

    if (sd->stereo.override_width && sd->stereo.override_height)
    {
        w = sd->stereo.override_width;
        h = sd->stereo.override_height;
    }
    else
    {
        DEVMODEW m;
        std::memset(&m, 0, sizeof(m));
        m.dmSize = sizeof(m);
        if (EnumDisplaySettingsW(NULL, ENUM_REGISTRY_SETTINGS, &m) &&
            m.dmPelsWidth >= 640 && m.dmPelsHeight >= 480)
        {
            w = m.dmPelsWidth;
            h = m.dmPelsHeight;
        }
        else
        {
            std::memset(&m, 0, sizeof(m));
            m.dmSize = sizeof(m);
            if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &m) &&
                m.dmPelsWidth >= 640 && m.dmPelsHeight >= 480)
            {
                w = m.dmPelsWidth;
                h = m.dmPelsHeight;
            }
        }
    }
    *out_w = w;
    *out_h = h;
}

/* ------------------------------------------------------------------------- */
/* Display-mode lock: swallow the app's ChangeDisplaySettings* while NV3D    */
/* owns the display mode.                                                    */
/* ------------------------------------------------------------------------- */

static volatile LONG g_nv3d_modes_locked = 0;
static volatile LONG g_nv3d_swallow_logged = 0;
static bool          g_nv3d_hook_installed = false;

typedef LONG (WINAPI *PFN_CDSExW)(LPCWSTR, DEVMODEW *, HWND, DWORD, LPVOID);
typedef LONG (WINAPI *PFN_CDSExA)(LPCSTR,  DEVMODEA *, HWND, DWORD, LPVOID);
typedef LONG (WINAPI *PFN_CDSW)(DEVMODEW *, DWORD);
typedef LONG (WINAPI *PFN_CDSA)(DEVMODEA *, DWORD);

static PFN_CDSExW g_real_CDSExW = NULL;
static PFN_CDSExA g_real_CDSExA = NULL;
static PFN_CDSW   g_real_CDSW   = NULL;
static PFN_CDSA   g_real_CDSA   = NULL;

static bool nv3d_swallow(const char *api, DWORD flags)
{
    if (!g_nv3d_modes_locked)
        return false;
    if (InterlockedIncrement(&g_nv3d_swallow_logged) <= 16)
        NV3D_LOG("display-mode change swallowed (%s flags=0x%lx) - NV3D owns the display mode",
                 api, (unsigned long)flags);
    return true;
}

static LONG WINAPI hk_CDSExW(LPCWSTR dev, DEVMODEW *dm, HWND hwnd, DWORD fl, LPVOID p)
{
    if (nv3d_swallow("ChangeDisplaySettingsExW", fl)) return DISP_CHANGE_SUCCESSFUL;
    return g_real_CDSExW ? g_real_CDSExW(dev, dm, hwnd, fl, p) : DISP_CHANGE_FAILED;
}
static LONG WINAPI hk_CDSExA(LPCSTR dev, DEVMODEA *dm, HWND hwnd, DWORD fl, LPVOID p)
{
    if (nv3d_swallow("ChangeDisplaySettingsExA", fl)) return DISP_CHANGE_SUCCESSFUL;
    return g_real_CDSExA ? g_real_CDSExA(dev, dm, hwnd, fl, p) : DISP_CHANGE_FAILED;
}
static LONG WINAPI hk_CDSW(DEVMODEW *dm, DWORD fl)
{
    if (nv3d_swallow("ChangeDisplaySettingsW", fl)) return DISP_CHANGE_SUCCESSFUL;
    return g_real_CDSW ? g_real_CDSW(dm, fl) : DISP_CHANGE_FAILED;
}
static LONG WINAPI hk_CDSA(DEVMODEA *dm, DWORD fl)
{
    if (nv3d_swallow("ChangeDisplaySettingsA", fl)) return DISP_CHANGE_SUCCESSFUL;
    return g_real_CDSA ? g_real_CDSA(dm, fl) : DISP_CHANGE_FAILED;
}

/* POD-only (SEH) - rewrites user32!ChangeDisplaySettings* imports of one module. */
static unsigned patch_user32_imports(HMODULE mod)
{
    BYTE    *base  = (BYTE *)mod;
    unsigned count = 0;

    __try
    {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return 0;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return 0;
        IMAGE_DATA_DIRECTORY dir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress)
            return 0;

        for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
             d->Name; d++)
        {
            if (_stricmp((const char *)(base + d->Name), "user32.dll") != 0)
                continue;

            IMAGE_THUNK_DATA *oft = (IMAGE_THUNK_DATA *)(base +
                (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
            IMAGE_THUNK_DATA *ft  = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);

            for (; oft->u1.AddressOfData; oft++, ft++)
            {
                if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal))
                    continue;
                const char *fn =
                    ((IMAGE_IMPORT_BY_NAME *)(base + oft->u1.AddressOfData))->Name;
                void *stub = NULL;
                if      (!strcmp(fn, "ChangeDisplaySettingsExW")) stub = (void *)hk_CDSExW;
                else if (!strcmp(fn, "ChangeDisplaySettingsExA")) stub = (void *)hk_CDSExA;
                else if (!strcmp(fn, "ChangeDisplaySettingsW"))   stub = (void *)hk_CDSW;
                else if (!strcmp(fn, "ChangeDisplaySettingsA"))   stub = (void *)hk_CDSA;
                if (!stub)
                    continue;

                DWORD old = 0;
                if (VirtualProtect(&ft->u1.Function, sizeof(ft->u1.Function),
                                   PAGE_READWRITE, &old))
                {
                    ft->u1.Function = (ULONG_PTR)stub;
                    VirtualProtect(&ft->u1.Function, sizeof(ft->u1.Function), old, &old);
                    count++;
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return count;
}

static bool path_has_prefix_i(const char *p, const char *prefix)
{
    size_t n = strlen(prefix);
    return n && _strnicmp(p, prefix, n) == 0;
}

static void nv3d_install_display_mode_hook(void)
{
    if (g_nv3d_hook_installed)
        return;
    g_nv3d_hook_installed = true;

    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32)
        user32 = LoadLibraryA("user32.dll");
    if (!user32)
        return;
    g_real_CDSExW = (PFN_CDSExW)(void *)GetProcAddress(user32, "ChangeDisplaySettingsExW");
    g_real_CDSExA = (PFN_CDSExA)(void *)GetProcAddress(user32, "ChangeDisplaySettingsExA");
    g_real_CDSW   = (PFN_CDSW)  (void *)GetProcAddress(user32, "ChangeDisplaySettingsW");
    g_real_CDSA   = (PFN_CDSA)  (void *)GetProcAddress(user32, "ChangeDisplaySettingsA");

    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void *)&nv3d_install_display_mode_hook, &self);

    char windir[MAX_PATH];
    windir[0] = '\0';
    GetWindowsDirectoryA(windir, MAX_PATH);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                           GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
    {
        NV3D_LOG("display-mode hook: module snapshot failed (%lu)", (unsigned long)GetLastError());
        return;
    }

    MODULEENTRY32 me;
    std::memset(&me, 0, sizeof(me));
    me.dwSize = sizeof(me);
    unsigned patched_total = 0, patched_modules = 0;
    if (Module32First(snap, &me))
    {
        do
        {
            HMODULE m = (HMODULE)me.modBaseAddr;
            if (m == self)
                continue;
            if (path_has_prefix_i(me.szExePath, windir))          /* system DLLs (d3d9 used by NV3D-Lib...) */
                continue;
            char lower[MAX_PATH];
            strncpy(lower, me.szExePath, sizeof(lower) - 1);
            lower[sizeof(lower) - 1] = '\0';
            _strlwr(lower);
            if (strstr(lower, "\\driverstore\\") || strstr(lower, "\\nvidia"))
                continue;
            unsigned n = patch_user32_imports(m);
            if (n)
            {
                patched_total += n;
                patched_modules++;
                NV3D_LOG("display-mode hook: %u import(s) patched in %s", n, me.szExePath);
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    NV3D_LOG("display-mode hook installed: %u import(s) in %u module(s)",
             patched_total, patched_modules);
}

/* ------------------------------------------------------------------------- */
/* NV3D resource creation                                                    */
/* ------------------------------------------------------------------------- */

extern "C" bool nv3d_init(
    StereoDevice *sd,
    uint32_t width,
    uint32_t height)
{
    /* Device-lifetime: swapchain (re)creation / resize / fullscreen never recreates NV3D. */
    if (sd->nv3d_ok)
    {
        NV3D_LOG("nv3d_init: already running (%ux%u per eye), app swapchain %ux%u -> scaled",
                 sd->nv3d_width / 2, sd->nv3d_height, width, height);
        return true;
    }

    nv3d_destroy(sd);   /* clean any partial earlier attempt */

    /* 3D Vision only works at 90/100/110/120/144 Hz. */
    nv3d_force_3dv_refresh(sd->stereo.refresh_rate);

    uint32_t eye_w = width, eye_h = height;
    nv3d_pick_eye_size(sd, width, height, &eye_w, &eye_h);
    NV3D_LOG("nv3d_init: per-eye %ux%u (app swapchain %ux%u)", eye_w, eye_h, width, height);

    NV3D::InterfaceVulkan *iface = nullptr;
    NV3D::InitParams params = {};

    /* Keep the popup topmost instead of auto-minimizing it whenever the host process
     * loses focus (DXVK's fullscreen switch does that -> iconic popup -> hang on teardown). */
    if (ini_flag(sd, "nv3d_tracked_game", true))
    {
        params.tracked_game_pid = GetCurrentProcessId();
        NV3D_LOG("tracked-game focus mode (pid %lu)", (unsigned long)params.tracked_game_pid);
    }

    NV3D::SetLogSink(
        [](NV3D::LogLevel, const wchar_t *msg, void *)
        {
            char utf8[4096];
            WideCharToMultiByte(CP_UTF8, 0, msg, -1, utf8, sizeof(utf8), nullptr, nullptr);
            utf8[sizeof(utf8) - 1] = '\0';
            NV3D_LIB_LOG(utf8);
        },
        nullptr);

    VkPhysicalDeviceIDProperties id = {};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 props2 = {};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &id;
    sd->si->real.GetPhysicalDeviceProperties2(sd->real_physdev, &props2);

    if (id.deviceLUIDValid)
    {
        params.has_external_luid = true;
        static_assert(sizeof(LUID) == VK_LUID_SIZE, "LUID mismatch");
        std::memcpy(&params.external_luid, id.deviceLUID, VK_LUID_SIZE);
        NV3D_LOG("passing external LUID to NV3D-Lib");
    }

    HRESULT hr = NV3D::CreateInterfaceVulkan(
        sd->si->real_instance,
        sd->real_physdev,
        sd->real_device,
        sd->gfx_qf,
        &params,
        &iface);
    if (FAILED(hr) || !iface)
    {
        NV3D_LOG("CreateInterfaceVulkan failed: 0x%08x", (unsigned)hr);
        return false;
    }

    HANDLE mem_handle   = NULL;
    HANDLE fence_handle = NULL;
    hr = iface->InitSharedResources(
        eye_w * 2,
        eye_h,
        87, /* DXGI_FORMAT_B8G8R8A8_UNORM */
        &mem_handle,
        &fence_handle);
    if (FAILED(hr))
    {
        NV3D_LOG("InitSharedResources failed: 0x%08x", (unsigned)hr);
        iface->Delete();
        return false;
    }

    sd->nv3d_iface        = iface;
    sd->nv3d_mem_handle   = mem_handle;
    sd->nv3d_fence_handle = fence_handle;
    sd->nv3d_width        = eye_w * 2;
    sd->nv3d_height       = eye_h;
    sd->nv3d_value        = 0;

    /* Imported image */
    VkExternalMemoryImageCreateInfo ext_img = {};
    ext_img.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

    VkImageCreateInfo ici = {};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext         = &ext_img;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = VK_FORMAT_B8G8R8A8_UNORM;
    ici.extent.width  = eye_w * 2;
    ici.extent.height = eye_h;
    ici.extent.depth  = 1;
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (sd->real.CreateImage(sd->real_device, &ici, NULL, &sd->nv3d_image) != VK_SUCCESS)
    {
        NV3D_LOG("CreateImage failed");
        nv3d_destroy(sd);
        return false;
    }

    VkMemoryRequirements mr;
    sd->real.GetImageMemoryRequirements(sd->real_device, sd->nv3d_image, &mr);

    uint32_t mem_type = find_memory_type(sd, mr.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX)
    {
        NV3D_LOG("no compatible memory type");
        nv3d_destroy(sd);
        return false;
    }

    VkImportMemoryWin32HandleInfoKHR import_info = {};
    import_info.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    import_info.handle     = mem_handle;

    VkMemoryAllocateInfo ai = {};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.pNext           = &import_info;
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = mem_type;

    if (sd->real.AllocateMemory(sd->real_device, &ai, NULL, &sd->nv3d_memory) != VK_SUCCESS)
    {
        NV3D_LOG("AllocateMemory failed");
        nv3d_destroy(sd);
        return false;
    }
    if (sd->real.BindImageMemory(sd->real_device, sd->nv3d_image, sd->nv3d_memory, 0) != VK_SUCCESS)
    {
        NV3D_LOG("BindImageMemory failed");
        nv3d_destroy(sd);
        return false;
    }

    /* Imported timeline semaphore */
    VkSemaphoreTypeCreateInfo sem_type = {};
    sem_type.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    sem_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    sem_type.initialValue  = 0;

    VkSemaphoreCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.pNext = &sem_type;

    if (sd->real.CreateSemaphore(sd->real_device, &sci, NULL, &sd->nv3d_timeline) != VK_SUCCESS)
    {
        NV3D_LOG("CreateSemaphore failed");
        nv3d_destroy(sd);
        return false;
    }

    VkImportSemaphoreWin32HandleInfoKHR import_sem = {};
    import_sem.sType      = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
    import_sem.semaphore  = sd->nv3d_timeline;
    import_sem.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    import_sem.handle     = fence_handle;

    if (!sd->real.ImportSemaphoreWin32HandleKHR)
    {
        NV3D_LOG("ImportSemaphoreWin32HandleKHR not loaded");
        nv3d_destroy(sd);
        return false;
    }
    if (sd->real.ImportSemaphoreWin32HandleKHR(sd->real_device, &import_sem) != VK_SUCCESS)
    {
        NV3D_LOG("ImportSemaphoreWin32HandleKHR failed");
        nv3d_destroy(sd);
        return false;
    }

    sd->nv3d_ok = true;

    /* From now on NV3D owns the display mode. */
    nv3d_install_display_mode_hook();
    InterlockedExchange(&g_nv3d_modes_locked, 1);

    NV3D_LOG("initialized: shared texture %ux%u (per eye %ux%u), display mode locked",
             sd->nv3d_width, sd->nv3d_height, eye_w, eye_h);
    return true;
}

/* ------------------------------------------------------------------------- */

extern "C" void nv3d_destroy(
    StereoDevice *sd)
{
    if (!sd)
        return;

    InterlockedExchange(&g_nv3d_modes_locked, 0);

    /* Make sure no queued present/blit still touches the imported resources. */
    if (sd->real_device && sd->real.DeviceWaitIdle &&
        (sd->nv3d_image || sd->nv3d_timeline || sd->nv3d_iface))
        sd->real.DeviceWaitIdle(sd->real_device);

    if (sd->nv3d_iface)
    {
        NV3D_LOG("shutting down NV3D");
        ((NV3D::InterfaceVulkan *)sd->nv3d_iface)->Delete();
        sd->nv3d_iface = NULL;
    }
    if (sd->nv3d_timeline)
    {
        sd->real.DestroySemaphore(sd->real_device, sd->nv3d_timeline, NULL);
        sd->nv3d_timeline = VK_NULL_HANDLE;
    }
    if (sd->nv3d_image)
    {
        sd->real.DestroyImage(sd->real_device, sd->nv3d_image, NULL);
        sd->nv3d_image = VK_NULL_HANDLE;
    }
    if (sd->nv3d_memory)
    {
        sd->real.FreeMemory(sd->real_device, sd->nv3d_memory, NULL);
        sd->nv3d_memory = VK_NULL_HANDLE;
    }

    sd->nv3d_ok     = false;
    sd->nv3d_width  = 0;
    sd->nv3d_height = 0;
    sd->nv3d_value  = 0;
}

/* ------------------------------------------------------------------------- */
/* Present                                                                    */
/* ------------------------------------------------------------------------- */

extern "C" VkResult nv3d_present(
    StereoDevice *sd,
    StereoSwapchain *sc,
    VkQueue queue,
    uint32_t wait_sem_count,
    const VkSemaphore *wait_sems)
{
    static uint32_t s_fail_count = 0;
    static bool     s_warned_fmt = false;

    if (!sd->nv3d_iface || !sd->nv3d_ok)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!sc->barrier_cmds || !sc->barrier_cmds[0] || !sc->stereo_images)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkCommandBuffer cmd = sc->barrier_cmds[0];

    const uint32_t eye_w = sd->nv3d_width / 2;
    const uint32_t eye_h = sd->nv3d_height;
    const bool     scaled = (sc->app_width != eye_w) || (sc->app_height != eye_h);
    const bool     bgra   = is_bgra_format(sc->format);
    const bool     first  = (sd->nv3d_value == 0);

    if (!bgra && !s_warned_fmt)
    {
        s_warned_fmt = true;
        NV3D_LOG("warning: app swapchain format %d is not BGRA - colors may be swapped", (int)sc->format);
    }

    sd->real.ResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    sd->real.BeginCommandBuffer(cmd, &begin);

    /* stereo render target -> transfer source */
    cmd_image_barrier(
        sd, cmd, sc->stereo_images[0], 2,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);

    /* NV3D imported texture -> transfer destination */
    cmd_image_barrier(
        sd, cmd, sd->nv3d_image, 1,
        first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);

    if (!scaled && bgra)
    {
        /* 1:1 raw copy (keeps sRGB-encoded bytes untouched) */
        VkImageCopy copies[2];
        std::memset(copies, 0, sizeof(copies));

        copies[0].srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        copies[0].srcSubresource.baseArrayLayer = 0;
        copies[0].srcSubresource.layerCount     = 1;
        copies[0].dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        copies[0].dstSubresource.baseArrayLayer = 0;
        copies[0].dstSubresource.layerCount     = 1;
        copies[0].extent.width  = eye_w;
        copies[0].extent.height = eye_h;
        copies[0].extent.depth  = 1;

        copies[1] = copies[0];
        copies[1].srcSubresource.baseArrayLayer = 1;
        copies[1].dstOffset.x                   = (int32_t)eye_w;

        sd->real.CmdCopyImage(
            cmd,
            sc->stereo_images[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            sd->nv3d_image,       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            2, copies);
    }
    else
    {
        /* scale (and, for RGBA UNORM sources, swizzle) each eye into its half */
        VkImageBlit blits[2];
        std::memset(blits, 0, sizeof(blits));
        for (int e = 0; e < 2; e++)
        {
            blits[e].srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            blits[e].srcSubresource.baseArrayLayer = (uint32_t)e;
            blits[e].srcSubresource.layerCount     = 1;
            blits[e].srcOffsets[1].x = (int32_t)sc->app_width;
            blits[e].srcOffsets[1].y = (int32_t)sc->app_height;
            blits[e].srcOffsets[1].z = 1;
            blits[e].dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            blits[e].dstSubresource.baseArrayLayer = 0;
            blits[e].dstSubresource.layerCount     = 1;
            blits[e].dstOffsets[0].x = e ? (int32_t)eye_w : 0;
            blits[e].dstOffsets[1].x = e ? (int32_t)(eye_w * 2) : (int32_t)eye_w;
            blits[e].dstOffsets[1].y = (int32_t)eye_h;
            blits[e].dstOffsets[1].z = 1;
        }
        sd->real.CmdBlitImage(
            cmd,
            sc->stereo_images[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            sd->nv3d_image,       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            2, blits,
            scaled ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }

    /* return imported texture to GENERAL for NV3D-Lib */
    cmd_image_barrier(
        sd, cmd, sd->nv3d_image, 1,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_MEMORY_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    cmd_image_barrier(
        sd, cmd, sc->stereo_images[0], 2,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    sd->real.EndCommandBuffer(cmd);

    sd->nv3d_value++;

    VkTimelineSemaphoreSubmitInfo tsi = {};
    tsi.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    tsi.signalSemaphoreValueCount = 1;
    tsi.pSignalSemaphoreValues    = &sd->nv3d_value;

    VkPipelineStageFlags *waitStages = NULL;
    if (wait_sem_count)
    {
        waitStages = (VkPipelineStageFlags *)alloca(sizeof(VkPipelineStageFlags) * wait_sem_count);
        for (uint32_t i = 0; i < wait_sem_count; i++)
            waitStages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }

    VkSubmitInfo sub = {};
    sub.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sub.pNext                = &tsi;
    sub.waitSemaphoreCount   = wait_sem_count;
    sub.pWaitSemaphores      = wait_sems;
    sub.pWaitDstStageMask    = waitStages;
    sub.commandBufferCount   = 1;
    sub.pCommandBuffers      = &cmd;
    sub.signalSemaphoreCount = 1;
    sub.pSignalSemaphores    = &sd->nv3d_timeline;

    VkResult vr = sd->real.QueueSubmit(queue, 1, &sub, VK_NULL_HANDLE);
    if (vr != VK_SUCCESS)
    {
        NV3D_LOG("QueueSubmit failed: %d", (int)vr);
        return vr;
    }

    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    HRESULT hr = ((NV3D::InterfaceVulkan *)sd->nv3d_iface)->Present(sd->nv3d_value);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
    if (ms > 250.0)
        NV3D_LOG("Present(%llu) took %.1f ms", (unsigned long long)sd->nv3d_value, ms);

    if (FAILED(hr))
    {
        s_fail_count++;
        if (s_fail_count <= 5 || (s_fail_count % 1000) == 0)
            NV3D_LOG("Present failed: 0x%08x (x%u)", (unsigned)hr, s_fail_count);
    }
    else if (s_fail_count)
    {
        NV3D_LOG("Present recovered after %u failure(s)", s_fail_count);
        s_fail_count = 0;
    }

    return VK_SUCCESS;
}

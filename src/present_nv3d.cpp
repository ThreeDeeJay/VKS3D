/*
 * present_nv3d.cpp - NVIDIA 3D Vision presentation through NV3D-Lib
 *
 * Design (fullscreen-safe)
 * ------------------------
 *  * NV3D (popup window + D3D9Ex FSE device + shared texture/fence) is PROCESS-lifetime:
 *    created once on a worker thread, kept across swapchain / VkDevice / VkInstance
 *    re-creation (DXUT apps rebuild the whole device on fullscreen toggles), shut down only
 *    after a 3 s grace period with no device or after sustained present failures.
 *  * The shared texture is always (2*W) x H where W x H is the display mode (per eye).
 *    Whatever size the app renders at is scaled into it with vkCmdBlitImage, so
 *    swapchain resizes / fullscreen switches never touch NV3D.
 *  * While NV3D is active the display mode belongs to NV3D: ChangeDisplaySettings*
 *    calls made by the app / DXVK (e.g. BasicHLSL10/11 forcing 60 Hz) are swallowed,
 *    otherwise they pull the mode out from under the D3D9 FSE device.
 *  * Before NV3D is created the desktop refresh rate is forced to a 3D Vision
 *    compatible one (90/100/110/120/144, preferring `refresh_rate` from vks3d.ini).
 *  * Before NV3D starts, the native desktop mode is restored (DXVK may have changed it).
 *  * A focus keeper hands foreground back to the app when the popup steals it and keeps
 *    the popup above the app window.  vks3d.ini [VKS3D] nv3d_tracked_game=1 selects the
 *    VRto3D-style tracked focus mode instead of the default legacy mode.
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <cstring>
#include <malloc.h>

#include "stereo_icd.h"
#include "ini.h"
#include "NV3D.hpp"

/* stereo.c: rebinds this DLL's d3d9/d3d11/dxgi/d3d12/nvapi imports to the real system DLLs
 * (otherwise NV3D-Lib talks to DXVK's copies in the app folder and crashes). */
extern "C" void vks3d_bind_system_d3d_imports(void);

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
}

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
    uint32_t ovr_w, uint32_t ovr_h,
    uint32_t app_w, uint32_t app_h,
    uint32_t *out_w, uint32_t *out_h)
{
    uint32_t w = app_w, h = app_h;

    if (ovr_w && ovr_h)
    {
        w = ovr_w;
        h = ovr_h;
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

static volatile LONG g_nv3d_modes_locked = 0;    /* 1 = NV3D active, app display/window changes are filtered */
static volatile LONG g_nv3d_swallow_logged = 0;
static volatile LONG g_host_mode  = 0;           /* 1 = D3D9 FSE device lives on the game window */
static volatile LONG g_popup_mode = 0;           /* 1 = separate NV3D popup window */
static HWND volatile g_host_hwnd  = NULL;
static bool          g_nv3d_hook_installed = false;

#ifndef HWND_TOP
#define HWND_TOP ((HWND)0)
#endif

typedef LONG (WINAPI *PFN_CDSExW)(LPCWSTR, DEVMODEW *, HWND, DWORD, LPVOID);
typedef LONG (WINAPI *PFN_CDSExA)(LPCSTR,  DEVMODEA *, HWND, DWORD, LPVOID);
typedef LONG (WINAPI *PFN_CDSW)(DEVMODEW *, DWORD);
typedef LONG (WINAPI *PFN_CDSA)(DEVMODEA *, DWORD);
typedef BOOL (WINAPI *PFN_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL (WINAPI *PFN_MoveWindow)(HWND, int, int, int, int, BOOL);
typedef BOOL (WINAPI *PFN_ShowWindow)(HWND, int);
typedef LONG (WINAPI *PFN_SetWindowLongA)(HWND, int, LONG);
typedef LONG (WINAPI *PFN_SetWindowLongW)(HWND, int, LONG);
typedef LONG_PTR (WINAPI *PFN_SetWindowLongPtrA)(HWND, int, LONG_PTR);
typedef LONG_PTR (WINAPI *PFN_SetWindowLongPtrW)(HWND, int, LONG_PTR);

static PFN_CDSExW g_real_CDSExW = NULL;
static PFN_CDSExA g_real_CDSExA = NULL;
static PFN_CDSW   g_real_CDSW   = NULL;
static PFN_CDSA   g_real_CDSA   = NULL;
static PFN_SetWindowPos       g_real_SetWindowPos       = NULL;
static PFN_MoveWindow         g_real_MoveWindow         = NULL;
static PFN_ShowWindow         g_real_ShowWindow         = NULL;
static PFN_SetWindowLongA     g_real_SetWindowLongA     = NULL;
static PFN_SetWindowLongW     g_real_SetWindowLongW     = NULL;
static PFN_SetWindowLongPtrA  g_real_SetWindowLongPtrA  = NULL;
static PFN_SetWindowLongPtrW  g_real_SetWindowLongPtrW  = NULL;

static bool nv3d_swallow(const char *api, DWORD flags)
{
    if (!g_nv3d_modes_locked)
        return false;
    if (InterlockedIncrement(&g_nv3d_swallow_logged) <= 24)
        NV3D_LOG("app call swallowed (%s flags=0x%lx) - NV3D owns the display/window state",
                 api, (unsigned long)flags);
    return true;
}

static bool nv3d_is_host(HWND h)
{
    return g_nv3d_modes_locked && g_host_mode && h && h == g_host_hwnd;
}

static bool nv3d_is_nv3d_window(HWND h)
{
    wchar_t cls[96];
    cls[0] = L'\0';
    GetClassNameW(h, cls, 96);
    return wcsstr(cls, L"NV3D") || wcsstr(cls, L"PresentWindow");
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

/* Host-window mode: the D3D9 FSE device owns the game window; the app must not fight it.
 * Popup mode: the app must never put its window ABOVE the NV3D popup (HWND_TOPMOST), that
 * uncovers a gap in which 3D Vision disengages (black/mono frame on windowed->fullscreen). */
static BOOL WINAPI hk_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT fl)
{
    if (nv3d_is_host(h))
    {
        nv3d_swallow("SetWindowPos(host)", fl);
        return TRUE;
    }
    if (g_nv3d_modes_locked && g_popup_mode && after == HWND_TOPMOST && h && !nv3d_is_nv3d_window(h))
    {
        if (InterlockedIncrement(&g_nv3d_swallow_logged) <= 24)
            NV3D_LOG("SetWindowPos(%p, HWND_TOPMOST) -> HWND_TOP (keep the NV3D popup on top)", (void *)h);
        after = HWND_TOP;
    }
    return g_real_SetWindowPos ? g_real_SetWindowPos(h, after, x, y, cx, cy, fl) : FALSE;
}
static BOOL WINAPI hk_MoveWindow(HWND h, int x, int y, int w, int hh, BOOL repaint)
{
    if (nv3d_is_host(h)) { nv3d_swallow("MoveWindow(host)", 0); return TRUE; }
    return g_real_MoveWindow ? g_real_MoveWindow(h, x, y, w, hh, repaint) : FALSE;
}
static BOOL WINAPI hk_ShowWindow(HWND h, int cmd)
{
    if (nv3d_is_host(h)) { nv3d_swallow("ShowWindow(host)", (DWORD)cmd); return IsWindowVisible(h); }
    return g_real_ShowWindow ? g_real_ShowWindow(h, cmd) : FALSE;
}
#define NV3D_GWL_STYLE   (-16)
#define NV3D_GWL_EXSTYLE (-20)
static LONG WINAPI hk_SetWindowLongA(HWND h, int idx, LONG v)
{
    if (nv3d_is_host(h) && (idx == NV3D_GWL_STYLE || idx == NV3D_GWL_EXSTYLE))
    { nv3d_swallow("SetWindowLongA(host)", (DWORD)idx); return GetWindowLongA(h, idx); }
    return g_real_SetWindowLongA ? g_real_SetWindowLongA(h, idx, v) : 0;
}
static LONG WINAPI hk_SetWindowLongW(HWND h, int idx, LONG v)
{
    if (nv3d_is_host(h) && (idx == NV3D_GWL_STYLE || idx == NV3D_GWL_EXSTYLE))
    { nv3d_swallow("SetWindowLongW(host)", (DWORD)idx); return GetWindowLongW(h, idx); }
    return g_real_SetWindowLongW ? g_real_SetWindowLongW(h, idx, v) : 0;
}
static LONG_PTR WINAPI hk_SetWindowLongPtrA(HWND h, int idx, LONG_PTR v)
{
    if (nv3d_is_host(h) && (idx == NV3D_GWL_STYLE || idx == NV3D_GWL_EXSTYLE))
    { nv3d_swallow("SetWindowLongPtrA(host)", (DWORD)idx); return GetWindowLongPtrA(h, idx); }
    return g_real_SetWindowLongPtrA ? g_real_SetWindowLongPtrA(h, idx, v) : 0;
}
static LONG_PTR WINAPI hk_SetWindowLongPtrW(HWND h, int idx, LONG_PTR v)
{
    if (nv3d_is_host(h) && (idx == NV3D_GWL_STYLE || idx == NV3D_GWL_EXSTYLE))
    { nv3d_swallow("SetWindowLongPtrW(host)", (DWORD)idx); return GetWindowLongPtrW(h, idx); }
    return g_real_SetWindowLongPtrW ? g_real_SetWindowLongPtrW(h, idx, v) : 0;
}

struct Nv3dHookEntry { const char *name; void *stub; void **real; };
static const Nv3dHookEntry k_hooks[] = {
    { "ChangeDisplaySettingsExW", (void *)hk_CDSExW,            (void **)&g_real_CDSExW },
    { "ChangeDisplaySettingsExA", (void *)hk_CDSExA,            (void **)&g_real_CDSExA },
    { "ChangeDisplaySettingsW",   (void *)hk_CDSW,              (void **)&g_real_CDSW },
    { "ChangeDisplaySettingsA",   (void *)hk_CDSA,              (void **)&g_real_CDSA },
    { "SetWindowPos",             (void *)hk_SetWindowPos,      (void **)&g_real_SetWindowPos },
    { "MoveWindow",               (void *)hk_MoveWindow,        (void **)&g_real_MoveWindow },
    { "ShowWindow",               (void *)hk_ShowWindow,        (void **)&g_real_ShowWindow },
    { "SetWindowLongA",           (void *)hk_SetWindowLongA,    (void **)&g_real_SetWindowLongA },
    { "SetWindowLongW",           (void *)hk_SetWindowLongW,    (void **)&g_real_SetWindowLongW },
    { "SetWindowLongPtrA",        (void *)hk_SetWindowLongPtrA, (void **)&g_real_SetWindowLongPtrA },
    { "SetWindowLongPtrW",        (void *)hk_SetWindowLongPtrW, (void **)&g_real_SetWindowLongPtrW },
};
static const int k_hook_count = (int)(sizeof(k_hooks) / sizeof(k_hooks[0]));

/* POD-only (SEH): rewrites the user32 imports listed in k_hooks for one module. */
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
                for (int i = 0; i < k_hook_count; i++)
                    if (*k_hooks[i].real && !strcmp(fn, k_hooks[i].name))
                    {
                        stub = k_hooks[i].stub;
                        break;
                    }
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
    for (int i = 0; i < k_hook_count; i++)
        *k_hooks[i].real = (void *)GetProcAddress(user32, k_hooks[i].name);

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
                NV3D_LOG("user32 hook: %u import(s) patched in %s", n, me.szExePath);
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    NV3D_LOG("user32 hooks installed: %u import(s) in %u module(s)",
             patched_total, patched_modules);
}

/* ------------------------------------------------------------------------- */
/* Restore the native desktop mode before NV3D starts                        */
/*                                                                           */
/* DXVK switches the desktop (e.g. 800x600@144) BEFORE our first swapchain.  */
/* NV3D-Lib's LightBoost snapshots that mode and hangs; put the native one   */
/* back first.                                                               */
/* ------------------------------------------------------------------------- */

static void nv3d_restore_native_mode(void)
{
    DEVMODEW reg, cur;
    std::memset(&reg, 0, sizeof(reg));
    std::memset(&cur, 0, sizeof(cur));
    reg.dmSize = cur.dmSize = sizeof(reg);
    if (!EnumDisplaySettingsW(NULL, ENUM_REGISTRY_SETTINGS, &reg) ||
        !EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &cur))
        return;
    if (reg.dmPelsWidth < 640 || reg.dmPelsHeight < 480)
        return;
    if (reg.dmPelsWidth        == cur.dmPelsWidth  &&
        reg.dmPelsHeight       == cur.dmPelsHeight &&
        reg.dmDisplayFrequency == cur.dmDisplayFrequency)
        return;

    NV3D_LOG("desktop mode %lux%lu@%lu differs from native %lux%lu@%lu (changed by app/DXVK) - restoring",
             cur.dmPelsWidth, cur.dmPelsHeight, cur.dmDisplayFrequency,
             reg.dmPelsWidth, reg.dmPelsHeight, reg.dmDisplayFrequency);
    LONG r = ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);   /* back to registry mode */
    NV3D_LOG("native mode restore -> %ld", (long)r);
    Sleep(500);   /* let the display settle before LightBoost looks at it */
}

/* ------------------------------------------------------------------------- */
/* Focus keeper                                                              */
/*                                                                           */
/* The NV3D popup grabs foreground when its D3D9 FSE device is created.      */
/* DXUT-style apps then see WM_ACTIVATEAPP(FALSE): they pause/minimize       */
/* (frozen frame, 160x28 swapchain).  Clicking the app afterwards raises its */
/* window above the popup (black screen).  So: hand foreground back to the   */
/* app window whenever the popup holds it, and keep the popup above the app. */
/* Never touches focus while ANOTHER process is in front (Alt+Tab works).    */
/* ------------------------------------------------------------------------- */

static HANDLE        g_keeper_thread = NULL;
static volatile LONG g_keeper_stop   = 0;

struct Nv3dFindCtx
{
    DWORD pid;
    HWND  popup;
    HWND  host;
    long  best;
};

static BOOL CALLBACK nv3d_enum_cb(HWND h, LPARAM lp)
{
    Nv3dFindCtx *c = (Nv3dFindCtx *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid)
        return TRUE;

    wchar_t cls[96];
    cls[0] = L'\0';
    GetClassNameW(h, cls, 96);
    if (wcsstr(cls, L"NV3D") || wcsstr(cls, L"PresentWindow"))
    {
        if (!c->popup) c->popup = h;
        return TRUE;
    }
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER))
        return TRUE;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
        return TRUE;
    if (wcsstr(cls, L"ConsoleWindowClass") || wcsstr(cls, L"IME") || wcsstr(cls, L"MSCTFIME"))
        return TRUE;

    RECT r;
    if (!GetWindowRect(h, &r))
        return TRUE;
    long area = (long)(r.right - r.left) * (long)(r.bottom - r.top);
    if (IsIconic(h))
        area = 1;                 /* a minimized host is still the host */
    if (area > c->best)
    {
        c->best = area;
        c->host = h;
    }
    return TRUE;
}

static bool nv3d_popup_below_host(HWND popup, HWND host)
{
    HWND w = popup;
    for (int i = 0; i < 128 && (w = GetWindow(w, GW_HWNDPREV)) != NULL; i++)
        if (w == host)
            return true;
    return false;
}

static DWORD WINAPI nv3d_keeper_proc(LPVOID)
{
    const DWORD self_pid = GetCurrentProcessId();
    ULONGLONG   last_focus = 0, last_raise = 0;
    unsigned    focus_logs = 0, raise_logs = 0;

    NV3D_LOG("focus keeper started");
    while (!g_keeper_stop)
    {
        Sleep(30);

        Nv3dFindCtx c;
        c.pid = self_pid; c.popup = NULL; c.host = NULL; c.best = 0;
        EnumWindows(nv3d_enum_cb, (LPARAM)&c);
        if (!c.popup || !c.host)
            continue;

        HWND  fg     = GetForegroundWindow();
        DWORD fg_pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fg_pid);
        if (fg_pid != self_pid)
            continue;             /* user is in another app: hands off */

        ULONGLONG now = GetTickCount64();

        /* popup stole foreground -> give it back to the app window */
        if (fg == c.popup && now - last_focus > 750)
        {
            last_focus = now;
            if (IsIconic(c.host))
                ShowWindow(c.host, SW_RESTORE);
            BOOL ok = SetForegroundWindow(c.host);
            if (focus_logs++ < 8)
                NV3D_LOG("focus keeper: foreground popup %p -> host %p (%s)",
                         (void *)c.popup, (void *)c.host, ok ? "ok" : "refused");
        }

        /* app window raised above the popup -> put the popup back on top */
        if (now - last_raise > 30 && nv3d_popup_below_host(c.popup, c.host))
        {
            last_raise = now;
            SetWindowPos(c.popup, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            if (raise_logs++ < 8)
                NV3D_LOG("focus keeper: popup %p was below host %p - raised", (void *)c.popup, (void *)c.host);
        }
    }
    NV3D_LOG("focus keeper stopped");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Host-window mode keeper                                                   */
/*                                                                           */
/* In host mode NV3D-Lib builds the D3D9 FSE device on the GAME's window and */
/* then makes that window click-through (WS_EX_TRANSPARENT|WS_EX_LAYERED),   */
/* which is right for a separate popup but wrong for the game itself:        */
/* clicks fall through to the windows behind, the window is never activated  */
/* (it also makes the library's own foreground grab a no-op) and, in true    */
/* exclusive fullscreen, the click moves focus away and the device is lost.  */
/* So: keep the click-through styles off the game window and give it         */
/* foreground when NV3D comes up.                                            */
/* ------------------------------------------------------------------------- */

static DWORD WINAPI nv3d_host_keeper_proc(LPVOID)
{
    unsigned strip_logs = 0, focus_tries = 0;
    bool focused = false;

    NV3D_LOG("host keeper started (window %p)", (void *)g_host_hwnd);
    while (!g_keeper_stop)
    {
        Sleep(50);
        HWND host = g_host_hwnd;
        if (!host || !IsWindow(host))
            continue;

        LONG_PTR ex = GetWindowLongPtrW(host, GWL_EXSTYLE);
        if (ex & (WS_EX_TRANSPARENT | WS_EX_LAYERED))
        {
            SetWindowLongPtrW(host, GWL_EXSTYLE, ex & ~(LONG_PTR)(WS_EX_TRANSPARENT | WS_EX_LAYERED));
            SetWindowPos(host, NULL, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            if (strip_logs++ < 6)
                NV3D_LOG("host keeper: removed click-through from game window %p (exstyle 0x%08lx -> 0x%08lx)",
                         (void *)host, (unsigned long)ex,
                         (unsigned long)(ex & ~(LONG_PTR)(WS_EX_TRANSPARENT | WS_EX_LAYERED)));
        }

        /* bring the game window to the foreground once NV3D is up (a few seconds of tries) */
        if (!focused && focus_tries < 100)
        {
            HWND fg = GetForegroundWindow();
            if (fg == host)
            {
                focused = true;
                NV3D_LOG("host keeper: game window %p is foreground", (void *)host);
            }
            else
            {
                DWORD fg_pid = 0;
                if (fg) GetWindowThreadProcessId(fg, &fg_pid);
                focus_tries++;
                if (IsIconic(host))
                    ShowWindow(host, SW_RESTORE);
                BOOL ok = SetForegroundWindow(host);
                if (!ok || GetForegroundWindow() != host)
                    SwitchToThisWindow(host, TRUE);
                if (focus_tries <= 3)
                    NV3D_LOG("host keeper: foreground request #%u for %p (was %p pid %lu) -> %s",
                             focus_tries, (void *)host, (void *)fg, (unsigned long)fg_pid,
                             GetForegroundWindow() == host ? "ok" : "pending");
            }
        }
    }
    NV3D_LOG("host keeper stopped");
    return 0;
}

static void nv3d_start_host_keeper(void)
{
    if (g_keeper_thread)
        return;
    g_keeper_stop = 0;
    g_keeper_thread = CreateThread(NULL, 0, nv3d_host_keeper_proc, NULL, 0, NULL);
}

static void nv3d_start_keeper(void)
{
    if (g_keeper_thread)
        return;
    g_keeper_stop = 0;
    g_keeper_thread = CreateThread(NULL, 0, nv3d_keeper_proc, NULL, 0, NULL);
}

static void nv3d_stop_keeper(void)
{
    if (!g_keeper_thread)
        return;
    g_keeper_stop = 1;
    WaitForSingleObject(g_keeper_thread, 2000);
    CloseHandle(g_keeper_thread);
    g_keeper_thread = NULL;
}

/* ------------------------------------------------------------------------- */
/* Process-lifetime NV3D                                                     */
/*                                                                           */
/* DXUT-style apps destroy and re-create the whole D3D device (and with DXVK */
/* the VkDevice, sometimes the VkInstance) when toggling fullscreen.  NV3D   */
/* (popup + D3D9 FSE device + LightBoost + shared texture/fence) does NOT    */
/* depend on the VkDevice after init, so it is created ONCE (on a worker     */
/* thread, so the app's threads are never blocked), kept across device /     */
/* instance re-creation, and only torn down after a grace period with no     */
/* device, or after sustained present failures.  Each VkDevice just          */
/* re-imports the same NT handles.                                           */
/* ------------------------------------------------------------------------- */

enum { NV3D_NONE = 0, NV3D_CREATING = 1, NV3D_READY = 2, NV3D_FAILED = 3 };

static SRWLOCK                 g_lock        = SRWLOCK_INIT;
static NV3D::InterfaceVulkan  *g_iface       = nullptr;
static HANDLE                  g_mem         = NULL;
static HANDLE                  g_fence       = NULL;
static uint32_t                g_eye_w       = 0;
static uint32_t                g_eye_h       = 0;
static uint64_t                g_value       = 0;
static volatile LONG           g_state       = NV3D_NONE;
static volatile LONG           g_dead        = 0;
static volatile LONG           g_users       = 0;
static volatile LONG           g_grace_gen   = 0;
static ULONGLONG               g_create_tick = 0;

struct Nv3dCreateReq
{
    VkInstance       inst;
    VkPhysicalDevice phys;
    VkDevice         dev;
    uint32_t         qf;
    bool             has_luid;
    uint8_t          luid[VK_LUID_SIZE];
    uint32_t         want_hz;
    uint32_t         ovr_w, ovr_h;
    uint32_t         app_w, app_h;
    bool             tracked;
    bool             recreate;
    HWND             host_hwnd;   /* non-NULL = D3D9 FSE device on the game window */
};

/* ---- window mode selection: separate popup (default) or the game's own window ---- */

/* "Disable fullscreen optimizations" (true exclusive fullscreen) is stored as the
 * DISABLEDXMAXIMIZEDWINDOWEDMODE compatibility layer for the exe. */
static bool nv3d_fso_disabled(void)
{
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH))
        return false;
    static const HKEY roots[2] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    for (int r = 0; r < 2; r++)
    {
        HKEY k = NULL;
        if (RegOpenKeyExA(roots[r],
                          "Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
                          0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
            continue;
        char data[1024];
        DWORD sz = sizeof(data) - 1, type = 0;
        LONG rc = RegQueryValueExA(k, exe, NULL, &type, (LPBYTE)data, &sz);
        RegCloseKey(k);
        if (rc == ERROR_SUCCESS && type == REG_SZ)
        {
            data[sz] = '\0';
            if (strstr(data, "DISABLEDXMAXIMIZEDWINDOWEDMODE"))
                return true;
        }
    }
    return false;
}

/* vks3d.ini [VKS3D] nv3d_host_window = auto (default) | 0 | 1
 *   0    separate NV3D popup window (works with fullscreen optimizations enabled)
 *   1    D3D9 FSE device on the game's own window (needed for true exclusive fullscreen)
 *   auto = 1 only if "Disable fullscreen optimizations" is set for the exe, else 0 */
static HWND nv3d_pick_host_window(StereoDevice *sd)
{
    char mode[24] = "auto";
    char tmp[24];
    if (ini_read_str(sd->global_ini, "VKS3D", "nv3d_host_window", tmp, sizeof(tmp)))
        strcpy(mode, tmp);
    if (ini_read_str(sd->local_ini, "VKS3D", "nv3d_host_window", tmp, sizeof(tmp)))
        strcpy(mode, tmp);

    bool fso_off = nv3d_fso_disabled();
    bool want;
    if (!_stricmp(mode, "1") || !_stricmp(mode, "true") || !_stricmp(mode, "yes") || !_stricmp(mode, "host"))
        want = true;
    else if (!_stricmp(mode, "0") || !_stricmp(mode, "false") || !_stricmp(mode, "no") || !_stricmp(mode, "popup"))
        want = false;
    else
        want = fso_off;

    Nv3dFindCtx c;
    c.pid = GetCurrentProcessId(); c.popup = NULL; c.host = NULL; c.best = 0;
    EnumWindows(nv3d_enum_cb, (LPARAM)&c);

    NV3D_LOG("window mode request: nv3d_host_window='%s' fullscreen-optimizations-disabled=%d -> %s (game window %p)",
             mode, fso_off ? 1 : 0, want ? "HOST WINDOW" : "POPUP", (void *)c.host);
    if (want && !c.host)
    {
        NV3D_LOG("no game window found - using popup mode");
        return NULL;
    }
    return want ? c.host : NULL;
}

/* Give a freshly started app a moment to become visible and foreground before NV3D engages. */
static void nv3d_wait_for_app_window(void)
{
    const DWORD self = GetCurrentProcessId();
    for (int i = 0; i < 60; i++)             /* up to ~3 s */
    {
        Nv3dFindCtx c;
        c.pid = self; c.popup = NULL; c.host = NULL; c.best = 0;
        EnumWindows(nv3d_enum_cb, (LPARAM)&c);
        if (c.host && IsWindowVisible(c.host) && !IsIconic(c.host))
        {
            HWND fg = GetForegroundWindow();
            DWORD fg_pid = 0;
            if (fg) GetWindowThreadProcessId(fg, &fg_pid);
            if (fg_pid == self || i >= 40)
            {
                NV3D_LOG("app window %p ready (foreground=%d) after %d ms", (void *)c.host, fg_pid == self ? 1 : 0, i * 50);
                return;
            }
        }
        Sleep(50);
    }
    NV3D_LOG("app window wait timed out - continuing");
}

static void nv3d_shutdown_global(void)
{
    nv3d_stop_keeper();
    InterlockedExchange(&g_nv3d_modes_locked, 0);
    InterlockedExchange(&g_host_mode, 0);
    InterlockedExchange(&g_popup_mode, 0);
    g_host_hwnd = NULL;

    AcquireSRWLockExclusive(&g_lock);
    NV3D::InterfaceVulkan *i = g_iface;
    g_iface = nullptr;
    g_mem   = NULL;
    g_fence = NULL;
    InterlockedExchange(&g_state, NV3D_NONE);
    InterlockedExchange(&g_dead, 0);
    if (i)
    {
        NV3D_LOG("shutting down NV3D");
        i->Delete();
    }
    ReleaseSRWLockExclusive(&g_lock);
}

static DWORD WINAPI nv3d_create_proc(LPVOID arg)
{
    Nv3dCreateReq *rq = (Nv3dCreateReq *)arg;
    ULONGLONG t0 = GetTickCount64();
    NV3D_LOG("create: begin%s", rq->recreate ? " (recreate after failure)" : "");

    if (rq->recreate)
        nv3d_shutdown_global();

    nv3d_wait_for_app_window();

    /* Undo mode changes the app/DXVK made before us, then make sure the refresh rate is
     * one 3D Vision can use (90/100/110/120/144 Hz). */
    nv3d_restore_native_mode();
    nv3d_force_3dv_refresh(rq->want_hz);

    uint32_t eye_w = rq->app_w, eye_h = rq->app_h;
    nv3d_pick_eye_size(rq->ovr_w, rq->ovr_h, rq->app_w, rq->app_h, &eye_w, &eye_h);
    NV3D_LOG("create: per-eye %ux%u (first app swapchain %ux%u)", eye_w, eye_h, rq->app_w, rq->app_h);

    NV3D::InitParams params = {};
    if (rq->tracked)
        params.tracked_game_pid = GetCurrentProcessId();
    if (rq->has_luid)
    {
        params.has_external_luid = true;
        static_assert(sizeof(LUID) == VK_LUID_SIZE, "LUID mismatch");
        std::memcpy(&params.external_luid, rq->luid, VK_LUID_SIZE);
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

    NV3D::InterfaceVulkan *iface = nullptr;
    HANDLE mem_handle = NULL, fence_handle = NULL;
    HRESULT hr = E_FAIL;
    for (int attempt = 0; attempt < 2; attempt++)
    {
        params.host_hwnd = rq->host_hwnd;       /* NULL = library-owned popup */
        iface = nullptr;
        mem_handle = fence_handle = NULL;
        hr = NV3D::CreateInterfaceVulkan(rq->inst, rq->phys, rq->dev, rq->qf, &params, &iface);
        NV3D_LOG("create: CreateInterfaceVulkan(%s) hr=0x%08x iface=%p (+%llu ms)",
                 rq->host_hwnd ? "host window" : "popup",
                 (unsigned)hr, (void *)iface, (unsigned long long)(GetTickCount64() - t0));
        if (SUCCEEDED(hr) && iface)
        {
            hr = iface->InitSharedResources(eye_w * 2, eye_h, 87 /* DXGI_FORMAT_B8G8R8A8_UNORM */,
                                            &mem_handle, &fence_handle);
            NV3D_LOG("create: InitSharedResources hr=0x%08x (+%llu ms)",
                     (unsigned)hr, (unsigned long long)(GetTickCount64() - t0));
            if (SUCCEEDED(hr))
                break;
            iface->Delete();
            iface = nullptr;
        }
        if (rq->host_hwnd)
        {
            NV3D_LOG("create: host-window mode failed - falling back to popup mode");
            rq->host_hwnd = NULL;
            continue;
        }
        break;
    }
    if (!iface || FAILED(hr))
    {
        InterlockedExchange(&g_state, NV3D_FAILED);
        free(rq);
        return 0;
    }

    AcquireSRWLockExclusive(&g_lock);
    g_iface = iface;
    g_mem   = mem_handle;
    g_fence = fence_handle;
    g_eye_w = eye_w;
    g_eye_h = eye_h;
    g_value = 0;
    InterlockedExchange(&g_dead, 0);
    ReleaseSRWLockExclusive(&g_lock);

    /* From now on NV3D owns the display mode. */
    nv3d_install_display_mode_hook();
    g_host_hwnd = rq->host_hwnd;                       /* non-NULL only if host mode is really in use */
    InterlockedExchange(&g_host_mode,  rq->host_hwnd ? 1 : 0);
    InterlockedExchange(&g_popup_mode, rq->host_hwnd ? 0 : 1);
    InterlockedExchange(&g_nv3d_modes_locked, 1);
    NV3D_LOG("window mode: %s", rq->host_hwnd ? "HOST WINDOW (D3D9 FSE on the game window)" : "POPUP (separate NV3D window)");
    if (!rq->host_hwnd)
        nv3d_start_keeper();
    else
        nv3d_start_host_keeper();

    InterlockedExchange(&g_state, NV3D_READY);
    NV3D_LOG("ready: shared texture %ux%u (per eye %ux%u), display mode locked (+%llu ms)",
             eye_w * 2, eye_h, eye_w, eye_h, (unsigned long long)(GetTickCount64() - t0));
    free(rq);
    return 0;
}

static void nv3d_start_create(StereoDevice *sd, uint32_t width, uint32_t height, bool recreate)
{
    Nv3dCreateReq *rq = (Nv3dCreateReq *)calloc(1, sizeof(*rq));
    if (!rq)
    {
        InterlockedExchange(&g_state, NV3D_FAILED);
        return;
    }
    rq->inst  = sd->si->real_instance;
    rq->phys  = sd->real_physdev;
    rq->dev   = sd->real_device;
    rq->qf    = sd->gfx_qf;
    rq->want_hz = sd->stereo.refresh_rate;
    rq->ovr_w = sd->stereo.override_width;
    rq->ovr_h = sd->stereo.override_height;
    rq->app_w = width;
    rq->app_h = height;
    rq->recreate = recreate;

    const bool tracked = ini_flag(sd, "nv3d_tracked_game", false);
    NV3D_LOG("focus mode: %s (nv3d_tracked_game=%d)", tracked ? "tracked" : "legacy", tracked ? 1 : 0);
    rq->tracked = tracked;
    rq->host_hwnd = nv3d_pick_host_window(sd);

    VkPhysicalDeviceIDProperties id = {};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 props2 = {};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &id;
    sd->si->real.GetPhysicalDeviceProperties2(sd->real_physdev, &props2);
    if (id.deviceLUIDValid)
    {
        rq->has_luid = true;
        std::memcpy(rq->luid, id.deviceLUID, VK_LUID_SIZE);
    }

    /* MUST happen before NV3D-Lib touches D3D9/D3D11/DXGI/NvAPI (DXVK processes) */
    vks3d_bind_system_d3d_imports();

    g_create_tick = GetTickCount64();
    HANDLE th = CreateThread(NULL, 0, nv3d_create_proc, rq, 0, NULL);
    if (!th)
    {
        NV3D_LOG("create: CreateThread failed (%lu)", (unsigned long)GetLastError());
        free(rq);
        InterlockedExchange(&g_state, NV3D_FAILED);
        return;
    }
    CloseHandle(th);
}

static DWORD WINAPI nv3d_grace_proc(LPVOID arg)
{
    LONG gen = (LONG)(INT_PTR)arg;
    for (int i = 0; i < 30; i++)
    {
        Sleep(100);
        if (g_grace_gen != gen || g_users > 0)
            return 0;
    }
    if (g_grace_gen == gen && g_users <= 0)
    {
        NV3D_LOG("no Vulkan device for 3 s - shutting NV3D down");
        nv3d_shutdown_global();
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Per-VkDevice import of the shared texture / fence                         */
/* ------------------------------------------------------------------------- */

static void nv3d_release_device(StereoDevice *sd)
{
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
    sd->nv3d_ok         = false;
    sd->nv3d_mem_handle = NULL;
    sd->nv3d_fence_handle = NULL;
    sd->nv3d_width      = 0;
    sd->nv3d_height     = 0;
}

/* caller holds g_lock (shared) and g_state == READY */
static bool nv3d_import_device(StereoDevice *sd)
{
    nv3d_release_device(sd);

    const uint32_t eye_w = g_eye_w, eye_h = g_eye_h;

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
        NV3D_LOG("import: CreateImage failed");
        nv3d_release_device(sd);
        return false;
    }

    VkMemoryRequirements mr;
    sd->real.GetImageMemoryRequirements(sd->real_device, sd->nv3d_image, &mr);
    uint32_t mem_type = find_memory_type(sd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX)
    {
        NV3D_LOG("import: no compatible memory type");
        nv3d_release_device(sd);
        return false;
    }

    VkImportMemoryWin32HandleInfoKHR import_info = {};
    import_info.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    import_info.handle     = g_mem;

    VkMemoryAllocateInfo ai = {};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.pNext           = &import_info;
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = mem_type;
    if (sd->real.AllocateMemory(sd->real_device, &ai, NULL, &sd->nv3d_memory) != VK_SUCCESS)
    {
        NV3D_LOG("import: AllocateMemory failed");
        nv3d_release_device(sd);
        return false;
    }
    if (sd->real.BindImageMemory(sd->real_device, sd->nv3d_image, sd->nv3d_memory, 0) != VK_SUCCESS)
    {
        NV3D_LOG("import: BindImageMemory failed");
        nv3d_release_device(sd);
        return false;
    }

    VkSemaphoreTypeCreateInfo sem_type = {};
    sem_type.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    sem_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    sem_type.initialValue  = 0;
    VkSemaphoreCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.pNext = &sem_type;
    if (sd->real.CreateSemaphore(sd->real_device, &sci, NULL, &sd->nv3d_timeline) != VK_SUCCESS)
    {
        NV3D_LOG("import: CreateSemaphore failed");
        nv3d_release_device(sd);
        return false;
    }

    VkImportSemaphoreWin32HandleInfoKHR import_sem = {};
    import_sem.sType      = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
    import_sem.semaphore  = sd->nv3d_timeline;
    import_sem.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    import_sem.handle     = g_fence;
    if (!sd->real.ImportSemaphoreWin32HandleKHR ||
        sd->real.ImportSemaphoreWin32HandleKHR(sd->real_device, &import_sem) != VK_SUCCESS)
    {
        NV3D_LOG("import: ImportSemaphoreWin32HandleKHR failed");
        nv3d_release_device(sd);
        return false;
    }

    sd->nv3d_mem_handle   = g_mem;
    sd->nv3d_fence_handle = g_fence;
    sd->nv3d_width        = eye_w * 2;
    sd->nv3d_height       = eye_h;
    sd->nv3d_ok           = true;
    NV3D_LOG("import: device %p imported shared texture %ux%u", (void *)sd->real_device, eye_w * 2, eye_h);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

/* Called for every NV3D swapchain creation.  Never blocks and never restarts NV3D
 * unless it failed / died. */
extern "C" bool nv3d_init(
    StereoDevice *sd,
    uint32_t width,
    uint32_t height)
{
    InterlockedIncrement(&g_grace_gen);              /* cancel a pending grace shutdown */

    if (sd->nv3d_iface != (void *)sd)                /* register this device as a user */
    {
        sd->nv3d_iface = (void *)sd;
        InterlockedIncrement(&g_users);
    }

    LONG st = g_state;
    bool recreate = false;
    if (st == NV3D_READY && g_dead)
    {
        NV3D_LOG("nv3d_init: NV3D was marked dead (Present failing) - recreating");
        if (InterlockedCompareExchange(&g_state, NV3D_CREATING, NV3D_READY) == NV3D_READY)
        {
            nv3d_start_create(sd, width, height, true);
        }
        return true;
    }
    if (st == NV3D_NONE || st == NV3D_FAILED)
    {
        if (InterlockedCompareExchange(&g_state, NV3D_CREATING, st) == st)
        {
            NV3D_LOG("nv3d_init: starting NV3D (app swapchain %ux%u)", width, height);
            nv3d_start_create(sd, width, height, recreate);
        }
        return true;
    }
    NV3D_LOG("nv3d_init: NV3D %s (%ux%u per eye), app swapchain %ux%u -> scaled",
             st == NV3D_READY ? "already running" : "still starting",
             g_eye_w, g_eye_h, width, height);
    return true;
}

/* Called when a swapchain / the device goes away: frees only the Vulkan-side imports.
 * NV3D itself stays alive (grace period) so the app's device re-creation doesn't reset it. */
extern "C" void nv3d_destroy(
    StereoDevice *sd)
{
    if (!sd)
        return;

    if (sd->real_device && sd->real.DeviceWaitIdle &&
        (sd->nv3d_image || sd->nv3d_timeline))
        sd->real.DeviceWaitIdle(sd->real_device);

    AcquireSRWLockShared(&g_lock);
    nv3d_release_device(sd);
    ReleaseSRWLockShared(&g_lock);

    if (sd->nv3d_iface == (void *)sd)
    {
        sd->nv3d_iface = NULL;
        if (InterlockedDecrement(&g_users) <= 0)
        {
            LONG gen = InterlockedIncrement(&g_grace_gen);
            HANDLE th = CreateThread(NULL, 0, nv3d_grace_proc, (LPVOID)(INT_PTR)gen, 0, NULL);
            if (th) CloseHandle(th);
            NV3D_LOG("last Vulkan device released - NV3D stays up for a 3 s grace period");
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Present                                                                    */
/* ------------------------------------------------------------------------- */

/* Not ready: still consume the app's render-complete semaphores so they can be reused. */
static void nv3d_consume_waits(StereoDevice *sd, VkQueue queue,
                               uint32_t n, const VkSemaphore *sems)
{
    if (!n)
        return;
    VkPipelineStageFlags *stages = (VkPipelineStageFlags *)alloca(sizeof(VkPipelineStageFlags) * n);
    for (uint32_t i = 0; i < n; i++)
        stages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo sub = {};
    sub.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sub.waitSemaphoreCount = n;
    sub.pWaitSemaphores    = sems;
    sub.pWaitDstStageMask  = stages;
    sd->real.QueueSubmit(queue, 1, &sub, VK_NULL_HANDLE);
}

extern "C" VkResult nv3d_present(
    StereoDevice *sd,
    StereoSwapchain *sc,
    VkQueue queue,
    uint32_t wait_sem_count,
    const VkSemaphore *wait_sems)
{
    static uint32_t  s_fail_count = 0;
    static bool      s_warned_fmt = false;
    static ULONGLONG s_last_wait_log = 0;

    if (g_state != NV3D_READY)
    {
        ULONGLONG now = GetTickCount64();
        if (now - s_last_wait_log > 2000)
        {
            s_last_wait_log = now;
            NV3D_LOG("present skipped: NV3D state=%ld (creating for %llu ms)",
                     (long)g_state, (unsigned long long)(now - g_create_tick));
        }
        nv3d_consume_waits(sd, queue, wait_sem_count, wait_sems);
        return VK_SUCCESS;
    }

    if (!sc->barrier_cmds || !sc->barrier_cmds[0] || !sc->stereo_images)
    {
        nv3d_consume_waits(sd, queue, wait_sem_count, wait_sems);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    AcquireSRWLockShared(&g_lock);
    if (!g_iface || g_state != NV3D_READY)
    {
        ReleaseSRWLockShared(&g_lock);
        nv3d_consume_waits(sd, queue, wait_sem_count, wait_sems);
        return VK_SUCCESS;
    }

    /* (Re)import when this VkDevice is new or NV3D was recreated */
    if (!sd->nv3d_ok || sd->nv3d_mem_handle != g_mem)
    {
        if (!nv3d_import_device(sd))
        {
            ReleaseSRWLockShared(&g_lock);
            nv3d_consume_waits(sd, queue, wait_sem_count, wait_sems);
            return VK_SUCCESS;
        }
    }

    VkCommandBuffer cmd = sc->barrier_cmds[0];

    const uint32_t eye_w = g_eye_w;
    const uint32_t eye_h = g_eye_h;
    const bool     scaled = (sc->app_width != eye_w) || (sc->app_height != eye_h);
    const bool     bgra   = is_bgra_format(sc->format);
    static uint32_t s_last_w = 0, s_last_h = 0; static VkFormat s_last_fmt = VK_FORMAT_UNDEFINED;
    if (s_last_w != sc->app_width || s_last_h != sc->app_height || s_last_fmt != sc->format)
    {
        s_last_w = sc->app_width; s_last_h = sc->app_height; s_last_fmt = sc->format;
        NV3D_LOG("present path: app %ux%u fmt=%d -> eye %ux%u via %s",
                 sc->app_width, sc->app_height, (int)sc->format, eye_w, eye_h,
                 (!scaled && bgra) ? "raw copy (layer0->left, layer1->right)" : "scaling blit (layer0->left, layer1->right)");
    }

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

    /* NV3D imported texture -> transfer destination (fully overwritten every frame) */
    cmd_image_barrier(
        sd, cmd, sd->nv3d_image, 1,
        VK_IMAGE_LAYOUT_UNDEFINED,
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

    sd->nv3d_value = ++g_value;   /* continues across devices: the D3D fence outlives them */

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
        ReleaseSRWLockShared(&g_lock);
        NV3D_LOG("QueueSubmit failed: %d", (int)vr);
        return vr;
    }

    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    HRESULT hr = g_iface->Present(sd->nv3d_value);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    ReleaseSRWLockShared(&g_lock);

    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
    if (ms > 250.0)
        NV3D_LOG("Present(%llu) took %.1f ms", (unsigned long long)sd->nv3d_value, ms);

    if (FAILED(hr))
    {
        s_fail_count++;
        if (s_fail_count <= 5 || (s_fail_count % 1000) == 0)
            NV3D_LOG("Present failed: 0x%08x (x%u)", (unsigned)hr, s_fail_count);
        if (s_fail_count == 240)
        {
            NV3D_LOG("Present failed 240x in a row - NV3D marked dead, will be recreated with the next swapchain");
            InterlockedExchange(&g_dead, 1);
        }
    }
    else if (s_fail_count)
    {
        NV3D_LOG("Present recovered after %u failure(s)", s_fail_count);
        s_fail_count = 0;
    }

    return VK_SUCCESS;
}

#pragma once
/*
 * platform.h — OS abstraction layer for VKS3D
 *
 * Provides unified wrappers around:
 *   - Dynamic library loading  (dlopen / LoadLibrary)
 *   - Mutual exclusion         (pthread_mutex / CRITICAL_SECTION)
 *   - Environment variables    (getenv / GetEnvironmentVariable)
 *   - DLL export decoration    (visibility / __declspec(dllexport))
 *   - Compiler helpers         (inline, likely/unlikely)
 *
 * All platform-specific code is contained here so the rest of the
 * source tree is OS-agnostic C11.
 */

#ifdef _WIN32
/* ── Windows ─────────────────────────────────────────────────────────────── */
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <winreg.h>
#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#  include <stdbool.h>

/* ── DLL export ──────────────────────────────────────────────────────────── */
#  define STEREO_EXPORT  __declspec(dllexport)

/* ── Dynamic library ─────────────────────────────────────────────────────── */
typedef HMODULE stereo_dl_t;
#  define STEREO_DL_NULL  NULL

static inline stereo_dl_t stereo_dl_open(const char *path)
{
    return LoadLibraryA(path);
}
static inline void *stereo_dl_sym(stereo_dl_t h, const char *name)
{
    return (void *)(uintptr_t)GetProcAddress(h, name);
}
static inline void stereo_dl_close(stereo_dl_t h)
{
    if (h) FreeLibrary(h);
}
static inline const char *stereo_dl_error(void)
{
    static char buf[256];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, GetLastError(), 0, buf, sizeof(buf), NULL);
    return buf;
}

/* ── Mutex ───────────────────────────────────────────────────────────────── */
typedef CRITICAL_SECTION stereo_mutex_t;

static inline void stereo_mutex_init(stereo_mutex_t *m)
{
    InitializeCriticalSection(m);
}
static inline void stereo_mutex_lock(stereo_mutex_t *m)
{
    EnterCriticalSection(m);
}
static inline void stereo_mutex_unlock(stereo_mutex_t *m)
{
    LeaveCriticalSection(m);
}
static inline void stereo_mutex_destroy(stereo_mutex_t *m)
{
    DeleteCriticalSection(m);
}

/* ── Environment variables ───────────────────────────────────────────────── */
/*
 * stereo_getenv — returns a pointer to a static buffer (not thread-safe for
 * simultaneous calls with different names, but fine for initialisation).
 * Returns NULL if the variable is not set.
 */
static inline const char *stereo_getenv(const char *name)
{
    /* Use a per-call static buffer large enough for paths */
    static char buf[4096];
    DWORD n = GetEnvironmentVariableA(name, buf, (DWORD)sizeof(buf));
    if (n == 0 || n >= (DWORD)sizeof(buf))
        return NULL;
    return buf;
}

static inline void stereo_setenv(const char *name, const char *value)
{
    SetEnvironmentVariableA(name, value);
}

/* ── Bit-width string (used in log messages / filenames) ─────────────────── */
#  ifdef _WIN64
#    define STEREO_ARCH_STR  "x64"
#    define STEREO_ARCH_BITS 64
#  else
#    define STEREO_ARCH_STR  "x86"
#    define STEREO_ARCH_BITS 32
#  endif

/* ── Windows registry helpers ────────────────────────────────────────────── */
/*
 * On Windows the Vulkan loader discovers ICDs from the registry:
 *
 *   64-bit:  HKLM\SOFTWARE\Khronos\Vulkan\Drivers
 *   32-bit:  HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers
 *
 * Each value name is the path to an ICD JSON manifest; the value data
 * is DWORD 0 (enabled).  The JSON contains "library_path" pointing to
 * the actual ICD DLL.
 *
 * stereo_registry_enum_icd_jsons enumerates these paths into a
 * caller-supplied buffer of string pointers (NULL-terminated).
 * Caller must free() each string and the array itself.
 */
static inline char **stereo_registry_enum_icd_jsons(void)
{
    static const char *key_paths[] = {
#  ifdef _WIN64
        "SOFTWARE\\Khronos\\Vulkan\\Drivers",
#  else
        "SOFTWARE\\WOW6432Node\\Khronos\\Vulkan\\Drivers",
        "SOFTWARE\\Khronos\\Vulkan\\Drivers",   /* fallback */
#  endif
        NULL
    };

    /* Collect all JSON paths into a dynamic list */
    char **list     = NULL;
    size_t list_cap = 0;
    size_t list_len = 0;

    for (int ki = 0; key_paths[ki]; ki++) {
        HKEY hkey = NULL;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key_paths[ki],
                          0, KEY_READ, &hkey) != ERROR_SUCCESS)
            continue;

        DWORD idx = 0;
        char  val_name[2048];
        DWORD val_name_len;
        DWORD val_type;
        DWORD val_data;
        DWORD val_data_size;

        while (1) {
            val_name_len  = (DWORD)sizeof(val_name);
            val_data_size = (DWORD)sizeof(val_data);
            LSTATUS st = RegEnumValueA(hkey, idx++, val_name, &val_name_len,
                                       NULL, &val_type,
                                       (LPBYTE)&val_data, &val_data_size);
            if (st == ERROR_NO_MORE_ITEMS) break;
            if (st != ERROR_SUCCESS)       continue;
            /* Value must be DWORD 0 (enabled) */
            if (val_type != REG_DWORD || val_data != 0) continue;

            /* val_name is the JSON path */
            if (list_len + 1 >= list_cap) {
                list_cap = list_cap ? list_cap * 2 : 16;
                list     = (char **)realloc(list,
                               (list_cap + 1) * sizeof(char *));
                if (!list) break;
            }
            list[list_len++] = _strdup(val_name);
            list[list_len]   = NULL;
        }
        RegCloseKey(hkey);
    }
    return list;  /* caller frees: each string + the array */
}

/*
 * stereo_json_read_library_path — reads "library_path" from a minimal
 * ICD JSON file.  Very minimal parser; no full JSON library needed.
 * Returns a heap-allocated string or NULL on failure.
 */
static inline char *stereo_json_read_library_path(const char *json_path)
{
    FILE *f = fopen(json_path, "r");
    if (!f) return NULL;

    char line[2048];
    char *result = NULL;
    while (fgets(line, sizeof(line), f)) {
        /* Look for: "library_path" : "..." */
        char *key = strstr(line, "library_path");
        if (!key) continue;
        char *q1 = strchr(key + 12, '"');
        if (!q1) continue;
        q1++;
        char *q2 = strchr(q1, '"');
        if (!q2) continue;
        *q2    = '\0';
        result = _strdup(q1);
        break;
    }
    fclose(f);
    return result;
}

/*
 * stereo_find_opengl_driver_icd
 * ─────────────────────────────
 * Searches Windows registry keys that store the OpenGL ICD DLL path.
 * On NVIDIA, the OpenGL driver DLL (nvoglv64.dll / nvoglv32.dll) is also
 * the Vulkan ICD, so these keys reliably locate the correct DLL.
 *
 * Keys checked (64-bit build checks 64-bit values; 32-bit checks WoW keys):
 *
 *   HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\MSOGL
 *     → REG_SZ value "DLL"
 *
 *   HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\####
 *     → REG_SZ value "OpenGLDriverName"  (64-bit)
 *     → REG_SZ value "OpenGLDriverNameWoW"  (32-bit)
 *
 *   HKLM\SYSTEM\CurrentControlSet\Control\Video\{*}\####
 *     → REG_SZ value "OpenGLDriverName"  (64-bit)
 *     → REG_SZ value "OpenGLDriverNameWoW"  (32-bit)
 *
 *   Also ControlSet001, ControlSet002 as fallback.
 *
 * Returns a heap-allocated path to the first existing DLL found, or NULL.
 * Caller must free() the result.
 */
static inline char *stereo_find_opengl_driver_icd(void)
{
#  ifdef _WIN64
#    define OGLI_VALNAME   "OpenGLDriverName"
#    define OGLI_SOFTKEY   "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers\\MSOGL"
#  else
#    define OGLI_VALNAME   "OpenGLDriverNameWoW"
#    define OGLI_SOFTKEY   "SOFTWARE\\WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers\\MSOGL"
#  endif

    /* Resolve a DLL name/path: if it has no backslash, look in System32 */
    static char ogli_buf[MAX_PATH];
    #define OGLI_TRY(dll) do { \
        if (!(dll) || !(dll)[0]) break; \
        const char *_d = (dll); \
        if (strchr(_d, '\\') || strchr(_d, '/')) { \
            if (GetFileAttributesA(_d) != INVALID_FILE_ATTRIBUTES) \
                return _strdup(_d); \
        } else { \
            /* bare name — look in System32 */ \
            snprintf(ogli_buf, sizeof(ogli_buf), \
                     "C:\\Windows\\System32\\%s", _d); \
            if (GetFileAttributesA(ogli_buf) != INVALID_FILE_ATTRIBUTES) \
                return _strdup(ogli_buf); \
        } \
    } while (0)

    /* Helper: read one REG_SZ value and try it */
    #define OGLI_READ_SZ(hk, name) do { \
        char _vbuf[MAX_PATH]; DWORD _vlen = sizeof(_vbuf), _vtype; \
        if (RegQueryValueExA((hk), (name), NULL, &_vtype, \
                             (LPBYTE)_vbuf, &_vlen) == ERROR_SUCCESS \
            && _vtype == REG_SZ && _vlen > 1) { \
            _vbuf[_vlen - 1] = '\0'; \
            OGLI_TRY(_vbuf); \
        } \
    } while (0)

    /* 1. MSOGL direct key */
    {
        HKEY hk = NULL;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, OGLI_SOFTKEY,
                          0, KEY_READ, &hk) == ERROR_SUCCESS) {
            OGLI_READ_SZ(hk, "DLL");
            RegCloseKey(hk);
        }
    }

    /* 2. Display adapter class key + Video key, for each ControlSet */
    static const char *s_csets[] = {
        "CurrentControlSet", "ControlSet001", "ControlSet002", "ControlSet003", NULL
    };
    static const char *s_subtree[] = {
        "Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}",
        "Control\\Video",
        NULL
    };

    for (int ci = 0; s_csets[ci]; ci++) {
        for (int ti = 0; s_subtree[ti]; ti++) {
            char base[256];
            snprintf(base, sizeof(base), "SYSTEM\\%s\\%s",
                     s_csets[ci], s_subtree[ti]);

            HKEY hbase = NULL;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, base,
                              0, KEY_READ | KEY_ENUMERATE_SUB_KEYS,
                              &hbase) != ERROR_SUCCESS)
                continue;

            /* Enumerate immediate subkeys (adapter GUIDs or "0000","0001"...) */
            DWORD si2 = 0;
            char  sk1[256];
            DWORD sk1_len;
            while (1) {
                sk1_len = (DWORD)sizeof(sk1);
                if (RegEnumKeyExA(hbase, si2++, sk1, &sk1_len,
                                  NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                    break;

                char sub1[512];
                snprintf(sub1, sizeof(sub1), "%s\\%s", base, sk1);
                HKEY h1 = NULL;
                if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, sub1,
                                  0, KEY_READ | KEY_ENUMERATE_SUB_KEYS,
                                  &h1) != ERROR_SUCCESS)
                    continue;

                /* Try value directly on this key */
                OGLI_READ_SZ(h1, OGLI_VALNAME);

                /* Also enumerate one level deeper (for Video\{GUID}\0000 layout) */
                DWORD si3 = 0;
                char  sk2[64];
                DWORD sk2_len;
                while (1) {
                    sk2_len = (DWORD)sizeof(sk2);
                    if (RegEnumKeyExA(h1, si3++, sk2, &sk2_len,
                                      NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                        break;
                    char sub2[640];
                    snprintf(sub2, sizeof(sub2), "%s\\%s", sub1, sk2);
                    HKEY h2 = NULL;
                    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, sub2,
                                      0, KEY_READ, &h2) == ERROR_SUCCESS) {
                        OGLI_READ_SZ(h2, OGLI_VALNAME);
                        RegCloseKey(h2);
                    }
                }
                RegCloseKey(h1);
            }
            RegCloseKey(hbase);
        }
    }

#  undef OGLI_TRY
#  undef OGLI_READ_SZ
#  undef OGLI_VALNAME
#  undef OGLI_SOFTKEY

    return NULL;  /* not found */
}

/* ── Logging ─────────────────────────────────────────────────────────────── */
/*
 * Logging is opt-in via the environment variable STEREO_LOGFILE_PATH.
 *
 * If STEREO_LOGFILE_PATH is NOT set  →  all STEREO_LOG / STEREO_ERR calls
 *                                        compile to no-ops (zero overhead).
 *
 * If STEREO_LOGFILE_PATH is set to a file path  →  every log line is written
 *   to that file AND to OutputDebugStringA (visible in Sysinternals DebugView
 *   and the VS debugger output window).
 *
 * Special values for STEREO_LOGFILE_PATH:
 *   "debugview"  →  OutputDebugStringA only, no file
 *   any path     →  file (truncated on each DLL load) + OutputDebugStringA
 *
 * vks3d_log_write() is safe to call from DllMain:
 *   - no heap allocation
 *   - stack buffers only
 *   - WriteFile is atomic for small writes (no extra lock needed)
 */

/* Defined once in stereo.c — extern so all translation units share the
 * same state.  (Declaring them static here would give each .c file its own
 * copy, so DllMain's vks3d_log_open() would only enable logging in stereo.c
 * while every other file's g_vks3d_log_enabled remained 0.) */
#ifdef STEREO_LOG_DEFINE_GLOBALS
HANDLE g_vks3d_log_handle  = INVALID_HANDLE_VALUE;
int    g_vks3d_log_enabled = 0;
#else
extern HANDLE g_vks3d_log_handle;
extern int    g_vks3d_log_enabled;
#endif

/*
 * vks3d_log_open — must be called from DllMain DLL_PROCESS_ATTACH.
 * Reads STEREO_LOGFILE_PATH WITHOUT using the CRT getenv() (which may not
 * be ready) and WITHOUT heap allocation.
 */
static inline void vks3d_log_open(void)
{
    /* Already initialised? */
    if (g_vks3d_log_enabled) return;

    /* Read STEREO_LOGFILE_PATH using the Win32 API directly */
    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("STEREO_LOGFILE_PATH", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        /* Variable not set (or too long) — logging disabled */
        g_vks3d_log_enabled = 0;
        return;
    }

    /* Mark as enabled regardless of whether the file opens — we still
     * want OutputDebugStringA output even if the file path is bad. */
    g_vks3d_log_enabled = 1;

    /* Special value "debugview": OutputDebugStringA only, no file */
    if (lstrcmpiA(path, "debugview") == 0) {
        g_vks3d_log_handle = INVALID_HANDLE_VALUE;
        return;
    }

    g_vks3d_log_handle = CreateFileA(
        path,
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        CREATE_ALWAYS,           /* truncate on each new DLL load */
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        NULL);
    /* If CreateFileA fails, OutputDebugStringA still works */
}

static inline void vks3d_log_write(const char *msg)
{
    OutputDebugStringA(msg);
    if (g_vks3d_log_handle != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(g_vks3d_log_handle, msg, (DWORD)lstrlenA(msg), &written, NULL);
    }
}

#  include <stdarg.h>

/* vks3d_logf — printf-style, stack-only, safe from DllMain */
static inline void vks3d_logf(const char *prefix, const char *fmt, ...)
{
    char buf[2048];
    char msg[2048 + 64];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)(sizeof(buf) - 1))
        n = (int)(sizeof(buf) - 2);
    buf[n] = '\0';
    _snprintf(msg, sizeof(msg) - 1, "%s%s\n", prefix, buf);
    msg[sizeof(msg) - 1] = '\0';
    vks3d_log_write(msg);
}

/* STEREO_LOG / STEREO_ERR: no-ops when logging is disabled */
#  define STEREO_LOG(fmt, ...) \
    do { if (g_vks3d_log_enabled) \
        vks3d_logf("[VKS3D " STEREO_ARCH_STR "] ", fmt, ##__VA_ARGS__); \
    } while (0)
#  define STEREO_ERR(fmt, ...) \
    do { if (g_vks3d_log_enabled) \
        vks3d_logf("[VKS3D " STEREO_ARCH_STR " ERROR] ", fmt, ##__VA_ARGS__); \
    } while (0)

#else
/* ── Linux / macOS ────────────────────────────────────────────────────────── */
#  include <dlfcn.h>
#  include <pthread.h>
#  include <stdlib.h>
#  include <string.h>
#  include <stdio.h>
#  include <stdint.h>
#  include <stdbool.h>

#  define STEREO_EXPORT  __attribute__((visibility("default")))

typedef void *stereo_dl_t;
#  define STEREO_DL_NULL  NULL

static inline stereo_dl_t stereo_dl_open(const char *path)
{
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
static inline void *stereo_dl_sym(stereo_dl_t h, const char *name)
{
    return dlsym(h, name);
}
static inline void stereo_dl_close(stereo_dl_t h)
{
    if (h) dlclose(h);
}
static inline const char *stereo_dl_error(void)
{
    return dlerror();
}

typedef pthread_mutex_t stereo_mutex_t;

static inline void stereo_mutex_init(stereo_mutex_t *m)
{
    pthread_mutex_init(m, NULL);
}
static inline void stereo_mutex_lock(stereo_mutex_t *m)
{
    pthread_mutex_lock(m);
}
static inline void stereo_mutex_unlock(stereo_mutex_t *m)
{
    pthread_mutex_unlock(m);
}
static inline void stereo_mutex_destroy(stereo_mutex_t *m)
{
    pthread_mutex_destroy(m);
}

static inline const char *stereo_getenv(const char *name)
{
    return getenv(name);
}
static inline void stereo_setenv(const char *name, const char *value)
{
    setenv(name, value, 1);
}

#  ifdef __x86_64__
#    define STEREO_ARCH_STR  "x64"
#    define STEREO_ARCH_BITS 64
#  else
#    define STEREO_ARCH_STR  "x86"
#    define STEREO_ARCH_BITS 32
#  endif

#  define STEREO_LOG(fmt, ...) \
    fprintf(stderr, "[VKS3D " STEREO_ARCH_STR "] " fmt "\n", ##__VA_ARGS__)
#  define STEREO_ERR(fmt, ...) \
    fprintf(stderr, "[VKS3D " STEREO_ARCH_STR " ERROR] " fmt "\n", ##__VA_ARGS__)

#endif /* _WIN32 */

/* ── VKAPI shim (when not using real Vulkan headers) ─────────────────────── */
#ifndef VKAPI_ATTR
#  ifdef _WIN32
#    define VKAPI_ATTR
#    define VKAPI_CALL  __stdcall
#  else
#    define VKAPI_ATTR  __attribute__((visibility("default")))
#    define VKAPI_CALL
#  endif
#endif

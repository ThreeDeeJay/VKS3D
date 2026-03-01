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

/* ── Console colour for log output ─────────────────────────────────────────── */
#  define STEREO_LOG(fmt, ...) \
    fprintf(stderr, "[VKS3D " STEREO_ARCH_STR "] " fmt "\n", ##__VA_ARGS__)
#  define STEREO_ERR(fmt, ...) \
    fprintf(stderr, "[VKS3D " STEREO_ARCH_STR " ERROR] " fmt "\n", ##__VA_ARGS__)

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

#pragma once
#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
# endif
# ifndef NOMINMAX
# define NOMINMAX
# endif
# include <windows.h>
# include <winreg.h>
# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <stdint.h>
# include <stdbool.h>
# define STEREO_EXPORT __declspec(dllexport)
typedef HMODULE stereo_dl_t;
# define STEREO_DL_NULL NULL
static inline stereo_dl_t stereo_dl_open(const char *path)
{
DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
HMODULE h = LoadLibraryExA(path, NULL, flags);
if (!h && GetLastError() == ERROR_INVALID_PARAMETER)
h = LoadLibraryA(path); 
return h;
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
static inline const char *stereo_getenv(const char *name)
{
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
# ifdef _WIN64
# define STEREO_ARCH_STR "x64"
# define STEREO_ARCH_BITS 64
# else
# define STEREO_ARCH_STR "x86"
# define STEREO_ARCH_BITS 32
# endif
static inline char **stereo_registry_enum_icd_jsons(void)
{
static const char *key_paths[] = {
# ifdef _WIN64
"SOFTWARE\\Khronos\\Vulkan\\Drivers",
# else
"SOFTWARE\\WOW6432Node\\Khronos\\Vulkan\\Drivers",
"SOFTWARE\\Khronos\\Vulkan\\Drivers", 
# endif
NULL
};
char **list = NULL;
size_t list_cap = 0;
size_t list_len = 0;
for (int ki = 0; key_paths[ki]; ki++) {
HKEY hkey = NULL;
if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key_paths[ki],
0, KEY_READ, &hkey) != ERROR_SUCCESS)
continue;
DWORD idx = 0;
char val_name[2048];
DWORD val_name_len;
DWORD val_type;
DWORD val_data;
DWORD val_data_size;
while (1) {
val_name_len = (DWORD)sizeof(val_name);
val_data_size = (DWORD)sizeof(val_data);
LSTATUS st = RegEnumValueA(hkey, idx++, val_name, &val_name_len,
NULL, &val_type,
(LPBYTE)&val_data, &val_data_size);
if (st == ERROR_NO_MORE_ITEMS) break;
if (st != ERROR_SUCCESS) continue;
if (val_type != REG_DWORD || val_data != 0) continue;
if (list_len + 1 >= list_cap) {
list_cap = list_cap ? list_cap * 2 : 16;
list = (char **)realloc(list,
(list_cap + 1) * sizeof(char *));
if (!list) break;
}
list[list_len++] = _strdup(val_name);
list[list_len] = NULL;
}
RegCloseKey(hkey);
}
return list; 
}
static inline char *stereo_json_read_library_path(const char *json_path)
{
FILE *f = fopen(json_path, "r");
if (!f) return NULL;
char line[2048];
char *result = NULL;
while (fgets(line, sizeof(line), f)) {
char *key = strstr(line, "library_path");
if (!key) continue;
char *colon = strchr(key + 12, ':');
if (!colon) continue;
char *q1 = strchr(colon + 1, '"');
if (!q1) continue;
q1++; 
char buf[2048];
char *dst = buf;
char *src = q1;
while (*src && *src != '"' && (size_t)(dst - buf) < sizeof(buf) - 1) {
if (*src == '\\' && *(src + 1)) {
src++;
switch (*src) {
case '\\': *dst++ = '\\'; break;
case '"': *dst++ = '"'; break;
case '/': *dst++ = '/'; break;
case 'n': *dst++ = '\n'; break;
case 'r': *dst++ = '\r'; break;
case 't': *dst++ = '\t'; break;
default: *dst++ = '\\'; *dst++ = *src; break;
}
src++;
} else {
*dst++ = *src++;
}
}
*dst = '\0';
result = _strdup(buf);
break;
}
fclose(f);
return result;
}
static inline char *stereo_find_opengl_driver_icd(void)
{
# ifdef _WIN64
# define OGLI_VALNAME "OpenGLDriverName"
# define OGLI_SOFTKEY "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers\\MSOGL"
# else
# define OGLI_VALNAME "OpenGLDriverNameWoW"
# define OGLI_SOFTKEY "SOFTWARE\\WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers\\MSOGL"
# endif
static char ogli_buf[MAX_PATH];
#define OGLI_TRY(dll) do { \
if (!(dll) || !(dll)[0]) break; \
const char *_d = (dll); \
if (strchr(_d, '\\') || strchr(_d, '/')) { \
if (GetFileAttributesA(_d) != INVALID_FILE_ATTRIBUTES) \
return _strdup(_d); \
} else { \
\
snprintf(ogli_buf, sizeof(ogli_buf), \
"C:\\Windows\\System32\\%s", _d); \
if (GetFileAttributesA(ogli_buf) != INVALID_FILE_ATTRIBUTES) \
return _strdup(ogli_buf); \
} \
} while (0)
#define OGLI_READ_SZ(hk, name) do { \
char _vbuf[MAX_PATH]; DWORD _vlen = sizeof(_vbuf), _vtype; \
if (RegQueryValueExA((hk), (name), NULL, &_vtype, \
(LPBYTE)_vbuf, &_vlen) == ERROR_SUCCESS \
&& _vtype == REG_SZ && _vlen > 1) { \
_vbuf[_vlen - 1] = '\0'; \
OGLI_TRY(_vbuf); \
} \
} while (0)
{
HKEY hk = NULL;
if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, OGLI_SOFTKEY,
0, KEY_READ, &hk) == ERROR_SUCCESS) {
OGLI_READ_SZ(hk, "DLL");
RegCloseKey(hk);
}
}
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
DWORD si2 = 0;
char sk1[256];
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
OGLI_READ_SZ(h1, OGLI_VALNAME);
DWORD si3 = 0;
char sk2[64];
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
# undef OGLI_TRY
# undef OGLI_READ_SZ
# undef OGLI_VALNAME
# undef OGLI_SOFTKEY
return NULL; 
}
#ifdef __cplusplus
extern "C" {
#endif
extern HANDLE g_vks3d_log_handle;
extern int g_vks3d_log_enabled;
static int g_vks3d_log_initialized = 0;
#ifdef __cplusplus
}
#endif
static inline void vks3d_log_open(void)
{
if (g_vks3d_log_initialized) return;
g_vks3d_log_initialized = 1;
char path[MAX_PATH];
DWORD n = GetEnvironmentVariableA("STEREO_LOGFILE_PATH", path, MAX_PATH);
if (n == 0 || n >= MAX_PATH) {
g_vks3d_log_enabled = 0;
return;
}
g_vks3d_log_enabled = 1;
if (lstrcmpiA(path, "debugview") == 0) {
g_vks3d_log_handle = INVALID_HANDLE_VALUE;
return;
}
g_vks3d_log_handle = CreateFileA(
path,
FILE_APPEND_DATA, 
FILE_SHARE_READ | FILE_SHARE_WRITE,
NULL,
OPEN_ALWAYS, 
FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
NULL);
}
static inline void vks3d_log_write(const char *msg)
{
OutputDebugStringA(msg);
if (g_vks3d_log_handle != INVALID_HANDLE_VALUE) {
DWORD written;
WriteFile(g_vks3d_log_handle, msg, (DWORD)lstrlenA(msg), &written, NULL);
}
}
# include <stdarg.h>
static inline void vks3d_logf(const char *prefix, const char *fmt, ...)
{
char msg[1024];
int pos = 0;
int rem = (int)sizeof(msg) - 1;
if (prefix && prefix[0]) {
int pn = _snprintf(msg, (size_t)rem, "%s", prefix);
if (pn > 0) { pos += pn; rem -= pn; }
}
if (rem > 1) {
va_list ap;
va_start(ap, fmt);
int n = vsnprintf(msg + pos, (size_t)rem, fmt, ap);
va_end(ap);
if (n > 0) pos += (n < rem ? n : rem - 1);
}
if (pos < (int)sizeof(msg) - 1) msg[pos++] = '\n';
msg[pos] = '\0';
vks3d_log_write(msg);
}
# ifndef STEREO_LOG
# define STEREO_LOG(fmt, ...) \
do { if (g_vks3d_log_enabled) \
vks3d_logf("", fmt, ##__VA_ARGS__); \
} while (0)
# define STEREO_ERR(fmt, ...) \
do { if (g_vks3d_log_enabled) \
vks3d_logf("[ERROR] ", fmt, ##__VA_ARGS__); \
} while (0)
# endif
#else
# include <dlfcn.h>
# include <pthread.h>
# include <stdlib.h>
# include <string.h>
# include <stdio.h>
# include <stdint.h>
# include <stdbool.h>
# define STEREO_EXPORT __attribute__((visibility("default")))
typedef void *stereo_dl_t;
# define STEREO_DL_NULL NULL
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
# ifdef __x86_64__
# define STEREO_ARCH_STR "x64"
# define STEREO_ARCH_BITS 64
# else
# define STEREO_ARCH_STR "x86"
# define STEREO_ARCH_BITS 32
# endif
# define STEREO_LOG(fmt, ...) \
fprintf(stderr, fmt "\n", ##__VA_ARGS__)
# define STEREO_ERR(fmt, ...) \
fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#endif 
#ifndef VKAPI_ATTR
# ifdef _WIN32
# define VKAPI_ATTR
# define VKAPI_CALL __stdcall
# else
# define VKAPI_ATTR __attribute__((visibility("default")))
# define VKAPI_CALL
# endif
#endif

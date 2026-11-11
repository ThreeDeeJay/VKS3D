#include <regex>
#include <string>
#include <mutex>
#include <cstdlib>
extern "C" {
const char *stereo_getenv(const char *name);
}
static std::once_flag g_log_regex_once;
static std::regex g_log_regex;
static bool g_log_regex_valid = false;
extern "C" int regex_match(const char *msg)
{
    std::call_once(g_log_regex_once, []() {
        const char *env = stereo_getenv("VKS3D_LOG_REGEX_MATCH");
        if (env && env[0]) {
            try {
                g_log_regex = std::regex(env, std::regex_constants::ECMAScript);
                g_log_regex_valid = true;
            } catch (const std::regex_error &) {
                g_log_regex_valid = false;
            }
        }
    });
    if (!g_log_regex_valid)
        return 1;
    return std::regex_search(msg, g_log_regex) ? 1 : 0;
}
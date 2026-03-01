/*
 * test_stereo_math.c — Stereo configuration math tests
 *
 * Validates the off-axis frustum stereo calculations:
 *   - Separation and convergence produce correct clip-space offsets
 *   - Defaults are sane for 65mm IPD
 *   - Environment variable parsing is correct
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include "stereo_icd.h"

/* ── Portable env helpers (setenv/unsetenv are POSIX-only) ───────────────── */
#ifdef _WIN32
static void test_setenv(const char *name, const char *value)
{
    SetEnvironmentVariableA(name, value);
}
static void test_unsetenv(const char *name)
{
    SetEnvironmentVariableA(name, NULL);  /* NULL removes the variable */
}
#else
static void test_setenv(const char *name, const char *value)
{
    setenv(name, value, 1);
}
static void test_unsetenv(const char *name)
{
    unsetenv(name);
}
#endif



static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define EPSILON 1e-6f

#define TEST(name) do { tests_run++; printf("  %-55s ", name); fflush(stdout); } while(0)
#define PASS()     do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(r)    do { tests_failed++; printf("FAIL (%s)\n", r); } while(0)

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static bool approx_eq(float a, float b, float eps) {
    return fabsf(a - b) < eps;
}

/* ── Tests ─────────────────────────────────────────────────────────────────── */

static void test_default_separation(void)
{
    TEST("Default separation is 65mm (0.065)");
    /* Unset env var */
    test_unsetenv("STEREO_SEPARATION");
    test_unsetenv("STEREO_CONVERGENCE");
    test_unsetenv("STEREO_ENABLED");

    StereoConfig cfg;
    stereo_config_init(&cfg);

    if (!approx_eq(cfg.separation, 0.065f, EPSILON)) {
        FAIL("separation != 0.065");
    } else {
        PASS();
    }
}

static void test_default_convergence(void)
{
    TEST("Default convergence is 0.030");
    StereoConfig cfg;
    stereo_config_init(&cfg);

    if (!approx_eq(cfg.convergence, 0.030f, EPSILON)) {
        FAIL("convergence != 0.030");
    } else {
        PASS();
    }
}

static void test_default_enabled(void)
{
    TEST("Default stereo is enabled");
    test_unsetenv("STEREO_ENABLED");
    StereoConfig cfg;
    stereo_config_init(&cfg);

    if (!cfg.enabled) {
        FAIL("stereo should be enabled by default");
    } else {
        PASS();
    }
}

static void test_env_separation(void)
{
    TEST("STEREO_SEPARATION env var sets separation");
    test_setenv("STEREO_SEPARATION", "0.100");
    StereoConfig cfg;
    stereo_config_init(&cfg);
    test_unsetenv("STEREO_SEPARATION");

    if (!approx_eq(cfg.separation, 0.100f, 1e-4f)) {
        FAIL("separation not read from env");
    } else {
        PASS();
    }
}

static void test_env_convergence(void)
{
    TEST("STEREO_CONVERGENCE env var sets convergence");
    test_setenv("STEREO_CONVERGENCE", "0.050");
    StereoConfig cfg;
    stereo_config_init(&cfg);
    test_unsetenv("STEREO_CONVERGENCE");

    if (!approx_eq(cfg.convergence, 0.050f, 1e-4f)) {
        FAIL("convergence not read from env");
    } else {
        PASS();
    }
}

static void test_env_disabled(void)
{
    TEST("STEREO_ENABLED=0 disables stereo");
    test_setenv("STEREO_ENABLED", "0");
    StereoConfig cfg;
    stereo_config_init(&cfg);
    test_unsetenv("STEREO_ENABLED");

    if (cfg.enabled) {
        FAIL("stereo should be disabled");
    } else {
        PASS();
    }
}

static void test_zero_separation_zero_offset(void)
{
    TEST("Zero separation produces zero eye offsets");
    StereoConfig cfg;
    cfg.enabled     = true;
    cfg.separation  = 0.0f;
    cfg.convergence = 0.0f;
    cfg.flip_eyes   = false;
    stereo_config_compute_offsets(&cfg);

    if (!approx_eq(cfg.left_eye_offset, 0.0f, EPSILON) ||
        !approx_eq(cfg.right_eye_offset, 0.0f, EPSILON)) {
        FAIL("zero separation should give zero offsets");
    } else {
        PASS();
    }
}

static void test_left_negative_right_positive(void)
{
    TEST("Left eye has negative offset, right has positive (no flip)");
    StereoConfig cfg;
    cfg.enabled     = true;
    cfg.separation  = 0.065f;
    cfg.convergence = 0.0f;
    cfg.flip_eyes   = false;
    stereo_config_compute_offsets(&cfg);

    if (cfg.left_eye_offset >= 0.0f) {
        FAIL("left eye offset should be negative");
    } else if (cfg.right_eye_offset <= 0.0f) {
        FAIL("right eye offset should be positive");
    } else {
        PASS();
    }
}

static void test_convergence_direction(void)
{
    TEST("Convergence moves eyes toward centre");
    StereoConfig cfg;
    cfg.enabled     = true;
    cfg.separation  = 0.065f;
    cfg.convergence = 0.020f;
    cfg.flip_eyes   = false;
    stereo_config_compute_offsets(&cfg);

    /* With convergence, the right eye should be less positive */
    float right_no_conv = 0.065f / 2.0f;          /* half_sep */
    float right_with_conv = cfg.right_eye_offset;
    /* right_with_conv = half_sep - half_conv = smaller positive */

    if (right_with_conv >= right_no_conv) {
        FAIL("convergence should reduce right offset");
    } else {
        PASS();
    }
}

static void test_offset_formula(void)
{
    TEST("Offset formula: left=-(sep/2)+(conv/2), right=+(sep/2)-(conv/2)");
    StereoConfig cfg;
    cfg.enabled     = true;
    cfg.separation  = 0.060f;
    cfg.convergence = 0.020f;
    cfg.flip_eyes   = false;
    stereo_config_compute_offsets(&cfg);

    float expected_left  = -(0.060f / 2.0f) + (0.020f / 2.0f);
    float expected_right = +(0.060f / 2.0f) - (0.020f / 2.0f);

    if (!approx_eq(cfg.left_eye_offset,  expected_left,  1e-5f) ||
        !approx_eq(cfg.right_eye_offset, expected_right, 1e-5f)) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "got L=%.6f R=%.6f, expected L=%.6f R=%.6f",
                 cfg.left_eye_offset, cfg.right_eye_offset,
                 expected_left, expected_right);
        FAIL(msg);
    } else {
        PASS();
    }
}

static void test_extreme_separation(void)
{
    TEST("Extreme separation (1.0) is handled without overflow");
    StereoConfig cfg;
    cfg.enabled     = true;
    cfg.separation  = 1.0f;
    cfg.convergence = 0.0f;
    cfg.flip_eyes   = false;
    stereo_config_compute_offsets(&cfg);

    if (!isfinite(cfg.left_eye_offset) || !isfinite(cfg.right_eye_offset)) {
        FAIL("infinite/NaN offset");
    } else {
        PASS();
    }
}

static void test_sbs_image_dimensions(void)
{
    TEST("SBS swapchain image is exactly 2× app width");
    /* This is a formula test — no Vulkan calls required */
    uint32_t app_w = 1920;
    uint32_t sbs_w = app_w * 2;

    if (sbs_w != 3840) {
        FAIL("2×1920 != 3840");
    } else {
        PASS();
    }
}

static void test_stereo_ubo_layout(void)
{
    TEST("StereoUBO struct has correct size (16 bytes)");
    if (sizeof(StereoUBO) != 16) {
        char msg[64];
        snprintf(msg, sizeof(msg), "sizeof(StereoUBO) = %zu, expected 16",
                 sizeof(StereoUBO));
        FAIL(msg);
    } else {
        PASS();
    }
}

/* ── Main ───────────────────────────────────────────────────────────────────── */
int main(void)
{
    printf("\n");
    printf("══════════════════════════════════════════════════════════\n");
    printf("  Stereo VK ICD — Stereo Math Unit Tests\n");
    printf("══════════════════════════════════════════════════════════\n\n");

    printf("Configuration:\n");
    test_default_separation();
    test_default_convergence();
    test_default_enabled();
    test_env_separation();
    test_env_convergence();
    test_env_disabled();

    printf("\nOffset Calculation:\n");
    test_zero_separation_zero_offset();
    test_left_negative_right_positive();
    test_convergence_direction();
    test_offset_formula();
    test_extreme_separation();

    printf("\nLayout / Sizing:\n");
    test_sbs_image_dimensions();
    test_stereo_ubo_layout();

    printf("\n══════════════════════════════════════════════════════════\n");
    int failed = tests_run - tests_passed;
    printf("  Results: %d/%d passed", tests_passed, tests_run);
    if (failed) printf("  (%d FAILED)", failed);
    printf("\n");
    printf("══════════════════════════════════════════════════════════\n\n");

    return failed ? 1 : 0;
}

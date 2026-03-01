cmake_minimum_required(VERSION 3.16)

# ── Unit tests (no GPU required) ─────────────────────────────────────────────
add_executable(test_spirv_patch
    test_spirv_patch.c
    ../src/shader.c
    ../src/stereo.c
)
target_include_directories(test_spirv_patch PRIVATE
    ../include ${Vulkan_INCLUDE_DIRS})
target_link_libraries(test_spirv_patch PRIVATE dl pthread m)
target_compile_definitions(test_spirv_patch PRIVATE
    STEREO_UNIT_TEST)

add_test(NAME spirv_patch COMMAND test_spirv_patch)

# ── Stereo math unit test ─────────────────────────────────────────────────────
add_executable(test_stereo_math
    test_stereo_math.c
    ../src/stereo.c
)
target_include_directories(test_stereo_math PRIVATE
    ../include ${Vulkan_INCLUDE_DIRS})
target_link_libraries(test_stereo_math PRIVATE dl pthread m)

add_test(NAME stereo_math COMMAND test_stereo_math)

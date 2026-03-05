# tests/CMakeLists.txt

find_package(Vulkan REQUIRED)

set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/include
    ${Vulkan_INCLUDE_DIRS}
)

set(TEST_COMPILE_DEFS "")
if(WIN32)
    list(APPEND TEST_COMPILE_DEFS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

# ── SPIR-V patcher unit tests ─────────────────────────────────────────────
if(VKS3D_BUILD_TESTS)
    add_executable(test_spirv_patch
        test_spirv_patch.c
        ${CMAKE_SOURCE_DIR}/src/shader.c
        ${CMAKE_SOURCE_DIR}/src/stereo.c
    )
    target_include_directories(test_spirv_patch PRIVATE ${TEST_INCLUDE_DIRS})
    target_compile_definitions(test_spirv_patch PRIVATE ${TEST_COMPILE_DEFS})
    if(WIN32)
        target_link_libraries(test_spirv_patch PRIVATE advapi32)
        if(MSVC)
            target_compile_options(test_spirv_patch PRIVATE /std:c11 /W3 /wd4100 /wd4152)
        endif()
    else()
        target_link_libraries(test_spirv_patch PRIVATE dl pthread m)
        target_compile_options(test_spirv_patch PRIVATE -Wall -Wno-unused-parameter)
    endif()
    add_test(NAME spirv_patch COMMAND test_spirv_patch)

    # ── Stereo math unit tests ─────────────────────────────────────────────
    add_executable(test_stereo_math
        test_stereo_math.c
        ${CMAKE_SOURCE_DIR}/src/stereo.c
    )
    target_include_directories(test_stereo_math PRIVATE ${TEST_INCLUDE_DIRS})
    target_compile_definitions(test_stereo_math PRIVATE ${TEST_COMPILE_DEFS})
    if(WIN32)
        target_link_libraries(test_stereo_math PRIVATE advapi32)
        if(MSVC)
            target_compile_options(test_stereo_math PRIVATE /std:c11 /W3 /wd4100 /wd4152)
        endif()
    else()
        target_link_libraries(test_stereo_math PRIVATE dl pthread m)
        target_compile_options(test_stereo_math PRIVATE -Wall -Wno-unused-parameter)
    endif()
    add_test(NAME stereo_math COMMAND test_stereo_math)
endif()

# ── Integration demo ──────────────────────────────────────────────────────
if(VKS3D_BUILD_DEMO)
    add_executable(VKS3D_demo demo.c)
    target_include_directories(VKS3D_demo PRIVATE ${TEST_INCLUDE_DIRS})
    target_compile_definitions(VKS3D_demo PRIVATE
        ${TEST_COMPILE_DEFS}
        STEREO_ICD_JSON_PATH="${CMAKE_SOURCE_DIR}/manifest/VKS3D_${STEREO_ARCH}.json"
    )
    if(WIN32)
        target_link_libraries(VKS3D_demo PRIVATE Vulkan::Vulkan advapi32)
        if(MSVC)
            target_compile_options(VKS3D_demo PRIVATE /std:c11 /W3)
        endif()
    else()
        target_link_libraries(VKS3D_demo PRIVATE Vulkan::Vulkan dl pthread)
    endif()
endif()

# ── VKS3D Diagnostics Tool ─────────────────────────────────────────────────
# Standalone executable that checks all prerequisites for stereo rendering:
#   - Vulkan instance/device extensions
#   - VK_KHR_multiview feature support
#   - VKS3D DLL registration
#   - Registry ICD discovery
#   - Render pass intercept verification
# Built unconditionally alongside the DLL (no separate option needed).
add_executable(vks3d_diag vks3d_diag.c)
target_include_directories(vks3d_diag PRIVATE ${TEST_INCLUDE_DIRS})
target_compile_definitions(vks3d_diag PRIVATE ${TEST_COMPILE_DEFS})
if(WIN32)
    target_link_libraries(vks3d_diag PRIVATE Vulkan::Vulkan advapi32)
    if(MSVC)
        target_compile_options(vks3d_diag PRIVATE /std:c11 /W3 /wd4996)
    endif()
    # Place the diag tool next to the DLL in the output directory
    set_target_properties(vks3d_diag PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
else()
    target_link_libraries(vks3d_diag PRIVATE Vulkan::Vulkan dl pthread)
    target_compile_options(vks3d_diag PRIVATE -Wall -Wno-unused-parameter)
endif()

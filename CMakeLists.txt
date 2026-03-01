cmake_minimum_required(VERSION 3.20)

project(VKS3D
    VERSION 1.0.0
    LANGUAGES C
    DESCRIPTION "Vulkan Stereoscopic 3D ICD"
)

# ── Policy: MSVC requires C11 via /std:c11 ─────────────────────────────────
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Determine bitness from generator/toolchain ──────────────────────────────
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(STEREO_BITS 64)
    set(STEREO_ARCH "x64")
else()
    set(STEREO_BITS 32)
    set(STEREO_ARCH "x86")
endif()
message(STATUS "VKS3D: building ${STEREO_ARCH} (${STEREO_BITS}-bit)")

# ── Output names ────────────────────────────────────────────────────────────
#   VKS3D_x64.dll  /  VKS3D_x86.dll   (Windows)
#   VKS3D_x64.so   /  VKS3D_x86.so    (Linux)
set(TARGET_NAME VKS3D_${STEREO_ARCH})

# ── Vulkan SDK ───────────────────────────────────────────────────────────────
find_package(Vulkan REQUIRED)

# Also try the VulkanSDK env var explicitly (MSVC installs don't always set
# the cmake config path, but the env var is always present after SDK install)
if(NOT Vulkan_FOUND AND DEFINED ENV{VULKAN_SDK})
    set(Vulkan_INCLUDE_DIR "$ENV{VULKAN_SDK}/Include")
    message(STATUS "VKS3D: Using Vulkan headers from VULKAN_SDK env: ${Vulkan_INCLUDE_DIR}")
endif()

# ── Source files ─────────────────────────────────────────────────────────────
set(STEREO_SOURCES
    src/icd_main.c
    src/stereo.c
    src/instance.c
    src/device.c
    src/render_pass.c
    src/shader.c
    src/swapchain.c
    src/present.c
)

# ── Shader compilation ───────────────────────────────────────────────────────
find_program(GLSLC     glslc
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
find_program(GLSLANG   glslangValidator
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")

if(GLSLC OR GLSLANG)
    set(SHADER_DIR   "${CMAKE_SOURCE_DIR}/shaders")
    set(SPIRV_OUT    "${CMAKE_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY "${SPIRV_OUT}")

    set(SHADER_TARGETS "")

    # Each entry is "stage:basename" — glslc needs explicit -fshader-stage=
    # because it does not infer the stage from a .glsl extension.
    set(SHADERS "vert:composite.vert" "frag:composite.frag")

    foreach(SHADER_ENTRY ${SHADERS})
        string(REPLACE ":" ";" SHADER_PARTS "${SHADER_ENTRY}")
        list(GET SHADER_PARTS 0 STAGE)
        list(GET SHADER_PARTS 1 SHADER)

        set(GLSL_FILE  "${SHADER_DIR}/${SHADER}.glsl")
        set(SPIRV_FILE "${SPIRV_OUT}/${SHADER}.spv")

        if(GLSLC)
            add_custom_command(OUTPUT "${SPIRV_FILE}"
                COMMAND "${GLSLC}" -fshader-stage=${STAGE} "${GLSL_FILE}" -o "${SPIRV_FILE}"
                DEPENDS "${GLSL_FILE}"
                COMMENT "Compiling ${SHADER}.glsl -> ${SHADER}.spv")
        else()
            add_custom_command(OUTPUT "${SPIRV_FILE}"
                COMMAND "${GLSLANG}" -V -S "${STAGE}" "${GLSL_FILE}" -o "${SPIRV_FILE}"
                DEPENDS "${GLSL_FILE}"
                COMMENT "Compiling ${SHADER}.glsl -> ${SHADER}.spv (glslangValidator)")
        endif()

        list(APPEND SHADER_TARGETS "${SPIRV_FILE}")
    endforeach()

    add_custom_target(shaders ALL DEPENDS ${SHADER_TARGETS})
else()
    message(WARNING "Neither glslc nor glslangValidator found — shaders won't be compiled.\n"
                    "Install the Vulkan SDK or set VULKAN_SDK.")
endif()

# ── DLL / shared library ─────────────────────────────────────────────────────
add_library(${TARGET_NAME} SHARED ${STEREO_SOURCES})

set_target_properties(${TARGET_NAME} PROPERTIES
    OUTPUT_NAME  ${TARGET_NAME}
    PREFIX       ""           # no "lib" prefix on Linux either
    C_VISIBILITY_PRESET hidden
)

target_include_directories(${TARGET_NAME} PRIVATE
    include
    ${Vulkan_INCLUDE_DIRS}
)

# ── Windows-specific settings ────────────────────────────────────────────────
if(WIN32)
    # .def file controls exports (correct on both x64 and x86 __stdcall)
    target_sources(${TARGET_NAME} PRIVATE src/VKS3D.def src/VKS3D.rc)
    set_target_properties(${TARGET_NAME} PROPERTIES
        LINK_FLAGS "/DEF:${CMAKE_SOURCE_DIR}/src/VKS3D.def"
    )
    target_link_libraries(${TARGET_NAME} PRIVATE
        advapi32    # RegOpenKeyEx, RegEnumValue
    )
    target_compile_definitions(${TARGET_NAME} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        VKS3D_ARCH="${STEREO_ARCH}"
    )
    # MSVC: enable C11, treat warnings sensibly
    if(MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE
            /std:c11
            /W3
            /wd4100  # unreferenced formal parameter
            /wd4152  # nonstandard extension: function/data pointer conversion
        )
    else()
        # MinGW / clang-cl
        target_compile_options(${TARGET_NAME} PRIVATE
            -Wall -Wno-unused-parameter
        )
    endif()

    # Install DLL + JSON manifest together
    install(TARGETS ${TARGET_NAME}
        RUNTIME DESTINATION "."
    )
    install(FILES "manifest/VKS3D_${STEREO_ARCH}.json"
        DESTINATION "."
    )
    install(FILES "scripts/install.bat" "scripts/uninstall.bat" "scripts/Install-VKS3D.ps1"
        DESTINATION "."
    )

else()
    # ── Linux ────────────────────────────────────────────────────────────────
    target_link_libraries(${TARGET_NAME} PRIVATE dl pthread m)

    # Linker version script: only expose the three ICD symbols
    if(EXISTS "${CMAKE_SOURCE_DIR}/stereo_icd.map.in")
        configure_file(stereo_icd.map.in stereo_icd.map @ONLY)
        set_target_properties(${TARGET_NAME} PROPERTIES
            LINK_FLAGS "-Wl,--version-script=${CMAKE_BINARY_DIR}/stereo_icd.map"
        )
    endif()

    install(TARGETS ${TARGET_NAME} LIBRARY DESTINATION lib)
    install(FILES "manifest/VKS3D_${STEREO_ARCH}.json" DESTINATION share/vulkan/icd.d)
endif()

if(TARGET shaders)
    add_dependencies(${TARGET_NAME} shaders)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
option(VKS3D_BUILD_TESTS "Build unit tests" ON)
option(VKS3D_BUILD_DEMO  "Build integration demo" ON)

if(VKS3D_BUILD_TESTS OR VKS3D_BUILD_DEMO)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Summary ──────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "  VKS3D build summary")
message(STATUS "  -------------------")
message(STATUS "  Target     : ${TARGET_NAME}")
message(STATUS "  Arch       : ${STEREO_ARCH} (${STEREO_BITS}-bit)")
message(STATUS "  Vulkan SDK : ${Vulkan_INCLUDE_DIRS}")
message(STATUS "  Tests      : ${VKS3D_BUILD_TESTS}")
message(STATUS "")

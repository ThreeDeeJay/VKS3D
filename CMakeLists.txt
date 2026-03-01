cmake_minimum_required(VERSION 3.16)
project(stereo_vk_icd VERSION 1.0.0 LANGUAGES C)

# ── Options ──────────────────────────────────────────────────────────────────
option(STEREO_BUILD_TESTS    "Build the test applications"     ON)
option(STEREO_BUILD_DEMO     "Build the stereo demo app"       ON)
option(STEREO_SPIRV_VALIDATE "Run spirv-val on patched shaders" OFF)

# ── Standard ─────────────────────────────────────────────────────────────────
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Find dependencies ─────────────────────────────────────────────────────────
find_package(Vulkan REQUIRED)

# ── Compile GLSL shaders → SPIR-V ─────────────────────────────────────────────
find_program(GLSLANG_VALIDATOR glslangValidator
    HINTS $ENV{VULKAN_SDK}/bin /usr/bin /usr/local/bin)
find_program(GLSLC glslc
    HINTS $ENV{VULKAN_SDK}/bin /usr/bin /usr/local/bin)

if(GLSLC)
    set(SHADER_COMPILER ${GLSLC})
    set(SHADER_ARGS -c)
elseif(GLSLANG_VALIDATOR)
    set(SHADER_COMPILER ${GLSLANG_VALIDATOR})
    set(SHADER_ARGS -V)
else()
    message(WARNING "No GLSL compiler found (glslc / glslangValidator). "
                    "Composite shaders will not be compiled.")
endif()

set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})

if(SHADER_COMPILER)
    add_custom_command(
        OUTPUT  ${SHADER_OUT}/composite.vert.spv
        COMMAND ${SHADER_COMPILER} ${SHADER_ARGS}
                ${SHADER_DIR}/composite.vert.glsl
                -o ${SHADER_OUT}/composite.vert.spv
        DEPENDS ${SHADER_DIR}/composite.vert.glsl
        COMMENT "Compiling composite.vert.glsl"
    )
    add_custom_command(
        OUTPUT  ${SHADER_OUT}/composite.frag.spv
        COMMAND ${SHADER_COMPILER} ${SHADER_ARGS}
                ${SHADER_DIR}/composite.frag.glsl
                -o ${SHADER_OUT}/composite.frag.spv
        DEPENDS ${SHADER_DIR}/composite.frag.glsl
        COMMENT "Compiling composite.frag.glsl"
    )
    add_custom_target(stereo_shaders
        DEPENDS ${SHADER_OUT}/composite.vert.spv
                ${SHADER_OUT}/composite.frag.spv
    )
endif()

# ── ICD shared library ────────────────────────────────────────────────────────
add_library(stereo_vk_icd SHARED
    src/icd_main.c
    src/stereo.c
    src/instance.c
    src/device.c
    src/render_pass.c
    src/shader.c
    src/swapchain.c
    src/present.c
)

target_include_directories(stereo_vk_icd
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${Vulkan_INCLUDE_DIRS}
)

target_compile_options(stereo_vk_icd PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -fvisibility=hidden   # hide all symbols by default
)

# Only export ICD entry points
target_compile_definitions(stereo_vk_icd PRIVATE
    VK_ICD_EXPOSE_API           # required by loader
    STEREO_ICD_VERSION_MAJOR=1
    STEREO_ICD_VERSION_MINOR=0
    STEREO_ICD_VERSION_PATCH=0
    # Shader SPIR-V paths baked in at compile time
    STEREO_VERT_SPV_PATH="${SHADER_OUT}/composite.vert.spv"
    STEREO_FRAG_SPV_PATH="${SHADER_OUT}/composite.frag.spv"
)

target_link_libraries(stereo_vk_icd
    PRIVATE
        dl          # dlopen / dlsym for real ICD loading
        pthread     # per-device mutex
        m           # math (fabs etc.)
)

# The ICD must NOT link against libvulkan (that would create a circular dep)
# We load it dynamically at runtime via dlopen.
# Explicit: do NOT add Vulkan::Vulkan here.

if(SHADER_COMPILER)
    add_dependencies(stereo_vk_icd stereo_shaders)
endif()

# Linker script to expose only the three ICD entry points
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/stereo_icd.map.in
    ${CMAKE_CURRENT_BINARY_DIR}/stereo_icd.map
    @ONLY
)
set_target_properties(stereo_vk_icd PROPERTIES
    LINK_FLAGS "-Wl,--version-script,${CMAKE_CURRENT_BINARY_DIR}/stereo_icd.map"
    OUTPUT_NAME "stereo_vk_icd"
    SOVERSION   "1"
)

# ── Install ───────────────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS stereo_vk_icd
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES manifest/stereo_vk_icd.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/vulkan/icd.d
)

# ── Tests ─────────────────────────────────────────────────────────────────────
if(STEREO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Demo app ──────────────────────────────────────────────────────────────────
if(STEREO_BUILD_DEMO)
    # demo needs to link against the real Vulkan loader
    find_package(Vulkan REQUIRED)
    add_executable(stereo_demo
        tests/demo.c
    )
    target_include_directories(stereo_demo
        PRIVATE include ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(stereo_demo
        PRIVATE Vulkan::Vulkan)
    target_compile_definitions(stereo_demo PRIVATE
        STEREO_ICD_JSON_PATH="${CMAKE_CURRENT_SOURCE_DIR}/manifest/stereo_vk_icd.json"
    )
endif()

# ── Summary ───────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "═══════════════════════════════════════════════")
message(STATUS "  Stereo VK ICD — Build Configuration")
message(STATUS "═══════════════════════════════════════════════")
message(STATUS "  Vulkan include : ${Vulkan_INCLUDE_DIRS}")
message(STATUS "  Shader compiler: ${SHADER_COMPILER}")
message(STATUS "  Install prefix : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
message(STATUS "  To use:  export VK_ICD_FILENAMES=$(pwd)/manifest/stereo_vk_icd.json")
message(STATUS "           export STEREO_REAL_ICD=/path/to/libvulkan_intel.so")
message(STATUS "           export STEREO_SEPARATION=0.065")
message(STATUS "           export STEREO_CONVERGENCE=0.030")
message(STATUS "═══════════════════════════════════════════════")
message(STATUS "")

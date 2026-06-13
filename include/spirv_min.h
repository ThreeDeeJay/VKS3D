/* Minimal spirv header with just the enums we need.
 * This file is vendored to avoid depending on system spirv-headers.
 * Values taken from the official SPIR-V registry (Khronos):
 *  - SpvBuiltInViewIndex = 36
 *  - SpvCapabilityMultiView = 6
 *
 * Note: these numeric values are from the SPIR-V specification; they
 * are intentionally minimal. If you later use more SPIR-V enums,
 * replace this file with the full spirv-headers distribution.
 */
#ifndef VKS3D_SPIRV_MIN_H
#define VKS3D_SPIRV_MIN_H

/* BuiltIn enumerants */
#define SpvBuiltInViewIndex 36

/* Capability enumerants */
#define SpvCapabilityMultiView 6

#endif /* VKS3D_SPIRV_MIN_H */

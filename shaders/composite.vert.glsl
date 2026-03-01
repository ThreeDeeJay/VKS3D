/*
 * composite.vert.glsl
 *
 * Full-screen triangle vertex shader for the SBS composite pass.
 * Generates a single triangle that covers the entire viewport without
 * requiring any vertex buffer.
 *
 * Compile with:
 *   glslangValidator -V composite.vert.glsl -o composite.vert.spv
 *   glslangValidator -V composite.frag.glsl -o composite.frag.spv
 *
 * Or with shaderc:
 *   glslc -fshader-stage=vert composite.vert.glsl -o composite.vert.spv
 *   glslc -fshader-stage=frag composite.frag.glsl -o composite.frag.spv
 */

#version 450
#extension GL_KHR_vulkan_glsl : enable

layout(location = 0) out vec2 outUV;

void main()
{
    /*
     * Full-screen triangle trick:
     *   vertex 0: uv=(0,0), pos=(-1,-1)
     *   vertex 1: uv=(2,0), pos=( 3,-1)
     *   vertex 2: uv=(0,2), pos=(-1, 3)
     * The triangle covers the entire [-1,1] NDC square.
     */
    outUV = vec2((gl_VertexIndex << 1) & 2,
                  gl_VertexIndex       & 2);
    gl_Position = vec4(outUV * 2.0 - 1.0, 0.0, 1.0);
}

/*
 * composite.frag.glsl
 *
 * Side-By-Side composite fragment shader.
 *
 * The 2-layer stereo image (layer 0 = left eye, layer 1 = right eye)
 * is sampled based on the horizontal UV coordinate:
 *   - Left  half [0.0, 0.5): sample from layer 0, stretch to full width
 *   - Right half [0.5, 1.0): sample from layer 1, stretch to full width
 *
 * Compile:
 *   glslangValidator -V composite.frag.glsl -o composite.frag.spv
 */

#version 450

/*
 * The stereo render target — a 2D array image with 2 layers.
 * Bound at set=0, binding=0 in the composite pipeline.
 */
layout(set = 0, binding = 0) uniform sampler2DArray uStereoTex;

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

void main()
{
    float u;
    float layer;

    if (inUV.x < 0.5) {
        /* Left eye occupies left half: remap [0, 0.5) → [0, 1) */
        u     = inUV.x * 2.0;
        layer = 0.0;   /* layer 0 = left eye */
    } else {
        /* Right eye occupies right half: remap [0.5, 1.0) → [0, 1) */
        u     = (inUV.x - 0.5) * 2.0;
        layer = 1.0;   /* layer 1 = right eye */
    }

    outColor = texture(uStereoTex, vec3(u, inUV.y, layer));
}

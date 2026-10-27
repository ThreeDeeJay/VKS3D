
#version 450
layout(set = 0, binding = 0) uniform sampler2DArray uStereoTex;
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
void main()
{
float u;
float layer;
if (inUV.x < 0.5) {
u = inUV.x * 2.0;
layer = 0.0; 
} else {
u = (inUV.x - 0.5) * 2.0;
layer = 1.0; 
}
outColor = texture(uStereoTex, vec3(u, inUV.y, layer));
}

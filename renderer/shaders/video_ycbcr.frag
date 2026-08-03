#version 450

// The immutable YCbCr sampler performs NV12 -> RGB conversion (colorimetry is
// baked into the VkSamplerYcbcrConversion object on the CPU side).
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D videoTex;

void main() {
    color = vec4(texture(videoTex, uv).rgb, 1.0);
}

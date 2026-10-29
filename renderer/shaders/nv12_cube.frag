#version 450

// Per-plane NV12 sampling: used when the driver cannot sample the imported
// dma-buf as a multi-planar image (e.g. NVIDIA block-linear modifiers).
// Luma is bound as R8, chroma as R8G8; YUV->RGB happens here, with
// colorimetry via push constants.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D lumaTex;   // R8
layout(set = 0, binding = 1) uniform sampler2D chromaTex; // R8G8

layout(push_constant) uniform PC {
    layout(offset=64) int matrixId;  // 0 = BT.601, 1 = BT.709
    int fullRange; // 0 = limited, 1 = full
} pc;

void main() {
    float y = texture(lumaTex, uv).r;
    vec2 cbcr = texture(chromaTex, uv).rg - 0.5;

    float yn;
    vec2 c;
    if (pc.fullRange != 0) {
        yn = y;
        c = cbcr;
    } else {
        yn = (y - 16.0 / 255.0) * (255.0 / 219.0);
        c = cbcr * (255.0 / 224.0);
    }

    vec3 rgb;
    if (pc.matrixId == 1) { // BT.709
        rgb = vec3(yn + 1.5748 * c.y,
                   yn - 0.1873 * c.x - 0.4681 * c.y,
                   yn + 1.8556 * c.x);
    } else { // BT.601
        rgb = vec3(yn + 1.4020 * c.y,
                   yn - 0.3441 * c.x - 0.7141 * c.y,
                   yn + 1.7720 * c.x);
    }
    color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}

#version 450

// M30: a simulated presenter tile. No vertex buffer - the quad is generated
// from gl_VertexIndex, so the whole "stream" costs one draw call and no
// allocation. Real streams go through video_pass; this exists so the scene,
// layout reflow and (later) the entry/exit animation can be exercised without
// a decoder, a network sender or a camera in the loop.

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;   // rgb = tile tint, a = presenter index (for the label bars)
} pc;

layout(location = 0) out vec2 vUv;

vec2 kQuad[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                       vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

void main() {
    vec2 p = kQuad[gl_VertexIndex];
    vUv = p * 0.5 + 0.5;
    gl_Position = pc.mvp * vec4(p, 0.0, 1.0);
}

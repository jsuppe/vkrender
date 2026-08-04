#version 450

// Unit cube (36 vertices embedded, no vertex buffers), video UVs on every
// face. Winding is CCW-from-outside in a Y-up right-handed space; the CPU-side
// projection flips Y for Vulkan clip space, which mirrors the winding, so the
// pipeline sets frontFace=CLOCKWISE and culls back faces (a solo convex object
// needs no depth attachment).
layout(push_constant) uniform PC { mat4 mvp; } pc;

layout(location = 0) out vec2 uv;

const vec3 P[36] = vec3[](
    // +Z front
    vec3(-1, -1,  1), vec3( 1, -1,  1), vec3( 1,  1,  1),
    vec3(-1, -1,  1), vec3( 1,  1,  1), vec3(-1,  1,  1),
    // -Z back
    vec3( 1, -1, -1), vec3(-1, -1, -1), vec3(-1,  1, -1),
    vec3( 1, -1, -1), vec3(-1,  1, -1), vec3( 1,  1, -1),
    // -X left
    vec3(-1, -1, -1), vec3(-1, -1,  1), vec3(-1,  1,  1),
    vec3(-1, -1, -1), vec3(-1,  1,  1), vec3(-1,  1, -1),
    // +X right
    vec3( 1, -1,  1), vec3( 1, -1, -1), vec3( 1,  1, -1),
    vec3( 1, -1,  1), vec3( 1,  1, -1), vec3( 1,  1,  1),
    // +Y top
    vec3(-1,  1,  1), vec3( 1,  1,  1), vec3( 1,  1, -1),
    vec3(-1,  1,  1), vec3( 1,  1, -1), vec3(-1,  1, -1),
    // -Y bottom
    vec3(-1, -1, -1), vec3( 1, -1, -1), vec3( 1, -1,  1),
    vec3(-1, -1, -1), vec3( 1, -1,  1), vec3(-1, -1,  1));

// v=0 is the top of the video; each face reads upright when it faces the
// viewer with +Y up.
const vec2 T[36] = vec2[](
    vec2(0, 1), vec2(1, 1), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(0, 0),
    vec2(0, 1), vec2(1, 1), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(0, 0),
    vec2(0, 1), vec2(1, 1), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(0, 0),
    vec2(0, 1), vec2(1, 1), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(0, 0),
    vec2(0, 1), vec2(1, 1), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(0, 0),
    vec2(0, 1), vec2(1, 1), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(0, 0));

void main() {
    uv = T[gl_VertexIndex];
    gl_Position = pc.mvp * vec4(P[gl_VertexIndex], 1.0);
}

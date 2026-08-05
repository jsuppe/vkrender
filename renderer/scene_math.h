#pragma once

#include <cmath>

#include <volk.h>

namespace vkr {

// Minimal column-major mat4 helpers for the cube scene (shared between the
// Android renderer and the desktop Wayland compositor mode).
struct Mat4 {
    float m[16]; // column-major, m[col*4+row]
};

inline Mat4 matMul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k) r.m[c * 4 + row] += a.m[k * 4 + row] * b.m[c * 4 + k];
    return r;
}

inline Mat4 matRotY(float a) {
    Mat4 r{};
    float c = cosf(a), s = sinf(a);
    r.m[0] = c; r.m[2] = -s; r.m[5] = 1.0f; r.m[8] = s; r.m[10] = c; r.m[15] = 1.0f;
    return r;
}

inline Mat4 matRotX(float a) {
    Mat4 r{};
    float c = cosf(a), s = sinf(a);
    r.m[0] = 1.0f; r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c; r.m[15] = 1.0f;
    return r;
}

// Vulkan conventions: depth 0..1, clip Y down (the Y flip lives in m[5]).
inline Mat4 matPerspective(float fovyRad, float aspect, float zNear, float zFar) {
    Mat4 r{};
    float f = 1.0f / tanf(fovyRad * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = -f;
    r.m[10] = zFar / (zNear - zFar);
    r.m[11] = -1.0f;
    r.m[14] = (zNear * zFar) / (zNear - zFar);
    return r;
}

// A cube instance's model matrix: translate(x, y, -7.5) * rotY * rotX *
// scale(sx, sy, sz). Rotation speeds/phase keep multiple cubes visibly
// different; the M15 wall layout uses near-zero sz for flat tiles.
inline Mat4 cubeModel(float tSec, float xOffset, float yOffset, float spinY, float spinX,
                      float phase, float sx, float sy, float sz) {
    Mat4 model = matMul(matRotY(spinY * tSec + phase), matRotX(spinX * tSec + phase));
    for (int r = 0; r < 3; ++r) {
        model.m[0 * 4 + r] *= sx;
        model.m[1 * 4 + r] *= sy;
        model.m[2 * 4 + r] *= sz;
    }
    model.m[12] += xOffset;
    model.m[13] += yOffset;
    model.m[14] -= 7.5f;
    return model;
}

// Asymmetric sub-frustum (M15 multi-window spanning): the full 45°-vertical
// frustum covers the virtual canvas; a window showing canvas region
// [u0,u1]x[v0,v1] (normalized, v = 0 at the TOP) gets the matching off-center
// projection so the scene lines up seamlessly across windows. Vulkan depth
// 0..1, clip Y down. u0=0,v0=0,u1=1,v1=1 reproduces matPerspective.
inline Mat4 matFrustumSub(float canvasAspect, float u0, float v0, float u1, float v1,
                          float zNear = 0.1f, float zFar = 100.0f) {
    float hh = zNear * tanf(0.7854f * 0.5f); // half-height at near
    float hw = hh * canvasAspect;
    float l = -hw + u0 * 2.0f * hw;
    float r = -hw + u1 * 2.0f * hw;
    float t = hh - v0 * 2.0f * hh;
    float b = hh - v1 * 2.0f * hh;
    Mat4 m{};
    m.m[0] = 2.0f * zNear / (r - l);
    m.m[8] = (r + l) / (r - l);
    m.m[5] = -2.0f * zNear / (t - b);
    m.m[9] = -(t + b) / (t - b);
    m.m[10] = zFar / (zNear - zFar);
    m.m[14] = (zNear * zFar) / (zNear - zFar);
    m.m[11] = -1.0f;
    return m;
}

// One cube of the two-cube scene (M12 behavior; Android still uses this).
inline Mat4 cubeMvp(float tSec, VkExtent2D extent, float xOffset, float yOffset, float spin,
                    float phase) {
    Mat4 model = cubeModel(tSec, xOffset, yOffset, spin * 0.9f, 0.37f, phase, 0.85f, 0.85f,
                           0.85f);
    float aspect = extent.height ? (float)extent.width / (float)extent.height : 1.0f;
    return matMul(matPerspective(0.7854f, aspect, 0.1f, 100.0f), model);
}

} // namespace vkr

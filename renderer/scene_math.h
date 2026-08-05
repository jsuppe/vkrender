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

// One cube of the two-cube scene: spins around two axes (spin sign and phase
// keep the cubes visibly different), scaled and offset so both fit fully
// inside a 45°-FOV view at z = -7.5 (half-height there is 3.11 and
// half-width 3.11*aspect; |offset| + scaledHalfDiagonal = 1.55 + 1.47 fits
// the offset axis). Callers offset along Y in portrait, X in landscape.
inline Mat4 cubeMvp(float tSec, VkExtent2D extent, float xOffset, float yOffset, float spin,
                    float phase) {
    Mat4 model = matMul(matRotY(spin * tSec * 0.9f + phase), matRotX(tSec * 0.37f + phase));
    constexpr float kScale = 0.85f;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) model.m[c * 4 + r] *= kScale;
    model.m[12] += xOffset;
    model.m[13] += yOffset;
    model.m[14] -= 7.5f;
    float aspect = extent.height ? (float)extent.width / (float)extent.height : 1.0f;
    return matMul(matPerspective(0.7854f, aspect, 0.1f, 100.0f), model);
}

} // namespace vkr

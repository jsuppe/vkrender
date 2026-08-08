#version 450

// Deliberately busy: a plain colour would hide exactly the artefacts this
// harness exists to catch. Vertical bars make horizontal motion (and any skew
// between panels) obvious at a seam, and the bright border shows where a tile
// ends versus where a bezel begins.

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;   // rgb = tile tint, a = presenter index
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 c = pc.color.rgb;

    // Vertical bars: motion reference.
    c *= 0.72 + 0.28 * step(0.5, fract(vUv.x * 8.0));

    // Horizontal band count = presenter index, so tiles are tellable apart
    // without any text rendering.
    float idx = pc.color.a;
    if (vUv.y > 0.86) {
        float slot = floor(vUv.x * 12.0);
        c = slot < idx ? vec3(1.0) : c * 0.35;
    }

    // Border.
    float edge = (vUv.x < 0.012 || vUv.x > 0.988 || vUv.y < 0.012 || vUv.y > 0.988)
                     ? 1.0 : 0.0;
    outColor = vec4(mix(c, vec3(1.0), edge), 1.0);
}

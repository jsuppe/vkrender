#version 450

// Procedural vaporwave backdrop (M12): sunset-gradient sky, striped synthwave
// sun on the horizon, neon perspective grid scrolling toward the viewer.
// Drawn with fullscreen.vert (uv.y = 0 at the top of the screen).
// uvOff/uvScale map this window's uv into the shared virtual-canvas uv so the
// backdrop spans seamlessly across multiple windows (M15); single-window
// callers pass off=(0,0), scale=(1,1).
layout(push_constant) uniform PC {
    float time;
    float aspect; // full canvas aspect
    vec2 uvOff;
    vec2 uvScale;
} pc;

layout(location = 0) in vec2 winUv;
layout(location = 0) out vec4 color;

void main() {
    vec2 uv = pc.uvOff + winUv * pc.uvScale;
    const float horizon = 0.62;

    if (uv.y < horizon) {
        // Sky: deep violet up top through hot pink into an orange band.
        float t = uv.y / horizon;
        vec3 sky = mix(vec3(0.10, 0.02, 0.24), vec3(0.62, 0.10, 0.52),
                       smoothstep(0.15, 0.85, t));
        sky = mix(sky, vec3(0.98, 0.45, 0.25), smoothstep(0.80, 1.0, t));

        // Sun: disc sitting on the horizon, yellow -> pink gradient, with the
        // classic widening scanline gaps across its lower half.
        vec2 p = vec2((uv.x - 0.5) * pc.aspect, uv.y - (horizon - 0.05));
        float r = length(p);
        float disc = smoothstep(0.20, 0.195, r);
        vec3 sunColor = mix(vec3(1.0, 0.88, 0.35), vec3(1.0, 0.25, 0.55),
                            clamp(p.y * 3.5 + 0.55, 0.0, 1.0));
        if (p.y > 0.0) {
            float band = fract(p.y * 26.0 - pc.time * 0.6);
            disc *= step(0.35 + 0.4 * clamp(p.y * 5.0, 0.0, 1.0), band);
        }
        sky += vec3(0.55, 0.18, 0.35) * exp(-r * 5.0); // glow
        color = vec4(mix(sky, sunColor, disc), 1.0);
    } else {
        // Ground: neon grid in perspective, scrolling toward the camera.
        float gy = (uv.y - horizon) / (1.0 - horizon); // 0 horizon .. 1 bottom
        float z = 1.0 / (gy + 0.02);                   // pseudo-depth
        float gx = (uv.x - 0.5) * pc.aspect * z;
        float gv = z * 1.6 + pc.time * 1.5;
        vec2 cell = abs(fract(vec2(gx * 0.9, gv)) - 0.5);
        // Line width shrinks with distance so the grid stays crisp.
        float w = clamp(0.03 * gy + 0.006, 0.006, 0.05);
        float line = max(smoothstep(w, 0.0, 0.5 - cell.x),
                         smoothstep(w, 0.0, 0.5 - cell.y));
        vec3 ground = vec3(0.07, 0.01, 0.16);
        vec3 gridColor = mix(vec3(0.0, 0.95, 0.95), vec3(1.0, 0.20, 0.85),
                             clamp(gy * 1.6, 0.0, 1.0));
        ground = mix(ground, gridColor, line);
        // Fog the grid into the horizon glow.
        ground = mix(vec3(0.95, 0.42, 0.30), ground, smoothstep(0.0, 0.18, gy));
        color = vec4(ground, 1.0);
    }
}

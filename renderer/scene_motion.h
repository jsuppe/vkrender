#pragma once

// M31: the animation layer for presenter tiles.
//
// Layouts compute TARGETS; this owns the CURRENT transform and moves it. The
// separation matters: a layout is a pure function of the stream count, so if
// it wrote positions directly every join or leave would teleport every tile.
//
// Three behaviours fall out of one mechanism:
//   entry   a new stream starts off the right of the canvas and travels in
//   reflow  a join/leave changes every target, so everyone glides aside
//   exit    a departed stream targets off-canvas and is dropped on arrival
//
// Spanning is not a special case. Because the canvas is one continuous space
// that the panels are viewports onto (docs/MULTIDISPLAY.md), a tile animating
// from +W to its slot slides across the whole wall with no cross-display
// logic at all.
//
// Header-only and free of Vulkan/platform types on purpose: the desktop scene
// host and the Android wall demo both use it, so the motion is written once.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace vkr {

struct TileTransform {
    float cx = 0.0f, cy = 0.0f, hw = 0.0f, hh = 0.0f;
};

// Critically damped spring, semi-implicit so it stays stable at large dt (an
// alt-tab or a stalled frame must not launch tiles across the wall).
//
// Deliberately NOT a fixed-duration tween. The common case is a second
// presenter joining while the first is still flying in, which re-targets
// mid-flight: a spring absorbs that continuously, a tween has to restart and
// visibly snaps.
struct Spring {
    float x = 0.0f, v = 0.0f;

    void reset(float value) {
        x = value;
        v = 0.0f;
    }

    void step(float target, float dt, float omega) {
        const float f = 1.0f + 2.0f * dt * omega;
        const float oo = omega * omega;
        const float hoo = dt * oo;
        const float hhoo = dt * hoo;
        const float detInv = 1.0f / (f + hhoo);
        const float detX = f * x + dt * v + hhoo * target;
        const float detV = v + hoo * (target - x);
        x = detX * detInv;
        v = detV * detInv;
    }
};

class SceneMotion {
public:
    // Stiffness. ~9 rad/s settles in roughly half a second, which reads as
    // deliberate rather than either sluggish or twitchy. VKR_MOTION_OMEGA
    // overrides; VKR_MOTION_OFF=1 disables animation entirely (tiles snap),
    // which is the honest fallback if cross-display skew ever makes motion
    // across a seam unacceptable - see docs/MULTIDISPLAY.md.
    SceneMotion() {
        if (const char* o = getenv("VKR_MOTION_OMEGA")) {
            float v = (float)atof(o);
            if (v > 0.0f) omega_ = v;
        }
        enabled_ = getenv("VKR_MOTION_OFF") == nullptr;
    }

    // `ids`/`targets` are this frame's laid-out streams, in layout order.
    // `canvasHalfWidth` is where "off-canvas" starts. Call once per frame.
    void update(const uint64_t* ids, const TileTransform* targets, size_t n, float dt,
                float canvasHalfWidth) {
        if (dt <= 0.0f) dt = 1.0f / 60.0f;
        if (dt > 0.1f) dt = 0.1f; // a long stall must not teleport anything

        for (auto& t : tiles_) t.present = false;

        for (size_t i = 0; i < n; ++i) {
            Tile* tile = find(ids[i]);
            if (!tile) {
                tiles_.push_back(Tile{});
                tile = &tiles_.back();
                tile->id = ids[i];
                // Enter from beyond the right edge, already the right size, so
                // the travel reads as the tile arriving rather than growing.
                tile->cx.reset(canvasHalfWidth + targets[i].hw);
                tile->cy.reset(targets[i].cy);
                tile->hw.reset(targets[i].hw);
                tile->hh.reset(targets[i].hh);
            }
            tile->present = true;
            tile->leaving = false;
            tile->target = targets[i];
        }

        // Anything the layout dropped this frame exits the way it came in.
        for (auto& t : tiles_) {
            if (t.present) continue;
            if (!t.leaving) {
                t.leaving = true;
                t.target.cx = canvasHalfWidth + t.target.hw;
            }
        }

        for (auto& t : tiles_) {
            if (!enabled_) {
                t.cx.reset(t.target.cx);
                t.cy.reset(t.target.cy);
                t.hw.reset(t.target.hw);
                t.hh.reset(t.target.hh);
                continue;
            }
            t.cx.step(t.target.cx, dt, omega_);
            t.cy.step(t.target.cy, dt, omega_);
            t.hw.step(t.target.hw, dt, omega_);
            t.hh.step(t.target.hh, dt, omega_);
        }

        // Reap only once a leaver is fully clear of the canvas, so it is never
        // seen popping out of existence mid-travel.
        for (size_t i = tiles_.size(); i-- > 0;) {
            const Tile& t = tiles_[i];
            if (t.leaving && (t.cx.x - t.hw.x) > canvasHalfWidth)
                tiles_.erase(tiles_.begin() + (long)i);
        }
    }

    size_t count() const { return tiles_.size(); }

    uint64_t idAt(size_t i) const { return tiles_[i].id; }

    TileTransform at(size_t i) const {
        const Tile& t = tiles_[i];
        return TileTransform{t.cx.x, t.cy.x, t.hw.x, t.hh.x};
    }

    // True while a tile is on its way out - callers that own a slot (a real
    // producer connection) can keep it alive until this stops being reported.
    bool leavingAt(size_t i) const { return tiles_[i].leaving; }

private:
    struct Tile {
        uint64_t id = 0;
        Spring cx, cy, hw, hh;
        TileTransform target;
        bool present = false;
        bool leaving = false;
    };

    Tile* find(uint64_t id) {
        for (auto& t : tiles_)
            if (t.id == id) return &t;
        return nullptr;
    }

    std::vector<Tile> tiles_;
    float omega_ = 9.0f;
    bool enabled_ = true;
};

} // namespace vkr

#pragma once
// Shared scene layout and motion: desktop hosts and Android NativeActivity.
#include "scene_math.h"
#include "scene_motion.h"
#include "external_video_image.h"
#include <algorithm>
#include <vector>
#include <utility>
namespace vkr {
enum class SceneLayout { TwoCubes = 0, SingleCube, Wall, Presentation, Count };
inline void buildScene(SceneLayout layout_, float t, float canvasAspect_, double dtSec,
    const std::vector<int64_t>& enabled, const std::vector<uint64_t>& enabledIds,
    const ExternalVideoPool& pool, SceneMotion& motion_,
    std::vector<std::pair<uint64_t,int64_t>>& tileSlots_,
    std::vector<Mat4>& models, std::vector<int64_t>& faceSlots) {
    models.clear(); faceSlots.clear();
    const bool wide = canvasAspect_ >= 1.0f;
    auto cycleFaces = [&](size_t faces) {
        for (size_t fIdx = 0; fIdx < faces; ++fIdx)
            faceSlots.push_back(enabled.empty() ? -1 : enabled[fIdx % enabled.size()]);
    };
    switch (layout_) {
        case SceneLayout::TwoCubes:
            models.push_back(cubeModel(t, wide ? -1.55f : 0.0f, wide ? 0.0f : 1.55f, 0.9f,
                                       0.37f, 0.0f, 0.85f, 0.85f, 0.85f));
            models.push_back(cubeModel(t, wide ? 1.55f : 0.0f, wide ? 0.0f : -1.55f, -0.63f,
                                       0.37f, 2.1f, 0.85f, 0.85f, 0.85f));
            cycleFaces(12);
            break;
        case SceneLayout::SingleCube:
            models.push_back(cubeModel(t, 0.0f, 0.0f, 0.9f, 0.37f, 0.0f, 1.35f, 1.35f, 1.35f));
            cycleFaces(6);
            break;
        case SceneLayout::Wall: {
            size_t n = enabled.size();
            if (n == 0) break;
            const float halfH = 3.11f, halfW = halfH * canvasAspect_;
            int cols = std::max(1, (int)std::lround(std::sqrt((double)n * canvasAspect_)));
            int rows = (int)((n + cols - 1) / cols);
            float tileW = 2.0f * halfW / (float)cols, tileH = 2.0f * halfH / (float)rows;
            float sx = std::min(tileW * 0.44f, tileH * 0.44f * 16.0f / 9.0f);
            float sy = sx * 9.0f / 16.0f;
            for (size_t i = 0; i < n; ++i) {
                int cIdx = (int)(i % cols), rIdx = (int)(i / cols);
                float cx = -halfW + tileW * ((float)cIdx + 0.5f);
                float cy = halfH - tileH * ((float)rIdx + 0.5f);
                // Gentle bob for parallax; face 0 (+Z) carries the stream.
                Mat4 m = cubeModel(t, cx, cy, 0.0f, 0.0f, 0.0f, sx, sy, 0.04f);
                m.m[14] += 0.12f * sinf(t * 0.8f + (float)i * 1.3f);
                models.push_back(m);
                faceSlots.push_back(enabled[i]);
                for (int f = 1; f < 6; ++f) faceSlots.push_back(-1);
            }
            break;
        }
        case SceneLayout::Presentation: {
            // M25: up to 4 presenters in fixed regions, scaled to fit
            // (16:9 aspect-preserving letterbox inside each region):
            //   1 -> full screen, 2 -> flat top/bottom, 3 -> triangle
            //   (one up top, two below), 4 -> quad (2x2). Extra streams
            //   beyond 4 wait off-scene until a slot frees.
            size_t n = std::min(enabled.size(), (size_t)4);
            if (n == 0) break;
            struct Region {
                float cx, cy, hw, hh;
            };
            const float H = 3.11f, W = H * canvasAspect_;

            // A tile takes its STREAM's aspect, not a hardcoded 16:9. Every
            // source tested before this was landscape, so the assumption held
            // by luck; the first real AirPlay cast was an iPad mirroring
            // PORTRAIT at 752x1080 (~0.70), and forcing that into a 16:9 quad
            // stretched it horizontally by 1.78/0.70 ~= 2.5x. Phones are
            // narrower still.
            //
            // 16:9 remains the fallback for the frames between a producer
            // connecting and its first real allocation, when the pool has no
            // dimensions yet (the ring starts 64x64).
            float aspects[4];
            for (size_t i = 0; i < n; ++i) {
                aspects[i] = 16.0f / 9.0f;
                if (const ExternalVideoPool::Slot* s = pool.find((uint32_t)enabled[i]))
                    if (s->width > 0 && s->height > 0)
                        aspects[i] = (float)s->width / (float)s->height;
            }

            // M29: pick the arrangement, don't hardcode it.
            //
            // The old table assumed a 16:9 canvas: 2 streams always went
            // top/bottom. But a 16:9 tile fitted into a half-height band is
            // limited by HEIGHT, so it can never be wider than half the canvas
            // - and on a three-panel wall (aspect 3.69) that left each tile at
            // ~24% of the width with both ends empty. Nothing to do with the
            // per-window frustum; this is purely where the tiles are placed in
            // canvas space.
            //
            // So evaluate the sensible arrangements and keep whichever makes
            // the tiles biggest. On 16:9 this reproduces the original choices;
            // on a wide wall 2 streams land side by side instead of stacked.
            struct Arrangement {
                int cols, rows;
                bool triangle; // 3-up: one across the top, two beneath
            };
            const Arrangement candidates[] = {
                {1, 1, false}, {2, 1, false}, {1, 2, false}, {3, 1, false},
                {1, 3, false}, {0, 0, true},  {2, 2, false}, {4, 1, false},
                {1, 4, false},
            };

            auto buildRegions = [&](const Arrangement& a, Region* out) -> float {
                if (a.triangle) {
                    out[0] = {0, H / 2, W, H / 2};
                    out[1] = {-W / 2, -H / 2, W / 2, H / 2};
                    out[2] = {W / 2, -H / 2, W / 2, H / 2};
                } else {
                    const float hw = W / (float)a.cols, hh = H / (float)a.rows;
                    for (int r = 0; r < a.rows; ++r)
                        for (int c = 0; c < a.cols; ++c)
                            out[r * a.cols + c] = {-W + hw * (2 * c + 1),
                                                   H - hh * (2 * r + 1), hw, hh};
                }
                // Score = the SMALLEST tile the arrangement produces, so a
                // layout is only preferred if it helps the worst-off stream.
                // Scored with each stream's OWN aspect: a portrait tile is
                // height-limited where a landscape one is width-limited, so
                // scoring them all as 16:9 would pick the wrong arrangement
                // for a mixed set (e.g. an iPad beside a laptop).
                float worst = 1e9f;
                // Only ever called with count == n (checked by the caller), so
                // aspects[i] is in range.
                const size_t count = a.triangle ? 3 : (size_t)(a.cols * a.rows);
                for (size_t i = 0; i < count; ++i)
                    worst = std::min(worst, std::min(out[i].hw, out[i].hh * aspects[i]));
                return worst;
            };

            Region regs[4];
            float best = -1.0f;
            for (const Arrangement& a : candidates) {
                const size_t count = a.triangle ? 3 : (size_t)(a.cols * a.rows);
                if (count != n) continue;
                Region trial[4];
                float score = buildRegions(a, trial);
                if (score > best) {
                    best = score;
                    for (size_t i = 0; i < n; ++i) regs[i] = trial[i];
                }
            }
            if (best < 0.0f) break; // no arrangement for this count

            // M31: the layout produces TARGETS; SceneMotion owns where tiles
            // actually are, so a join or leave makes everyone glide instead of
            // teleport. Shared with the Android wall demo via scene_motion.h.
            const float margin = 0.98f;
            TileTransform targets[4];
            for (size_t i = 0; i < n; ++i) {
                const Region& r = regs[i];
                float sx = std::min(r.hw, r.hh * aspects[i]) * margin;
                targets[i] = {r.cx, r.cy, sx, sx / aspects[i]};
            }
            motion_.update(enabledIds.data(), targets, n, (float)dtSec, W);

            // Remember each live stream's pool slot. A departing stream is no
            // longer in `streams` (that is how motion learns it is leaving),
            // so without this its texture could not be found and the exit
            // would pop. The service keeps the ring alive until
            // retainsStream() goes false, so the slot stays valid throughout.
            for (size_t i = 0; i < enabledIds.size(); ++i) {
                bool found = false;
                for (auto& e : tileSlots_)
                    if (e.first == enabledIds[i]) {
                        e.second = enabled[i];
                        found = true;
                        break;
                    }
                if (!found) tileSlots_.push_back({enabledIds[i], enabled[i]});
            }

            for (size_t k = 0; k < motion_.count(); ++k) {
                int64_t slot = -1;
                for (const auto& e : tileSlots_)
                    if (e.first == motion_.idAt(k)) {
                        slot = e.second;
                        break;
                    }
                if (slot < 0) continue;
                const TileTransform tr = motion_.at(k);
                // Static, face-on: zero rotation params, wafer-thin depth.
                models.push_back(
                    cubeModel(0.0f, tr.cx, tr.cy, 0.0f, 0.0f, 0.0f, tr.hw, tr.hh, 0.01f));
                faceSlots.push_back(slot);
                for (int f = 1; f < 6; ++f) faceSlots.push_back(-1);
            }
            // Drop cache entries for tiles the motion has finished with, so a
            // long session does not accumulate one per stream ever seen.
            for (size_t i = tileSlots_.size(); i-- > 0;)
                if (!motion_.has(tileSlots_[i].first))
                    tileSlots_.erase(tileSlots_.begin() + (long)i);
            break;
        }
        default: break;
    }
}
} // namespace vkr

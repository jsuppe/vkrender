#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
namespace vkr {
template<size_t Capacity> struct ModerationPolicy {
    bool hidden[Capacity] = {};
    bool pinned[Capacity] = {};
    int spotlight = -1;                  // slot index, -1 = none
    uint32_t maxVisible = 4;
    double rotateSeconds = 8.0;
    std::vector<uint32_t> order;         // slot indices, front = first seated

    void ensureOrdered(uint32_t slot) {
        for (uint32_t o : order)
            if (o == slot) return;
        order.push_back(slot);
    }
    void toFront(uint32_t slot) {
        ensureOrdered(slot);
        for (size_t i = 0; i < order.size(); ++i)
            if (order[i] == slot) {
                order.erase(order.begin() + (ptrdiff_t)i);
                break;
            }
        order.insert(order.begin(), slot);
    }

    // The seated slots for this frame, in seating order.
    std::vector<uint32_t> select(const bool live[Capacity], int64_t nowNsV) const {
        std::vector<uint32_t> out;
        if (spotlight >= 0 && spotlight < (int)Capacity && live[spotlight] &&
            !hidden[spotlight]) {
            out.push_back((uint32_t)spotlight);
            return out;
        }
        std::vector<uint32_t> eligible; // ordered, live, not hidden
        for (uint32_t o : order)
            if (o < Capacity && live[o] && !hidden[o]) eligible.push_back(o);
        // Pinned first, in order.
        std::vector<uint32_t> rest;
        for (uint32_t e : eligible) {
            if (pinned[e] && out.size() < maxVisible) out.push_back(e);
            else if (!pinned[e]) rest.push_back(e);
        }
        const size_t seats = maxVisible > out.size() ? maxVisible - out.size() : 0;
        if (rest.size() <= seats) {
            out.insert(out.end(), rest.begin(), rest.end());
        } else if (seats > 0) {
            // The carousel: the window over `rest` advances one PAGE per
            // period, so every presenter gets equal time and the transition
            // is the presentation layout's own enter/exit animation.
            const uint64_t page =
                (uint64_t)((double)nowNsV * 1e-9 / (rotateSeconds > 0.5 ? rotateSeconds : 0.5));
            const size_t start = (size_t)((page * seats) % rest.size());
            for (size_t i = 0; i < seats; ++i)
                out.push_back(rest[(start + i) % rest.size()]);
        }
        return out;
    }
};
}

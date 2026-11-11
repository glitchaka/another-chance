/*
===========================================================================
Another Chance: idScreenRect adapted from DOOM 3 BFG Edition.
Original source:
    neo/renderer/ScreenRect.h
    neo/renderer/ScreenRect.cpp
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Licensed under GPLv3-or-later subject to DOOM 3 BFG's additional terms.
See COPYING.txt.

Adaptation: use 32-bit pixel coordinates for long scrolling web documents,
remove Doom-specific depth-buffer data and debug-view dependencies.
Inclusive pixel coordinates are retained from idTech.
===========================================================================
*/
#pragma once
#include <algorithm>
#include <cstdint>

namespace ac::tech {
class idScreenRect {
public:
    int32_t x1 = 0, y1 = 0, x2 = -1, y2 = -1;

    static idScreenRect FromHalfOpen(int left, int top, int right, int bottom) {
        return {left, top, right - 1, bottom - 1};
    }

    void Clear() {
        x1 = y1 = 0;
        x2 = y2 = -1;
    }

    [[nodiscard]] bool IsEmpty() const { return x1 > x2 || y1 > y2; }
    [[nodiscard]] int Width() const { return IsEmpty() ? 0 : x2 - x1 + 1; }
    [[nodiscard]] int Height() const { return IsEmpty() ? 0 : y2 - y1 + 1; }

    void AddPoint(int x, int y) {
        if (IsEmpty()) { x1 = x2 = x; y1 = y2 = y; return; }
        x1 = std::min(x1, x); y1 = std::min(y1, y);
        x2 = std::max(x2, x); y2 = std::max(y2, y);
    }

    void Expand() {
        --x1; --y1; ++x2; ++y2;
    }

    void Intersect(const idScreenRect& r) {
        x1 = std::max(x1, r.x1);
        y1 = std::max(y1, r.y1);
        x2 = std::min(x2, r.x2);
        y2 = std::min(y2, r.y2);
    }

    void Union(const idScreenRect& r) {
        if (r.IsEmpty()) return;
        if (IsEmpty()) { *this = r; return; }
        x1 = std::min(x1, r.x1);
        y1 = std::min(y1, r.y1);
        x2 = std::max(x2, r.x2);
        y2 = std::max(y2, r.y2);
    }

    [[nodiscard]] bool Contains(int x, int y) const {
        return !IsEmpty() && x >= x1 && x <= x2 && y >= y1 && y <= y2;
    }
};
} // namespace ac::tech

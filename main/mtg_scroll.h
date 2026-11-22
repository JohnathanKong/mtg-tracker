#pragma once
#include <algorithm>

namespace mtg_scroll {
constexpr int kRowHeight = 46;
constexpr int kViewportTop = 110;
constexpr int kViewportHeight = 184;
constexpr int kViewportBottom = kViewportTop + kViewportHeight;
constexpr int kVisibleRows = 5; // Includes one partially visible row.

inline int maxScroll(int itemCount) {
    return std::max(0, itemCount * kRowHeight - kViewportHeight);
}
inline int clampScroll(int pixels, int itemCount) {
    return std::clamp(pixels, 0, maxScroll(itemCount));
}
inline int firstRow(int pixels) {
    return pixels / kRowHeight;
}
inline int rowOffset(int pixels) {
    return pixels % kRowHeight;
}
inline int tappedIndex(int y, int scrollPixels, int itemCount) {
    if (y < kViewportTop || y >= kViewportBottom) return -1;
    const int index = (scrollPixels + y - kViewportTop) / kRowHeight;
    return index < itemCount ? index : -1;
}
}

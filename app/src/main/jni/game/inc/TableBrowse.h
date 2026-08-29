#pragma once

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <string>

namespace TableBrowse {

inline constexpr int TABLE_COUNT = 17;
inline int g_centerModeIndex = -1; // learned from the last table selected by the player
inline bool g_browseScreenActive = false;
inline bool g_touchActive = false;
inline float g_touchStartX = 0.0f;
inline float g_touchLastX = 0.0f;

inline int NormalizeIndex(int index) {
    while (index < 0) index += TABLE_COUNT;
    while (index >= TABLE_COUNT) index -= TABLE_COUNT;
    return index;
}

inline int ModeIndexFromKey(const std::string& mode) {
    if (mode.size() < 2 || mode[0] != 'M') return -1;
    char* end = nullptr;
    const long value = std::strtol(mode.c_str() + 1, &end, 10);
    if (!end || *end != '\0' || value < 1 || value > TABLE_COUNT) return -1;
    return (int)value - 1;
}

inline std::string ModeKeyForOffset(int offset) {
    if (g_centerModeIndex < 0 || g_centerModeIndex >= TABLE_COUNT) return std::string();
    const int index = NormalizeIndex(g_centerModeIndex + offset);
    return std::string("M") + std::to_string(index + 1);
}

inline void SetBrowseScreenActive(bool active) {
    g_browseScreenActive = active;
    if (!active) {
        g_touchActive = false;
    }
}

inline void SetCenterFromMode(const std::string& mode) {
    const int index = ModeIndexFromKey(mode);
    if (index >= 0) g_centerModeIndex = index;
}

inline void SetInitialFromCoins(int64_t coins) {
    if (g_centerModeIndex >= 0 || coins <= 0) return;
    static constexpr int64_t entryBets[TABLE_COUNT] = {
        50, 100, 500, 2500, 10000, 50000, 100000, 250000, 500000,
        1000000, 2500000, 4000000, 5000000, 10000000, 15000000,
        25000000, 100000000
    };
    int best = 0;
    for (int i = 0; i < TABLE_COUNT; ++i) {
        if (coins >= entryBets[i]) best = i;
    }
    g_centerModeIndex = best;
}

inline void OnTouchBegin(float x, float y) {
    (void)y;
    if (!g_browseScreenActive) return;
    g_touchActive = true;
    g_touchStartX = x;
    g_touchLastX = x;
}

inline void OnTouchMove(float x, float y) {
    (void)y;
    if (!g_browseScreenActive || !g_touchActive) return;
    g_touchLastX = x;
}

inline void OnTouchEnd(float x, float y) {
    (void)y;
    if (!g_browseScreenActive || !g_touchActive) return;
    g_touchLastX = x;

    const float dx = g_touchLastX - g_touchStartX;
    if (std::fabs(dx) >= 80.0f) {
        // Swipe left moves to the next card; swipe right moves back.
        g_centerModeIndex = NormalizeIndex(g_centerModeIndex + (dx < 0.0f ? 1 : -1));
    }
    g_touchActive = false;
}

} // namespace TableBrowse

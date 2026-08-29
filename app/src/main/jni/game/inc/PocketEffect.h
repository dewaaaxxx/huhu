#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace PocketEffect {

struct Burst {
    bool active = false;
    ImVec2 position = ImVec2(0.0f, 0.0f);
    ImVec4 color = ImVec4(1.0f, 0.65f, 0.1f, 1.0f);
    float age = 0.0f;
};

inline std::array<Ball::State, 32> g_previousStates{};
inline std::array<Burst, 12> g_bursts{};
inline int g_previousCount = -1;
inline ptr g_previousTable = 0;
inline bool g_initialized = false;

inline bool IsPocketed(Ball::State state) {
    return state == Ball::State::IN_POCKET || state == Ball::State::POTTED;
}

inline ImVec4 ColorForBall(Ball::Classification classification) {
    switch (classification) {
        case Ball::Classification::CUE_BALL:
            return ImVec4(0.92f, 0.96f, 1.0f, 1.0f);
        case Ball::Classification::SOLID:
            return ImVec4(1.0f, 0.24f, 0.08f, 1.0f);
        case Ball::Classification::STRIPE:
            return ImVec4(0.12f, 0.72f, 1.0f, 1.0f);
        case Ball::Classification::EIGHT_BALL:
            return ImVec4(0.78f, 0.18f, 1.0f, 1.0f);
        default:
            return ImVec4(1.0f, 0.72f, 0.08f, 1.0f);
    }
}

inline void StartBurst(Ball ball) {
    const Vec2d world = ball.position();
    const ImVec2 screen = WorldToScreen(world);
    if (!std::isfinite(screen.x) || !std::isfinite(screen.y)) return;

    int slot = -1;
    float oldest = -1.0f;
    for (int i = 0; i < (int)g_bursts.size(); ++i) {
        if (!g_bursts[i].active) {
            slot = i;
            break;
        }
        if (g_bursts[i].age > oldest) {
            oldest = g_bursts[i].age;
            slot = i;
        }
    }

    g_bursts[slot].active = true;
    g_bursts[slot].position = screen;
    g_bursts[slot].color = ColorForBall(ball.classification());
    g_bursts[slot].age = 0.0f;
}

inline void DrawBurst(ImDrawList* draw, Burst& burst, float dt) {
    if (!burst.active || !draw) return;

    constexpr float duration = 0.92f;
    burst.age += dt;
    const float t = burst.age / duration;
    if (t >= 1.0f) {
        burst.active = false;
        return;
    }

    constexpr float twoPi = 6.28318530717958647692f;
    const float fade = 1.0f - t;
    const float flash = std::max(0.0f, 1.0f - t * 5.0f);
    const float pulse = 0.82f + 0.18f * std::sin(burst.age * 26.0f);
    const float radius = (18.0f + 145.0f * t) * pulse;
    const ImVec4 c = burst.color;
    const auto rgba = [&](float alpha) -> ImU32 {
        return IM_COL32((int)(c.x * 255.0f), (int)(c.y * 255.0f),
                        (int)(c.z * 255.0f), (int)(std::max(0.0f, std::min(1.0f, alpha)) * 255.0f));
    };

    // نواة نارية بيضاء مع طبقات وهج واسعة.
    draw->AddCircleFilled(burst.position, radius * 0.72f, rgba(0.08f * fade), 48);
    draw->AddCircleFilled(burst.position, radius * 0.46f, rgba(0.16f * fade), 48);
    draw->AddCircleFilled(burst.position, radius * (0.16f + 0.10f * flash), rgba(0.95f * fade), 32);
    draw->AddCircleFilled(burst.position, radius * 0.065f, IM_COL32(255, 255, 255, (int)(255.0f * fade)), 24);

    // حلقات نيون متوسعة، مع حلقة ثانية بلون أبيض لزيادة اللمعان.
    for (int ring = 0; ring < 3; ++ring) {
        const float ringRadius = radius * (0.58f + ring * 0.19f);
        draw->AddCircle(burst.position, ringRadius, rgba((0.78f - ring * 0.16f) * fade), 48,
                        (6.0f - ring * 1.2f) * fade + 1.5f);
    }
    draw->AddCircle(burst.position, radius * 0.30f, IM_COL32(255, 255, 255, (int)(185.0f * fade)), 40, 2.5f * fade + 1.0f);

    // أشعة نارية طويلة ومتفاوتة الطول.
    const int rays = 22;
    for (int i = 0; i < rays; ++i) {
        const float angle = (float)i * (twoPi / (float)rays) + burst.age * 2.8f;
        const float inner = radius * (0.48f + 0.12f * std::sin(angle * 2.0f));
        const float outer = radius * (1.05f + 0.34f * std::sin(angle * 3.0f + burst.age * 7.0f));
        const ImVec2 p1(burst.position.x + std::cos(angle) * inner,
                        burst.position.y + std::sin(angle) * inner);
        const ImVec2 p2(burst.position.x + std::cos(angle) * outer,
                        burst.position.y + std::sin(angle) * outer);
        draw->AddLine(p1, p2, rgba(0.88f * fade), 3.8f * fade + 1.0f);
    }

    // جزيئات ملونة صغيرة تبتعد عن نقطة الجيب.
    const int particles = 16;
    for (int i = 0; i < particles; ++i) {
        const float angle = (float)i * (twoPi / (float)particles) - burst.age * 3.5f;
        const float distance = radius * (0.82f + 0.34f * std::sin((float)i * 4.0f + burst.age * 8.0f));
        const float particleR = (2.0f + 4.0f * fade) * (0.7f + 0.3f * std::sin((float)i));
        const ImVec2 p(burst.position.x + std::cos(angle) * distance,
                       burst.position.y + std::sin(angle) * distance);
        draw->AddCircleFilled(p, particleR, rgba(0.95f * fade), 12);
    }
}

inline void Reset() {
    g_initialized = false;
    g_previousCount = -1;
    g_previousTable = 0;
    for (auto& burst : g_bursts) burst.active = false;
}

inline void Update(Table table, ImDrawList* draw) {
    if (!table) {
        Reset();
        return;
    }

    const ptr currentTable = (ptr)table;
    if (g_previousTable != 0 && g_previousTable != currentTable) {
        Reset();
    }
    g_previousTable = currentTable;

    auto& balls = table.mBalls();
    const int count = balls ? (int)balls.Count : 0;
    const int limit = std::min(count, (int)g_previousStates.size());

    // The first frame of a new table only seeds state; it must not create fake bursts.
    if (!g_initialized || count != g_previousCount) {
        for (int i = 0; i < limit; ++i) {
            Ball ball = balls[i];
            g_previousStates[i] = ball ? ball.state() : Ball::State::UNKNOWN;
        }
        g_previousCount = count;
        g_initialized = true;
    } else {
        for (int i = 0; i < limit; ++i) {
            Ball ball = balls[i];
            if (!ball) continue;

            const Ball::State current = ball.state();
            const Ball::State previous = g_previousStates[i];
            if (IsPocketed(current) && !IsPocketed(previous)) {
                StartBurst(ball);
            }
            g_previousStates[i] = current;
        }
    }

    const float dt = ImGui::GetIO().DeltaTime > 0.0f ? ImGui::GetIO().DeltaTime : (1.0f / 60.0f);
    for (auto& burst : g_bursts) DrawBurst(draw, burst, dt);
}

} // namespace PocketEffect

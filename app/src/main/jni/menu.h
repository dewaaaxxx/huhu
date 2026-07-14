#pragma once
#include "include/includes.h"
#include "game.h"
#include "game/Ruleset.h"
#include "imgui/inc/8bp.h"
#include "mod/keylogin.h"
#include "oxorany/oxorany.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <sys/system_properties.h>
#include <ctime>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdarg>
#include <Vector/Vectors.h>
#include <imgui/imgui.h>
#include "icons/icons.h"

using namespace ImGui;
using namespace std;

struct MenuState {
    bool isOpen      = false;
    int  currentTab  = 0;
    float sidebarWidth = 820.0f;
    float menuAlpha  = 0.0f;
    ImVec2 menuPos = ImVec2(0.0f, 0.0f);
    bool menuPosInitialized = false;
};
static MenuState g_menu;
static bool g_isInGame = false;

// ── Theme color globals ───────────────────────────────────────
static ImU32 g_ThemeAccent = IM_COL32(231,171,42,255);
static ImU32 g_ThemeBorder = IM_COL32(220,177,72,120);
static ImU32 g_ThemeGlow   = IM_COL32(150,101,16,235);
static ImU32 g_ThemeText   = IM_COL32(245,197,74,255);

static void ApplyTheme(int t) {
    switch (t) {
        case 1: // Red
            g_ThemeAccent=IM_COL32(220,55,55,255); g_ThemeBorder=IM_COL32(200,45,45,130);
            g_ThemeGlow  =IM_COL32(180,25,25,235); g_ThemeText  =IM_COL32(255,110,110,255); break;
        case 2: // Cyan Neon
            g_ThemeAccent=IM_COL32(25,215,195,255); g_ThemeBorder=IM_COL32(15,195,175,130);
            g_ThemeGlow  =IM_COL32(8,140,130,235);  g_ThemeText  =IM_COL32(70,235,215,255); break;
        case 3: // Purple
            g_ThemeAccent=IM_COL32(155,75,220,255); g_ThemeBorder=IM_COL32(135,55,200,130);
            g_ThemeGlow  =IM_COL32(115,35,175,235); g_ThemeText  =IM_COL32(185,115,255,255); break;
        case 4: // Green
            g_ThemeAccent=IM_COL32(45,195,75,255);  g_ThemeBorder=IM_COL32(35,175,55,130);
            g_ThemeGlow  =IM_COL32(18,135,38,235);  g_ThemeText  =IM_COL32(95,225,125,255); break;
        case 5: // Blue
            g_ThemeAccent=IM_COL32(50,130,230,255); g_ThemeBorder=IM_COL32(40,110,210,130);
            g_ThemeGlow  =IM_COL32(25,80,180,235);  g_ThemeText  =IM_COL32(100,175,255,255); break;
        default: // Gold
            g_ThemeAccent=IM_COL32(231,171,42,255); g_ThemeBorder=IM_COL32(220,177,72,120);
            g_ThemeGlow  =IM_COL32(150,101,16,235); g_ThemeText  =IM_COL32(245,197,74,255); break;
    }
}

static bool DEBUG_BYPASS_LOGIN = false;
static char g_licenseKeyInput[128] = "";
static bool g_licenseKeyInputInitialized = false;

static const char* BoolText(bool value) {
    return value ? O("Yes") : O("No");
}

static void FormatRealtime(char* out, size_t outSize) {
    time_t now = time(nullptr);
    struct tm tmNow{};
    localtime_r(&now, &tmNow);
    strftime(out, outSize, O("%Y-%m-%d %H:%M:%S"), &tmNow);
}

static std::string AndroidProperty(const char* name, const char* fallback = "Unknown") {
    char value[PROP_VALUE_MAX] = {0};
    int len = __system_property_get(name, value);
    return len > 0 ? std::string(value) : std::string(fallback);
}

static void TextWrappedColored(const ImVec4& color, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    PushTextWrapPos(GetCursorPosX() + GetContentRegionAvail().x);
    TextColoredV(color, fmt, args);
    PopTextWrapPos();
    va_end(args);
}

static std::string NormalizeLicenseKey(std::string key) {
    key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) {
        return std::isspace(c);
    }), key.end());
    return key;
}


static float EaseOutBack(float x) {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    return 1.0f + c3 * powf(x - 1.0f, 3.0f) + c1 * powf(x - 1.0f, 2.0f);
}

static float EaseOutQuart(float x) {
    return 1.0f - powf(1.0f - x, 4.0f);
}

static void DrawGradientRect(ImDrawList* dl, ImVec2 p1, ImVec2 p2, ImU32 col1, ImU32 col2, bool horizontal = true) {
    if (horizontal) {
        dl->AddRectFilledMultiColor(p1, p2, col1, col2, col2, col1);
    } else {
        dl->AddRectFilledMultiColor(p1, p2, col1, col1, col2, col2);
    }
}

static bool GameNavButton(const char* label, int iconType, bool selected, float width, float height) {
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);

    ImVec2 pos  = window->DC.CursorPos;
    ImVec2 size = ImVec2(width, height);
    const ImRect bb(pos, pos + size);
    ItemSize(size, style.FramePadding.y);
    if (!ItemAdd(bb, id)) return false;

    bool hovered, held;
    bool pressed = ButtonBehavior(bb, id, &hovered, &held);

    ImDrawList* dl = window->DrawList;
    ImU32 bg = selected ? IM_COL32(28, 28, 27, 245) : (hovered ? IM_COL32(24, 24, 23, 220) : IM_COL32(0, 0, 0, 0));
    if (selected || hovered) {
        dl->AddRectFilled(bb.Min, bb.Max, bg, 12.0f);
    }

    ImVec2 center(bb.Min.x + width * 0.5f, bb.Min.y + 24.0f);
    ImU32 iconCol = selected ? g_ThemeAccent : IM_COL32(210, 210, 210, 225);
    ImU32 textCol = selected ? g_ThemeText   : IM_COL32(205, 205, 205, 230);

    if (iconType == 0) { // eye / visual
        dl->PathClear();
        for (int i = 0; i < 28; ++i) {
            float a = ((float)i / 28.0f) * (IM_PI * 2.0f);
            dl->PathLineTo(ImVec2(center.x + cosf(a) * 17.0f, center.y + sinf(a) * 9.0f));
        }
        dl->PathStroke(iconCol, true, 2.0f);
        dl->AddCircleFilled(center, 4.5f, iconCol, 16);
    } else if (iconType == 1) { // crosshair / aim
        dl->AddCircle(center, 10.5f, iconCol, 24, 2.0f);
        dl->AddLine(ImVec2(center.x - 16, center.y), ImVec2(center.x - 6, center.y), iconCol, 2.0f);
        dl->AddLine(ImVec2(center.x + 6, center.y), ImVec2(center.x + 16, center.y), iconCol, 2.0f);
        dl->AddLine(ImVec2(center.x, center.y - 16), ImVec2(center.x, center.y - 6), iconCol, 2.0f);
        dl->AddLine(ImVec2(center.x, center.y + 6), ImVec2(center.x, center.y + 16), iconCol, 2.0f);
    } else if (iconType == 2) { // simple cog / misc
        dl->AddCircle(center, 10.0f, iconCol, 20, 2.0f);
        for (int i = 0; i < 8; ++i) {
            float a = (float)i * IM_PI / 4.0f;
            ImVec2 p1(center.x + cosf(a) * 13.0f, center.y + sinf(a) * 13.0f);
            ImVec2 p2(center.x + cosf(a) * 17.0f, center.y + sinf(a) * 17.0f);
            dl->AddLine(p1, p2, iconCol, 2.0f);
        }
        dl->AddCircleFilled(center, 3.0f, iconCol, 12);
    } else { // user
        dl->AddCircle(ImVec2(center.x, center.y - 6), 6.5f, iconCol, 16, 2.0f);
        dl->AddBezierCubic(ImVec2(center.x - 15, center.y + 13), ImVec2(center.x - 10, center.y + 2),
                           ImVec2(center.x + 10, center.y + 2), ImVec2(center.x + 15, center.y + 13), iconCol, 2.2f, 18);
    }

    ImVec2 labelSize = CalcTextSize(label);
    dl->AddText(ImVec2(bb.Min.x + (width - labelSize.x) * 0.5f, bb.Min.y + 48.0f), textCol, label);

    if (selected) {
        dl->AddRectFilled(ImVec2(bb.Max.x - 5.0f, bb.Min.y + 12.0f),
                          ImVec2(bb.Max.x, bb.Max.y - 12.0f), g_ThemeAccent, 4.0f);
    }

    return pressed;
}

static void DrawSectionTitle(const char* icon, const char* title, const char* subtitle = nullptr) {
    ImDrawList* dl = GetWindowDrawList();
    ImVec2 pos = GetCursorScreenPos();
    ImVec2 avail = GetContentRegionAvail();
    SetWindowFontScale(1.08f);
    TextColored(ImVec4(0.95f, 0.72f, 0.22f, 1.0f), "[%s]", icon);
    SameLine();
    TextColored(ImVec4(0.92f, 0.92f, 0.90f, 1.0f), "%s", title);
    SetWindowFontScale(1.0f);
    if (subtitle) {
        PushTextWrapPos(GetCursorPosX() + avail.x);
        TextColored(ImVec4(0.62f, 0.62f, 0.62f, 1.0f), "%s", subtitle);
        PopTextWrapPos();
    }
    float y = GetCursorScreenPos().y + 6.0f;
    dl->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + avail.x, y), IM_COL32(98, 75, 28, 150), 1.0f);
    Dummy(ImVec2(0, 13.0f));
}


static void DrawGoldBar(const char* label, const char* value, float valueRatio, float height = 48.0f) {
    ImGuiWindow* window = GetCurrentWindow();
    ImDrawList* dl = window->DrawList;
    ImVec2 pos = GetCursorScreenPos();
    float w = GetContentRegionAvail().x;
    ImVec2 size(w, height);

    InvisibleButton(label, size);
    ImU32 frameCol = IM_COL32(27, 27, 25, 235);
    ImU32 fillCol = IM_COL32(212, 152, 30, 245);
    dl->AddRectFilled(pos, pos + size, frameCol, 8.0f);
    dl->AddRect(pos, pos + size, IM_COL32(55, 45, 25, 210), 8.0f, 0, 1.0f);
    float fillW = ImClamp(w * valueRatio, 0.0f, w);
    dl->AddRectFilled(pos, ImVec2(pos.x + fillW, pos.y + height), fillCol, 8.0f);

    ImVec2 vSize = CalcTextSize(value);
    dl->AddText(ImVec2(pos.x + w * 0.5f - vSize.x * 0.5f, pos.y + height * 0.5f - vSize.y * 0.5f),
                IM_COL32(255, 230, 157, 255), value);
    ImVec2 lSize = CalcTextSize(label);
    dl->AddText(ImVec2(pos.x + w + 12.0f, pos.y + height * 0.5f - lSize.y * 0.5f),
                IM_COL32(245, 196, 78, 255), label);
}


static bool ToggleSwitch(const char* label, bool* v) {
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);

    float scale = 1.5f;
    float height = 32.0f * scale;
    float width  = 56.0f * scale;
    float radius = height * 0.5f;

    ImVec2 textSize = CalcTextSize(label);
    ImVec2 pos  = window->DC.CursorPos;
    ImVec2 size = ImVec2(GetContentRegionAvail().x, ImMax(height, textSize.y) + style.FramePadding.y * 2 + 10.0f);

    const ImRect bb(pos, pos + size);
    ItemSize(size, style.FramePadding.y);
    if (!ItemAdd(bb, id)) return false;

    bool hovered, held;
    bool pressed = ButtonBehavior(bb, id, &hovered, &held);
    if (pressed) *v = !*v;

    static std::map<ImGuiID, float> switchAnim;
    float& animT   = switchAnim[id];
    float targetT  = *v ? 1.0f : 0.0f;
    animT += (targetT - animT) * g.IO.DeltaTime * 14.0f;

    ImDrawList* dl = window->DrawList;

    if (hovered) {
        dl->AddRectFilled(bb.Min, bb.Max, IM_COL32(45, 45, 55, 100), 10.0f);
    }

    ImVec2 togglePos = ImVec2(bb.Max.x - width - 15.0f, bb.Min.y + (size.y - height) * 0.5f);
    ImVec2 toggleEnd = ImVec2(togglePos.x + width, togglePos.y + height);

    ImVec4 offColor = ImVec4(0.27f, 0.27f, 0.31f, 1.0f);
    ImVec4 onColor  = ImVec4(1.0f, 0.f, 0.f, 1.0f);
    ImVec4 bgColorV = ImLerp(offColor, onColor, animT);
    dl->AddRectFilled(togglePos, toggleEnd, ImColor(bgColorV), radius);

    float knobX = togglePos.x + radius + (width - height) * animT;
    float knobY = togglePos.y + radius;
    float knobR = radius - 4.0f;

    dl->AddCircleFilled(ImVec2(knobX, knobY), knobR + 2.0f, IM_COL32(0, 0, 0, 40));
    dl->AddCircleFilled(ImVec2(knobX, knobY), knobR, IM_COL32(255, 255, 255, 255));

    dl->AddText(ImVec2(bb.Min.x + 15.0f, bb.Min.y + (size.y - textSize.y) * 0.5f),
                IM_COL32(230, 230, 240, 255), label);

    return pressed;
}

static bool IsExpired() {
    return (g_ExpiryTimestamp > 0) && ((int64_t)time(nullptr) >= g_ExpiryTimestamp);
}

INLINE void DrawExpired(ImGuiIO& io) {
    float winW = g_menu.sidebarWidth;

    SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    SetNextWindowSize(ImVec2(winW, 0), ImGuiCond_Always);
    PushStyleColor(ImGuiCol_WindowBg, IM_COL32(21, 21, 21, 255));
    PushStyleVar(ImGuiStyleVar_WindowRounding, 20.0f);
    PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30.0f, 30.0f));
    PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    if (Begin(O("##ExpiredWin"), nullptr,
              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
              ImGuiWindowFlags_AlwaysAutoResize)) {

        SetWindowFontScale(1.6f);
        ImVec2 titleSz = CalcTextSize(O("MOD EXPIRED"));
        SetCursorPosX((winW - 60.0f - titleSz.x) * 0.5f);
        TextColored(ImVec4(1.0f, 0.1f, 0.1f, 1.0f), "%s", O("MOD EXPIRED"));
        SetWindowFontScale(1.0f);

        Dummy(ImVec2(0, 16));

        PushTextWrapPos(GetCursorPosX() + winW - 60.0f);
        TextColored(ImVec4(0.85f, 0.85f, 0.90f, 1.0f), "%s",
            O("Beta Version Expired. Update on our Telegram Your Id"));
        PopTextWrapPos();

        Dummy(ImVec2(0, 10));
    }
    End();
    PopStyleVar(3);
    PopStyleColor();
}

static bool g_aqCounting = false;
static std::chrono::steady_clock::time_point g_aqLastCall;
static std::chrono::steady_clock::time_point g_aqCountdownStart;

INLINE void DrawAutoQueue() {
    if ((!g_Token.empty() && !g_Auth.empty() && g_Token == g_Auth) || DEBUG_BYPASS_LOGIN) {
        auto now = std::chrono::steady_clock::now();

        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_aqLastCall).count() > 500)
            g_aqCounting = false;
        g_aqLastCall = now;

        if (!g_aqCounting) {
            g_aqCounting = true;
            g_aqCountdownStart = now;
        }

        auto elapsed     = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_aqCountdownStart).count();
        int  remaining_ms = 3000 - (int)elapsed;
        std::string count_str = std::to_string((remaining_ms / 1000) + 1);

        SetNextWindowPos(ImVec2(Width * 0.5f, Height * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1.f));
        PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32.0f, 20.0f));
        PushStyleVar(ImGuiStyleVar_WindowRounding, 24.0f);

        if (Begin(O("##AutoQueueCD"), nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                  ImGuiWindowFlags_AlwaysAutoResize)) {
            ImDrawList* dl  = GetWindowDrawList();
            ImVec2      wp  = GetWindowPos();
            ImVec2      ws  = GetWindowSize();
            dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), IM_COL32(20, 20, 28, 0), 24.0f);
            SetWindowFontScale(3.5f);
            TextColored(ImVec4(1.f, 0.f, 0.f, 1.0f), "%s", count_str.c_str());
            SetWindowFontScale(1.0f);
        }
        End();
        PopStyleVar(2);
        PopStyleColor();
    }
}

#include "mod/ButtonClicker.h"

static void DrawToggleButton(bool cancelMode);

static bool  g_autoPlayCalculating = false;
static float g_sideBtnsX      = 0.0f;
static float g_sideBtnsY      = 0.0f;
static float g_toggleRotAngle = 0.0f;

INLINE void DrawESP(ImDrawList* draw) {
    if ((!g_Token.empty() && !g_Auth.empty() && g_Token == g_Auth) || DEBUG_BYPASS_LOGIN) {
        if (!sharedGameManager) return;

        UpdateScreenTable();

        sharedDirector = F(ptr, libmain + O(0x4f06288));
        if (!sharedDirector) return;

        sharedUserInfo = F(ptr, libmain + O(0x4e9feb8));
        if (!sharedUserInfo) return;

        F(bool, sharedUserInfo + 0x340) = true;

        sharedMainManager = F(ptr, libmain + O(0x4dde3e0));
        if (!sharedMainManager) return;

        sharedMenuManager = F(ptr, libmain + O(0x4dfe838));
        if (!sharedMenuManager) return;

        MainStateManager mainStateManager = sharedMainManager.mStateManager;
        if (!mainStateManager) return;

        g_isInGame = mainStateManager.isInGame();

        if (!g_isInGame) {
            if (persistent_bool[O("bAutoQueue")]) {
                if (!sharedMenuManager.isInQueue()) DrawAutoQueue();
                DrawToggleButton(true);
            }
            return;
        }

        auto visualCue = sharedGameManager.mVisualCue();

        Ball::Classification myclass = sharedGameManager.getPlayerClassification();

        Table table = sharedGameManager.mTable;
        if (!table) return;

        auto tableProperties = table.mTableProperties();
        if (!tableProperties) return;

        auto& pockets = tableProperties.mPockets();

        GameStateManager gameStateManager = sharedGameManager.mStateManager;
        if (!gameStateManager) return;

        if (persistent_bool[O("bAutoPlay")]) {
            DrawToggleButton(false);
            AutoPlay::Update();
        }

        auto stateId = gameStateManager.getCurrentStateId();
        if (stateId == 4 || stateId == 7 || stateId == 8) gPrediction->determineShotResult(false);

        // ── SHOOT ANIMATION ─────────────────────────────────────────────────
        // Detect transisi state 4→6 (tembakan dilepas) → expanding ring flash
        static int    s_prevState  = 0;
        static float  s_shotTime   = -999.0f;
        static ImVec2 s_shotOrigin = ImVec2(0, 0);
        if (s_prevState == 4 && stateId == 6 && gPrediction && gPrediction->guiData.ballsCount > 0) {
            s_shotTime = (float)GetTime();
            for (int ci = 0; ci < gPrediction->guiData.ballsCount; ci++) {
                auto& cb = gPrediction->guiData.balls[ci];
                if (cb.classification == Ball::Classification::CUE_BALL && cb.onTable) {
                    s_shotOrigin = WorldToScreen(cb.initialPosition);
                    break;
                }
            }
        }
        s_prevState = stateId;

        // Track giliran lokal player: pakai sharedGameManager.mStateManager() persis seperti AutoPlay
        static bool s_localTurn = false;
        if (stateId == 4 || stateId == 7 || stateId == 8)
            s_localTurn = sharedGameManager.mStateManager().isPlayerTurn();
        else if (stateId != 6)
            s_localTurn = false;

        if (persistent_bool[O("bESP_ShootAnim")]) {
            float el = (float)GetTime() - s_shotTime;
            if (el >= 0.0f && el < 0.9f) {
                float prog = el / 0.9f;
                // 3 cincin yang mengembang dengan delay berselang
                for (int r = 0; r < 3; r++) {
                    float rp = fmodf(prog + r * 0.333f, 1.0f);
                    draw->AddCircle(s_shotOrigin, 15.0f + rp * 95.0f,
                                    IM_COL32(255, 210, 60, (ImU8)(190*(1.0f-rp))), 32, 3.0f);
                }
                // Flash awal setelah tembak
                if (prog < 0.18f) {
                    float fa = (0.18f - prog) / 0.18f;
                    draw->AddCircleFilled(s_shotOrigin, 36.0f, IM_COL32(255, 230, 80, (ImU8)(110*fa)));
                    draw->AddCircle(s_shotOrigin, 18.0f, IM_COL32(255, 255, 120, (ImU8)(220*fa)), 24, 4.0f);
                }
            }
        }

        // ── BALL ROLLING ANIMATION (state 6) ────────────────────────────────
        // Hanya tampil saat giliran local player (s_localTurn = true dari state 4/7/8)
        if (stateId == 6) {
            if (s_localTurn && persistent_bool[O("bESP_RollingAnim")] && persistent_bool[O("bESP_DrawPredictionLine")] && gPrediction && gPrediction->guiData.ballsCount > 0) {
                float t = (float)GetTime();
                for (int i = 0; i < gPrediction->guiData.ballsCount; i++) {
                    auto& ball = gPrediction->guiData.balls[i];
                    if (!ball.onTable || ball.initialPosition == ball.predictedPosition) continue;
                    ImVec2 destPos = WorldToScreen(ball.predictedPosition);
                    float pulse = 0.5f + 0.5f * sinf(t * 6.5f + i * 0.85f);
                    // Outer pulsing ring
                    draw->AddCircle(destPos, 22.0f + pulse*7.0f,
                                    IM_COL32(255, 200, 50, (ImU8)(55+40*pulse)), 32, 3.0f);
                    // Inner fill
                    draw->AddCircleFilled(destPos, 13.0f,
                                          IM_COL32(255, 200, 50, (ImU8)(18+18*pulse)), 20);
                    // 4 orbit dots yang berputar
                    for (int k = 0; k < 4; k++) {
                        float a = t * 5.5f + k * IM_PI * 0.5f;
                        ImVec2 orb(destPos.x + cosf(a)*18.0f, destPos.y + sinf(a)*18.0f);
                        draw->AddCircleFilled(orb, 3.5f, IM_COL32(255, 205, 50, 215), 8);
                    }
                }
            }
            return;
        }

        if (persistent_bool[O("bESP_DrawPocketsShotState")]) {
            for (int i = 0; i < 6; i++) {
                if (Prediction::pocketStatus[i]) {
                    auto screenPos = WorldToScreen(pockets[i]);
                    draw->AddCircle(ImVec2(screenPos.x, screenPos.y), 30, GREEN, 0, 5.f);
                }
            }
        }

        if (persistent_bool[O("bESP_DrawEnemyLines")] && gPrediction->guiData.ballsCount > 0) {
            // ── ENEMY LINES (animated dashed cyan, no red) ──────────────────
            ImVec2 cuePos = ImVec2(-1.f, -1.f);
            for (int i = 0; i < gPrediction->guiData.ballsCount; i++) {
                auto& cb = gPrediction->guiData.balls[i];
                if (cb.classification == Ball::Classification::CUE_BALL && cb.onTable) {
                    cuePos = WorldToScreen(cb.initialPosition);
                    break;
                }
            }
            if (cuePos.x < 0.f) goto skip_enemy_lines;

            {
                float dashT = fmodf((float)GetTime() * 1.8f, 1.0f);
                for (int i = 0; i < gPrediction->guiData.ballsCount; i++) {
                    auto& ball = gPrediction->guiData.balls[i];
                    if (!ball.originalOnTable || !ball.onTable) continue;
                    if (ball.classification == Ball::Classification::CUE_BALL) continue;
                    if (ball.classification == Ball::Classification::ERR_CLASSIFICATION) continue;

                    bool isEnemy = false;
                    if (myclass == Ball::Classification::SOLID) {
                        isEnemy = ball.classification == Ball::Classification::STRIPE ||
                                  ball.classification == Ball::Classification::EIGHT_BALL;
                    } else if (myclass == Ball::Classification::STRIPE) {
                        isEnemy = ball.classification == Ball::Classification::SOLID ||
                                  ball.classification == Ball::Classification::EIGHT_BALL;
                    } else {
                        isEnemy = true;
                    }
                    if (!isEnemy) continue;

                    ImVec2 enemyPos = WorldToScreen(ball.initialPosition);

                    // Dashed animated line dari cue ke bola musuh — cyan, bukan merah
                    ImVec2 ld(enemyPos.x - cuePos.x, enemyPos.y - cuePos.y);
                    float dist = sqrtf(ld.x*ld.x + ld.y*ld.y);
                    if (dist > 1.0f) {
                        ld.x /= dist; ld.y /= dist;
                        const float dLen = 13.0f, gLen = 8.0f, seg = dLen + gLen;
                        float off = dashT * seg;
                        for (float d = off - seg; d < dist; d += seg) {
                            float d0 = d < 0.0f ? 0.0f : d;
                            float d1 = (d + dLen) > dist ? dist : (d + dLen);
                            if (d1 <= d0) continue;
                            float fade = 1.0f - d0 / dist;
                            ImVec2 p0(cuePos.x + ld.x*d0, cuePos.y + ld.y*d0);
                            ImVec2 p1(cuePos.x + ld.x*d1, cuePos.y + ld.y*d1);
                            draw->AddLine(p0, p1, IM_COL32(80, 200, 255, (ImU8)(165*fade)), 2.2f);
                        }
                    }

                    // Pulsing ring highlight di bola musuh
                    float pulse = 0.5f + 0.5f * sinf((float)GetTime() * 4.5f + i * 1.1f);
                    float pr = 13.0f + pulse * 4.0f;
                    draw->AddCircleFilled(enemyPos, pr, IM_COL32(80, 200, 255, (ImU8)(28+22*pulse)), 24);
                    draw->AddCircle(enemyPos, pr, IM_COL32(80, 200, 255, (ImU8)(165+70*pulse)), 24, 2.2f);
                }
            }
            skip_enemy_lines:;
        }

        if (persistent_bool[O("bESP_DrawPredictionLine")]) {
            float lineThick = (float)persistent_int[O("iLineThickness")];
            if (lineThick < 1.f) lineThick = 1.f;

            const ImVec2& dsp = GetIO().DisplaySize;
            const float margin = 300.0f;

            // Pass 1: garis lintasan prediksi
            for (int i = 0; i < gPrediction->guiData.ballsCount; i++) {
                auto& ball = gPrediction->guiData.balls[i];
                if (ball.initialPosition == ball.predictedPosition) continue;
                if (ball.positions.size() < 2) continue;
                // Inisialisasi dari positions[0] agar segmen pertama ikut tergambar
                ImVec2 lastPos = WorldToScreen(ball.positions[0]);
                for (int j = 1; j < (int)ball.positions.size(); j++) {
                    ImVec2 point = WorldToScreen(ball.positions[j]);
                    // Skip titik yang jauh di luar layar (posisi fisika off-table)
                    bool lastOk = (lastPos.x > -margin && lastPos.x < dsp.x + margin &&
                                   lastPos.y > -margin && lastPos.y < dsp.y + margin);
                    bool pointOk = (point.x > -margin && point.x < dsp.x + margin &&
                                    point.y > -margin && point.y < dsp.y + margin);
                    if (lastOk && pointOk)
                        draw->AddLine(lastPos, point, colors[i], lineThick);
                    lastPos = point;
                }
            }

            // Pass 2: circle di posisi awal (outline) & prediksi (filled) — persis seperti source
            for (int i = 0; i < gPrediction->guiData.ballsCount; i++) {
                auto& ball = gPrediction->guiData.balls[i];
                if (ball.initialPosition == ball.predictedPosition) continue;
                draw->AddCircle(WorldToScreen(ball.initialPosition), 20.0f, colors[i], 0, 6.0f);
                draw->AddCircleFilled(WorldToScreen(ball.predictedPosition), 20.0f, colors[i]);
            }
        }
    }
}

static void DrawSidebar(float winH, float sidebarW) {
    ImDrawList* dl = GetWindowDrawList();
    ImVec2 wp = GetWindowPos();

    // Hitung sizeScale dari sidebarW agar semua dimensi proporsional
    const float ss = sidebarW / 170.0f;

    dl->AddRectFilled(wp, ImVec2(wp.x + sidebarW, wp.y + winH), IM_COL32(12, 13, 12, 242), 16.0f,
                      ImDrawFlags_RoundCornersLeft);
    {
        float gTop = 62.0f * ss, gBot = winH - 35.0f * ss;
        if (gTop < gBot)
            dl->AddRectFilled(ImVec2(wp.x + sidebarW - 5.0f, wp.y + gTop),
                              ImVec2(wp.x + sidebarW,         wp.y + gBot), g_ThemeGlow, 5.0f);
    }
    dl->AddLine(ImVec2(wp.x + 18.0f * ss, wp.y + 116.0f * ss),
                ImVec2(wp.x + sidebarW - 18.0f * ss, wp.y + 116.0f * ss),
                IM_COL32(92, 70, 25, 180), 1.0f);

    // LYN4XP logo
    {
        static GLuint lyn4xp_tex = LoadTextureFromMemory(lyn4xp_logo_png, lyn4xp_logo_png_len);
        const float iconSz = 52.0f * ss;
        ImVec2 iconMin(wp.x + sidebarW * 0.5f - iconSz * 0.5f, wp.y + 46.0f * ss);
        ImVec2 iconMax(iconMin.x + iconSz, iconMin.y + iconSz);
        if (lyn4xp_tex)
            dl->AddImageRounded((void*)(intptr_t)lyn4xp_tex, iconMin, iconMax,
                                ImVec2(0,0), ImVec2(1,1), IM_COL32(255,255,255,230), 8.0f * ss);
        dl->AddRect(iconMin, iconMax, g_ThemeAccent, 8.0f * ss, 0, 1.5f);
    }

    // Dimensi nav button — semua di-scale agar tidak overflow winH
    const float navX  = 14.0f * ss;
    const float navW  = sidebarW - navX * 2.0f;
    const float navH  = 64.0f * ss;
    const float gap   = 7.0f  * ss;
    const float navY0 = 135.0f * ss;  // Y awal button pertama

    // Label pakai char[] bukan O() agar tidak dangling pointer
    static const char* labels[4] = { "Visual", "Aim", "Settings", "User" };

    ImGuiIO& io = GetIO();
    // Direct hit test — bypass ImGui item system sepenuhnya sehingga
    // clip rect dari BeginChild di DrawContentArea tidak memblokir klik.
    // Setiap tab dicek secara manual terhadap posisi sentuhan.
    for (int i = 0; i < 4; ++i) {
        float btnY = navY0 + i * (navH + gap);
        if (btnY + navH > winH + 1.0f) continue;  // skip jika overflow window

        ImVec2 bMin(wp.x + navX, wp.y + btnY);
        ImVec2 bMax(bMin.x + navW, bMin.y + navH);
        bool hov = ImRect(bMin, bMax).Contains(io.MousePos);
        // io.MouseClicked[0] adalah flag global — tidak bergantung pada clip rect
        if (hov && io.MouseClicked[0]) g_menu.currentTab = i;
        bool sel = (g_menu.currentTab == i);

        // Background tombol
        if (sel || hov) {
            ImU32 bg = sel ? IM_COL32(28,28,27,245) : IM_COL32(24,24,23,220);
            dl->AddRectFilled(bMin, bMax, bg, 12.0f);
        }

        ImU32 icCol = sel ? g_ThemeAccent : IM_COL32(210,210,210,225);
        ImU32 txCol = sel ? g_ThemeText   : IM_COL32(205,205,205,230);
        ImVec2 ctr(bMin.x + navW * 0.5f, bMin.y + navH * 0.375f);

        // Ikon (sama seperti GameNavButton, di-scale dengan ss)
        if (i == 0) { // mata / visual
            dl->PathClear();
            for (int j = 0; j < 28; ++j) {
                float a = (float)j / 28.0f * (IM_PI * 2.0f);
                dl->PathLineTo(ImVec2(ctr.x + cosf(a)*17.0f*ss, ctr.y + sinf(a)*9.0f*ss));
            }
            dl->PathStroke(icCol, true, 2.0f);
            dl->AddCircleFilled(ctr, 4.5f * ss, icCol, 16);
        } else if (i == 1) { // crosshair / aim
            float r = 10.5f*ss, arm = 16.0f*ss, g2 = 6.0f*ss;
            dl->AddCircle(ctr, r, icCol, 24, 2.0f);
            dl->AddLine(ImVec2(ctr.x-arm,ctr.y), ImVec2(ctr.x-g2,ctr.y), icCol, 2.0f);
            dl->AddLine(ImVec2(ctr.x+g2,ctr.y),  ImVec2(ctr.x+arm,ctr.y), icCol, 2.0f);
            dl->AddLine(ImVec2(ctr.x,ctr.y-arm), ImVec2(ctr.x,ctr.y-g2), icCol, 2.0f);
            dl->AddLine(ImVec2(ctr.x,ctr.y+g2),  ImVec2(ctr.x,ctr.y+arm), icCol, 2.0f);
        } else if (i == 2) { // gear / misc
            dl->AddCircle(ctr, 10.0f*ss, icCol, 20, 2.0f);
            for (int j = 0; j < 8; ++j) {
                float a = (float)j * IM_PI / 4.0f;
                dl->AddLine(ImVec2(ctr.x+cosf(a)*13.0f*ss, ctr.y+sinf(a)*13.0f*ss),
                             ImVec2(ctr.x+cosf(a)*17.0f*ss, ctr.y+sinf(a)*17.0f*ss), icCol, 2.0f);
            }
            dl->AddCircleFilled(ctr, 3.0f*ss, icCol, 12);
        } else { // orang / user
            dl->AddCircle(ImVec2(ctr.x, ctr.y - 6.0f*ss), 6.5f*ss, icCol, 16, 2.0f);
            dl->AddBezierCubic(
                ImVec2(ctr.x-15.0f*ss, ctr.y+13.0f*ss),
                ImVec2(ctr.x-10.0f*ss, ctr.y+2.0f*ss),
                ImVec2(ctr.x+10.0f*ss, ctr.y+2.0f*ss),
                ImVec2(ctr.x+15.0f*ss, ctr.y+13.0f*ss), icCol, 2.2f, 18);
        }

        // Label teks di bawah ikon
        ImVec2 lblSz = CalcTextSize(labels[i]);
        dl->AddText(ImVec2(bMin.x + (navW - lblSz.x)*0.5f, bMin.y + navH*0.72f), txCol, labels[i]);

        // Indikator aktif (bar kanan)
        if (sel)
            dl->AddRectFilled(ImVec2(bMax.x-5.0f, bMin.y+navH*0.19f),
                              ImVec2(bMax.x,       bMax.y-navH*0.19f), g_ThemeAccent, 4.0f);
    }
}


static std::string ReadNSString(ptr str) {
    if (!str) return "null";
    int32_t len = F(int32_t, str + 0x10);
    if (len <= 0 || len > 512) return "?";
    std::string result;
    result.reserve(len);
    for (int32_t i = 0; i < len; i++) {
        uint16_t ch = F(uint16_t, str + 0x14 + i * 2);
        result += (ch > 0 && ch < 128) ? (char)ch : '?';
    }
    return result;
}

static void svConfig_Save() {
    std::string path = O("/data/user/0/") + PACKAGE_NAME + O("/files/svConfig.txt");
    FILE* f = fopen(path.c_str(), O("w"));
    if (!f) return;
    fprintf(f, O("iLineThickness=%d\n"),  persistent_int[O("iLineThickness")]);
    fprintf(f, O("iMenuSizeOffset=%d\n"), persistent_int[O("iMenuSizeOffset")]);
    fprintf(f, O("iTheme=%d\n"),          persistent_int[O("iTheme")]);
    fclose(f);
}
static void svConfig_Load() {
    std::string path = O("/data/user/0/") + PACKAGE_NAME + O("/files/svConfig.txt");
    FILE* f = fopen(path.c_str(), O("r"));
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        int v = 0;
        if (sscanf(line, O("iLineThickness=%d"),  &v) == 1) { persistent_int[O("iLineThickness")]  = v; continue; }
        if (sscanf(line, O("iMenuSizeOffset=%d"), &v) == 1) { persistent_int[O("iMenuSizeOffset")] = v; continue; }
        if (sscanf(line, O("iTheme=%d"),          &v) == 1) { persistent_int[O("iTheme")]          = v; ApplyTheme(v); }
    }
    fclose(f);
}

static void DrawCalculating(ImGuiIO& io) {
    SetNextWindowPos(ImVec2(Width * 0.5f, Height * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    PushStyleColor(ImGuiCol_WindowBg, IM_COL32(21, 21, 21, 255));
    PushStyleColor(ImGuiCol_Border, IM_COL32(220, 30, 30, 255));
    PushStyleVar(ImGuiStyleVar_WindowRounding, 18.0f);
    PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);

    if (Begin(O("##CalcOverlay"), nullptr,
              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
              ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs)) {
        SetWindowFontScale(1.4f);
        TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), O("CALCULATING..."));
        SetWindowFontScale(1.0f);
    }
    End();
    PopStyleVar(2);
    PopStyleColor(2);
}

static void DrawContentArea(float winW, float winH, float sidebarW) {
    bool need_save = false;

    ImDrawList* dl = GetWindowDrawList();
    ImVec2 wp = GetWindowPos();
    float contentX = sidebarW;
    float headerH = 72.0f;
    float pad = 24.0f;
    float contentW = winW - sidebarW;

    if (g_menu.currentTab < 0 || g_menu.currentTab > 3) g_menu.currentTab = 0;

    dl->AddRectFilled(wp, ImVec2(wp.x + winW, wp.y + winH), IM_COL32(7, 8, 7, 230), 16.0f);
    dl->AddRectFilled(ImVec2(wp.x + contentX, wp.y), ImVec2(wp.x + winW, wp.y + winH),
                      IM_COL32(14, 15, 14, 238), 16.0f, ImDrawFlags_RoundCornersRight);
    dl->AddRect(ImVec2(wp.x + 1.0f, wp.y + 1.0f), ImVec2(wp.x + winW - 1.0f, wp.y + winH - 1.0f),
                g_ThemeBorder, 16.0f, 0, 1.2f);

    // Top status bar: colored indicator dots, title/version, and close dot just like the reference overlay.
    dl->AddCircleFilled(ImVec2(wp.x + 17.0f, wp.y + 18.0f), 6.0f, IM_COL32(246, 163, 27, 255));
    dl->AddCircleFilled(ImVec2(wp.x + 37.0f, wp.y + 18.0f), 6.0f, IM_COL32(232, 232, 216, 255));
    dl->AddCircleFilled(ImVec2(wp.x + 57.0f, wp.y + 18.0f), 6.0f, IM_COL32(221, 171, 58, 255));

    // Drag handle: only covers content (right of sidebar) so sidebar logo stays tappable.
    // Also stop before the close button area (last 44px).
    SetCursorScreenPos(ImVec2(wp.x + contentX, wp.y));
    InvisibleButton(O("##MenuDragHandle"), ImVec2(contentW - 44.0f, 40.0f));
    if (IsItemActive() && IsMouseDragging(ImGuiMouseButton_Left)) {
        g_menu.menuPos.x = ImClamp(g_menu.menuPos.x + GetIO().MouseDelta.x, 0.0f, ImMax(0.0f, GetIO().DisplaySize.x - winW));
        g_menu.menuPos.y = ImClamp(g_menu.menuPos.y + GetIO().MouseDelta.y, 0.0f, ImMax(0.0f, GetIO().DisplaySize.y - winH));
    }

    {
        // Gunakan plain char[] — O() macro returns pointer ke temporary yang langsung hancur
        // sehingga pointer jadi dangling dan teks tidak render sama sekali.
        char part1[32]  = "LYN8BP v3 | ARM64 | FPS ";
        char fpsText[16];
        snprintf(fpsText, sizeof(fpsText), "%.0f", GetIO().Framerate);
        ImVec2 part1Sz = CalcTextSize(part1);
        ImVec2 fpsSz   = CalcTextSize(fpsText);
        float statusX  = wp.x + contentX + (contentW - part1Sz.x - fpsSz.x) * 0.5f;
        dl->AddText(ImVec2(statusX,             wp.y + 11.0f), IM_COL32(245, 245, 235, 255), part1);
        dl->AddText(ImVec2(statusX + part1Sz.x, wp.y + 11.0f), IM_COL32(45, 236, 59, 255),  fpsText);

        // Baris ke-2: status game (state) + status lisensi singkat
        char stateLine[64];
        const char* stateStr = g_isInGame ? "In Game" : "Lobby";
        const char* licStr   = (g_LicenseStatusCode == LSC_OK)      ? "Active"
                             : (g_LicenseStatusCode == LSC_BANNED)   ? "Banned"
                             : (g_LicenseStatusCode == LSC_EXPIRED)  ? "Expired"
                             : (g_LicenseStatusCode == LSC_PENDING)  ? "Pending"
                             :                                          "No Key";
        snprintf(stateLine, sizeof(stateLine), "State: %s  |  Key: %s", stateStr, licStr);
        ImVec2 stateSz = CalcTextSize(stateLine);
        float stateX   = wp.x + contentX + (contentW - stateSz.x) * 0.5f;
        dl->AddText(ImVec2(stateX, wp.y + 29.0f), IM_COL32(160, 160, 155, 210), stateLine);
    }
    // Close button — larger 36x36 hit area for easier touch
    ImVec2 closeCenter(wp.x + winW - 22.0f, wp.y + 18.0f);
    dl->AddCircleFilled(closeCenter, 9.0f, IM_COL32(245, 245, 232, 255));
    SetCursorScreenPos(ImVec2(closeCenter.x - 18.0f, closeCenter.y - 18.0f));
    if (InvisibleButton(O("##CloseMenuTopDot"), ImVec2(36.0f, 36.0f))) g_menu.isOpen = false;

    SetCursorPos(ImVec2(contentX + pad, headerH));
    PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    BeginChild(O("##ContentArea"), ImVec2(contentW - pad * 1.45f, winH - headerH - 24.0f), false,
               ImGuiWindowFlags_NoScrollWithMouse);

    const float rowW = ImMax(220.0f, GetContentRegionAvail().x - 24.0f);
    switch (g_menu.currentTab) {
        case 0: {
            DrawSectionTitle(O("VIS"), O("VISUAL"), O("Overlay and prediction line settings"));
            need_save |= ToggleSwitch(O("Draw Lines"), &persistent_bool[O("bESP_DrawPredictionLine")]);
            need_save |= ToggleSwitch(O("Draw Pockets"), &persistent_bool[O("bESP_DrawPocketsShotState")]);
            need_save |= ToggleSwitch(O("Enemy Lines"), &persistent_bool[O("bESP_DrawEnemyLines")]);

            Dummy(ImVec2(0, 10));
            TextWrappedColored(ImVec4(0.93f, 0.70f, 0.25f, 1.0f), O("Line Thickness"));
            Dummy(ImVec2(0, 7));
            if (persistent_int[O("iLineThickness")] < 1) persistent_int[O("iLineThickness")] = 4;
            PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
            PushStyleVar(ImGuiStyleVar_GrabRounding, 8.0f);
            PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.10f, 0.09f, 1.0f));
            PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.86f, 0.61f, 0.12f, 1.0f));
            PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(1.0f, 0.74f, 0.20f, 1.0f));
            SetNextItemWidth(rowW);
            need_save |= SliderInt(O("##lineThick"), &persistent_int[O("iLineThickness")], 1, 10, "%d");
            PopStyleColor(3);
            PopStyleVar(2);
            break;
        }

        case 1: {
            DrawSectionTitle(O("AIM"), O("AUTOMATION"), O("Auto Play and aiming settings"));
            TextWrappedColored(ImVec4(0.93f, 0.70f, 0.25f, 1.0f), O("Automation"));
            Dummy(ImVec2(0, 5));
            need_save |= ToggleSwitch(O("Enable AutoPlay"), &persistent_bool[O("bAutoPlay")]);
            need_save |= ToggleSwitch(O("Enable AutoQueue"), &persistent_bool[O("bAutoQueue")]);

            Dummy(ImVec2(0, 14));
            Separator();
            Dummy(ImVec2(0, 8));
            TextWrappedColored(ImVec4(0.93f, 0.70f, 0.25f, 1.0f), O("Live Status"));
            TextWrappedColored(ImVec4(0.95f, 0.78f, 0.31f, 1.0f), O("State        : %s"), AutoPlay::bAutoPlaying ? O("Running") : O("Idle"));
            TextWrappedColored(ImVec4(0.95f, 0.78f, 0.31f, 1.0f), O("Speed        : %s"), AutoPlay::bAutoPlaying ? O("Fast") : O("Idle"));
            break;
        }

        case 2: {
            DrawSectionTitle(O("SET"), O("SETTINGS"), O("Appearance, menu, and config"));

            // ── THEME COLOR ──────────────────────────────────────────────────
            TextWrappedColored(ImVec4(0.93f, 0.70f, 0.25f, 1.0f), O("Theme Color"));
            Dummy(ImVec2(0, 8));
            {
                struct ThemeInfo { const char* name; ImVec4 swatch; };
                static const ThemeInfo themes[] = {
                    { "Gold",   ImVec4(0.906f,0.671f,0.165f,1.0f) },
                    { "Red",    ImVec4(0.863f,0.216f,0.216f,1.0f) },
                    { "Cyan",   ImVec4(0.098f,0.843f,0.765f,1.0f) },
                    { "Purple", ImVec4(0.608f,0.294f,0.863f,1.0f) },
                    { "Green",  ImVec4(0.176f,0.765f,0.314f,1.0f) },
                    { "Blue",   ImVec4(0.196f,0.510f,0.902f,1.0f) },
                };
                int& iTheme = persistent_int[O("iTheme")];
                const int numThemes = 6;
                const float swatchSz = 36.0f, swatchGap = 10.0f;
                float startX = GetCursorPosX();
                for (int ti = 0; ti < numThemes; ti++) {
                    bool isSel = (iTheme == ti);
                    PushID(ti + 100);
                    SetCursorPosX(startX + ti * (swatchSz + swatchGap));
                    PushStyleColor(ImGuiCol_Button,        themes[ti].swatch);
                    PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(
                        ImMin(themes[ti].swatch.x*1.2f,1.0f),
                        ImMin(themes[ti].swatch.y*1.2f,1.0f),
                        ImMin(themes[ti].swatch.z*1.2f,1.0f), 1.0f));
                    PushStyleColor(ImGuiCol_ButtonActive,  themes[ti].swatch);
                    PushStyleVar(ImGuiStyleVar_FrameRounding,   isSel ? 14.0f : 8.0f);
                    PushStyleVar(ImGuiStyleVar_FrameBorderSize, isSel ? 2.5f  : 0.0f);
                    PushStyleColor(ImGuiCol_Border, ImVec4(1,1,1,1));
                    if (Button(O("##swt"), ImVec2(swatchSz, swatchSz))) {
                        iTheme = ti; ApplyTheme(iTheme); need_save = true;
                    }
                    PopStyleColor(4);
                    PopStyleVar(2);
                    if (IsItemHovered()) SetTooltip("%s", themes[ti].name);
                    PopID();
                    if (ti < numThemes - 1) SameLine();
                }
                Dummy(ImVec2(0, 4));
                TextWrappedColored(ImVec4(0.75f,0.75f,0.75f,1.0f), O("Active: %s"),
                    (iTheme >= 0 && iTheme < numThemes) ? themes[iTheme].name : "Gold");
            }

            Dummy(ImVec2(0, 14));
            Separator();
            Dummy(ImVec2(0, 8));

            // ── ANIMASI EFEK ─────────────────────────────────────────────────
            TextWrappedColored(ImVec4(0.93f, 0.70f, 0.25f, 1.0f), O("Animasi Efek"));
            Dummy(ImVec2(0, 5));
            need_save |= ToggleSwitch(O("Shoot Animation"), &persistent_bool[O("bESP_ShootAnim")]);
            need_save |= ToggleSwitch(O("Rolling Animation"), &persistent_bool[O("bESP_RollingAnim")]);

            Dummy(ImVec2(0, 14));
            Separator();
            Dummy(ImVec2(0, 8));

            // ── MENU SCALE ───────────────────────────────────────────────────
            TextWrappedColored(ImVec4(0.93f, 0.70f, 0.25f, 1.0f), O("Menu Scale"));
            Dummy(ImVec2(0, 7));
            PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
            PushStyleVar(ImGuiStyleVar_GrabRounding, 8.0f);
            PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.10f, 0.09f, 1.0f));
            PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.86f, 0.61f, 0.12f, 1.0f));
            PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(1.0f, 0.74f, 0.20f, 1.0f));
            SetNextItemWidth(rowW);
            {
                int& menuSz = persistent_int[O("iMenuSizeOffset")];
                need_save |= SliderInt(O("##menuSize"), &menuSz, -10, 10, menuSz == 0 ? O("Normal") : "%d");
            }
            PopStyleColor(3);
            PopStyleVar(2);

            Dummy(ImVec2(0, 18));
            PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
            PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.48f, 0.10f, 1.0f));
            PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.64f, 0.16f, 1.0f));
            PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.54f, 0.36f, 0.07f, 1.0f));
            if (Button(O("Save Config"), ImVec2(rowW, 52.0f))) svConfig_Save();
            PopStyleColor(3);
            PopStyleVar();
            break;
        }

        case 3: {
            DrawSectionTitle(O("USR"), O("USER"), O("Device info, license, and status"));

            // ── DEVICE CARD ──────────────────────────────────────────────────
            {
                std::string manufacturer = AndroidProperty(O("ro.product.manufacturer"));
                std::string model        = AndroidProperty(O("ro.product.model"));
                std::string androidVer   = AndroidProperty(O("ro.build.version.release"));
                char realtime[32];
                FormatRealtime(realtime, sizeof(realtime));

                ImVec2 cPos = GetCursorScreenPos();
                const float cH = 82.0f;
                ImDrawList* cdl = GetWindowDrawList();
                cdl->AddRectFilled(cPos, ImVec2(cPos.x+rowW, cPos.y+cH), IM_COL32(18,19,17,220), 10.0f);
                cdl->AddRect(cPos, ImVec2(cPos.x+rowW, cPos.y+cH), g_ThemeBorder, 10.0f, 0, 1.0f);
                cdl->AddText(ImVec2(cPos.x+14, cPos.y+13), g_ThemeAccent, manufacturer.c_str());
                cdl->AddText(ImVec2(cPos.x+14, cPos.y+31), IM_COL32(220,215,200,255), model.c_str());
                char aInfo[80];
                snprintf(aInfo, sizeof(aInfo), "Android %s  |  %s", androidVer.c_str(), realtime);
                cdl->AddText(ImVec2(cPos.x+14, cPos.y+55), IM_COL32(125,125,120,215), aInfo);
                Dummy(ImVec2(rowW, cH));
            }

            Dummy(ImVec2(0, 10));

            // ── LICENSE CARD ─────────────────────────────────────────────────
            {
                const char* statusStr = "Unknown";
                ImU32 statusCol = IM_COL32(150,150,145,255);
                switch (g_LicenseStatusCode) {
                    case LSC_OK:          statusStr="Active";   statusCol=IM_COL32(50,230,100,255);  break;
                    case LSC_BANNED:      statusStr="Banned";   statusCol=IM_COL32(255,80,80,255);   break;
                    case LSC_EXPIRED:     statusStr="Expired";  statusCol=IM_COL32(255,155,25,255);  break;
                    case LSC_VERSION_OLD: statusStr="Old Ver";  statusCol=IM_COL32(130,160,255,255); break;
                    case LSC_PENDING:     statusStr="Pending";  statusCol=IM_COL32(230,215,50,255);  break;
                }
                bool isLifetime = (g_ExpTime.empty() || g_ExpTime == O("N/A") || g_ExpTime == O("Lifetime"));

                const std::string& rawKey = persistent_string[O("key")];
                std::string masked = rawKey.empty() ? "(none)" :
                    (rawKey.size() > 8 ? rawKey.substr(0,4) + "****" + rawKey.substr(rawKey.size()-4) : rawKey);
                const std::string& hwid = persistent_string[O("hwid")];
                std::string shortHwid   = hwid.empty() ? "(none)" :
                    (hwid.size() > 12 ? hwid.substr(0,6) + "..." + hwid.substr(hwid.size()-4) : hwid);

                // cH diperbesar agar setiap baris punya ruang cukup di layar HD
                const float cH = isLifetime ? 112.0f : 138.0f;
                ImVec2 cPos = GetCursorScreenPos();
                ImDrawList* cdl = GetWindowDrawList();
                cdl->AddRectFilled(cPos, ImVec2(cPos.x+rowW, cPos.y+cH), IM_COL32(17,18,16,220), 10.0f);
                cdl->AddRect(cPos, ImVec2(cPos.x+rowW, cPos.y+cH), g_ThemeBorder, 10.0f, 0, 1.0f);

                // ── Baris 1: Status badge (kanan atas, baris tersendiri) ────────
                float bW = CalcTextSize(statusStr).x + 20.0f;
                ImVec2 bMin(cPos.x+rowW-bW-10, cPos.y+10);
                ImVec2 bMax(cPos.x+rowW-10,    cPos.y+28);
                cdl->AddRectFilled(bMin, bMax, IM_COL32(20,20,18,200), 7.0f);
                cdl->AddRect(bMin, bMax, statusCol, 7.0f, 0, 1.5f);
                cdl->AddText(ImVec2(bMin.x+10, bMin.y+5), statusCol, statusStr);

                // ── Baris 2: Key (jarak cukup di bawah badge) ───────────────────
                cdl->AddText(ImVec2(cPos.x+14, cPos.y+38), IM_COL32(155,150,140,220), "Key");
                cdl->AddText(ImVec2(cPos.x+52, cPos.y+38), g_ThemeAccent, masked.c_str());

                // ── Baris 3: Expiry ──────────────────────────────────────────────
                if (isLifetime) {
                    cdl->AddText(ImVec2(cPos.x+14, cPos.y+62), IM_COL32(155,150,140,220), "Expires");
                    cdl->AddText(ImVec2(cPos.x+82, cPos.y+62), IM_COL32(50,220,100,255), "Lifetime");
                } else {
                    char expBuf[64];
                    snprintf(expBuf, sizeof(expBuf), "%s", g_ExpTime.c_str());
                    cdl->AddText(ImVec2(cPos.x+14, cPos.y+62), IM_COL32(155,150,140,220), "Expires");
                    cdl->AddText(ImVec2(cPos.x+82, cPos.y+62), IM_COL32(200,195,180,255), expBuf);
                    // ── Baris 4: Countdown ────────────────────────────────────────
                    if (g_ExpiryTimestamp > 0) {
                        int64_t secsLeft = (int64_t)g_ExpiryTimestamp - (int64_t)time(nullptr);
                        char remBuf[48];
                        if (secsLeft > 0) {
                            snprintf(remBuf, sizeof(remBuf), "%dd %dh %02dm %02ds",
                                     (int)(secsLeft/86400),
                                     (int)((secsLeft%86400)/3600),
                                     (int)((secsLeft%3600)/60),
                                     (int)(secsLeft%60));
                            cdl->AddText(ImVec2(cPos.x+14, cPos.y+88), IM_COL32(50,215,95,255), remBuf);
                        } else {
                            cdl->AddText(ImVec2(cPos.x+14, cPos.y+88), IM_COL32(255,75,75,255), "Expired!");
                        }
                    }
                }

                // ── Baris 5: HWID (paling bawah) ────────────────────────────────
                char hwidLine[64];
                snprintf(hwidLine, sizeof(hwidLine), "HWID  %s", shortHwid.c_str());
                cdl->AddText(ImVec2(cPos.x+14, cPos.y+cH-18), IM_COL32(100,100,95,195), hwidLine);

                Dummy(ImVec2(rowW, cH));
            }

            Dummy(ImVec2(0, 10));

            // ── FEATURES ─────────────────────────────────────────────────────
            if (g_Features.empty()) {
                TextColored(ImVec4(0.3f,0.9f,0.5f,1.0f), "  + All Access (Semua Fitur Aktif)");
            } else {
                TextWrappedColored(ImVec4(0.93f, 0.70f, 0.25f, 1.0f), O("Features"));
                Dummy(ImVec2(0, 6));
                PushStyleColor(ImGuiCol_TableBorderLight, IM_COL32(80,65,30,120));
                if (BeginTable("##featTable", 2,
                               ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame,
                               ImVec2(rowW, 0))) {
                    for (const auto& feat : g_Features) {
                        TableNextColumn();
                        Dummy(ImVec2(0, 2));
                        TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.0f), "+ %s", feat.c_str());
                        Dummy(ImVec2(0, 2));
                    }
                    EndTable();
                }
                PopStyleColor();
            }

            Dummy(ImVec2(0, 10));
            Separator();
            Dummy(ImVec2(0, 6));
            TextWrappedColored(ImVec4(0.35f, 1.0f, 0.23f, 1.0f), O("LYN8BP  —  lyn8bp.vercel.app"));
            break;
        }
    }

    if (need_save) save_persistence();

    EndChild();
    PopStyleColor();
}


INLINE void DrawMenu(ImGuiIO& io) {
    if ((!g_Token.empty() && !g_Auth.empty() && g_Token == g_Auth) || DEBUG_BYPASS_LOGIN) {
        if (is_segv_handler_active()) {
            jump_buffer_active = 1;
            if (!sigsetjmp(jump_buffer, 1)) DrawESP(GetBackgroundDrawList());
            jump_buffer_active = 0;
        }

        if (g_menu.isOpen) {
            g_menu.menuAlpha += (1.0f - g_menu.menuAlpha) * io.DeltaTime * 12.0f;
        } else {
            g_menu.menuAlpha = 0.0f;
        }

        if (g_menu.menuAlpha > 0.01f) {
            ApplyTheme(persistent_int[O("iTheme")]);
            float sizeScale = 1.0f + (float)persistent_int[O("iMenuSizeOffset")] * 0.03f;
            if (sizeScale < 0.3f) sizeScale = 0.3f;
            float winW = g_menu.sidebarWidth * sizeScale;
            float winH = 470.0f * sizeScale;

            if (!g_menu.menuPosInitialized) {
                g_menu.menuPos = ImVec2((Width - winW) * 0.5f, (Height - winH) * 0.5f);
                g_menu.menuPosInitialized = true;
            }
            g_menu.menuPos.x = ImClamp(g_menu.menuPos.x, 0.0f, ImMax(0.0f, io.DisplaySize.x - winW));
            g_menu.menuPos.y = ImClamp(g_menu.menuPos.y, 0.0f, ImMax(0.0f, io.DisplaySize.y - winH));
            winH = 450.0f * sizeScale;

            SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_Always);
            SetNextWindowPos(g_menu.menuPos, ImGuiCond_Always);

            PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.13f, 0.f));
            PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
            PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            PushStyleVar(ImGuiStyleVar_Alpha, g_menu.menuAlpha);

            ImGuiWindowFlags winFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                        ImGuiWindowFlags_NoResize;

            if (Begin(O("##MainMenu"), &g_menu.isOpen, winFlags)) {
                const float sidebarW = 170.0f * sizeScale;
                // DrawContentArea dulu (backgrounds + header + content).
                // DrawSidebar setelahnya agar sidebar tampil di atas background.
                // Klik tab sidebar sekarang pakai direct hit test (io.MouseClicked[0])
                // sehingga clip rect dari BeginChild tidak memblokir tab mana pun.
                DrawContentArea(winW, winH, sidebarW);
                DrawSidebar(winH, sidebarW);
            }
            End();

            PopStyleVar(4);
            PopStyleColor();
        }
    }
}

static void DrawToggleButton(bool cancelMode) {
    if (g_menu.isOpen) return;

    ImGuiIO& io = GetIO();
    static GLuint play_on_tex  = LoadTextureFromMemory(play_on_png,  play_on_png_len);
    static GLuint play_off_tex = LoadTextureFromMemory(play_off_png, play_off_png_len);

    float button_size  = 130.f;
    float windowWidth  = button_size + GetStyle().WindowPadding.x * 2.0f;
    float windowHeight = button_size + GetStyle().WindowPadding.y * 2.0f;
    const float rightMargin = 20.0f;

    if (g_sideBtnsY <= 0.0f) g_sideBtnsY = io.DisplaySize.y - 150.0f;

    float fixedX = io.DisplaySize.x - rightMargin - windowWidth;

    SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_Always);
    SetNextWindowPos(ImVec2(fixedX, g_sideBtnsY), ImGuiCond_Always);

    PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    PushStyleColor(ImGuiCol_Border,   IM_COL32(0, 0, 0, 0));
    PushStyleVar(ImGuiStyleVar_WindowRounding, 99.0f);

    if (Begin(O("##ToggleBtn"), nullptr,
              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {

        ImVec2 pos = GetCursorScreenPos();
        ImVec2 size(button_size, button_size);
        ImVec2 center(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);

        if (InvisibleButton(O("##TglBtnHit"), size)) {
            if (cancelMode) {
                persistent_bool[O("bAutoQueue")] = false;
                g_aqCounting = false;
            } else {
                AutoPlay::bAutoPlaying = !AutoPlay::bAutoPlaying;
                if (AutoPlay::bAutoPlaying) AutoPlay::ClearState();
            }
        }

        GLuint tex = cancelMode ? play_on_tex :
                     (AutoPlay::bAutoPlaying ? play_on_tex : play_off_tex);

        float r = size.x * 0.5f;
        ImDrawList* dl = GetWindowDrawList();
        dl->AddImage((void*)(intptr_t)tex, ImVec2(center.x - r, center.y - r), ImVec2(center.x + r, center.y + r));
    }
    End();
    PopStyleVar();
    PopStyleColor(2);
}

static void DrawFloatingButton(ImGuiIO& io) {
    if (g_menu.isOpen) return;

    float btnR    = 65.0f;
    float winSize = (btnR * 2.0f) + 10.0f;
    const float rightMargin = 20.0f;

    if (g_sideBtnsY <= 0.0f) g_sideBtnsY = io.DisplaySize.y - 150.0f;

    float toggleWidth = 130.f + (GetStyle().WindowPadding.x * 2.0f);
    float fixedX = io.DisplaySize.x - rightMargin - toggleWidth + (toggleWidth - winSize) * 0.5f;
    float posY   = g_sideBtnsY - 140.0f;

    SetNextWindowPos(ImVec2(fixedX, posY), ImGuiCond_Always);
    SetNextWindowSize(ImVec2(winSize, winSize), ImGuiCond_Always);

    PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    if (Begin(O("##FloatBtn"), nullptr,
              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
              ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {

        static GLuint logo_tex = LoadTextureFromMemory(logo_png, logo_png_len);

        ImDrawList* dl = GetWindowDrawList();
        ImVec2 center  = ImVec2(fixedX + (winSize * 0.5f), posY + (winSize * 0.5f));

        InvisibleButton(O("##FloatBtnHit"), ImVec2(winSize, winSize));

        if (IsItemActive() && IsMouseDragging(ImGuiMouseButton_Left)) {
            g_sideBtnsY += io.MouseDelta.y;
            g_sideBtnsY = ImClamp(g_sideBtnsY, 160.0f, io.DisplaySize.y - 150.0f);
        }

        if (IsItemHovered() && IsMouseReleased(0) && ImGui::GetMouseDragDelta(0).y == 0) {
            g_menu.isOpen = true;
        }

        dl->AddImage((void*)(intptr_t)logo_tex,
                     ImVec2(center.x - btnR, center.y - btnR),
                     ImVec2(center.x + btnR, center.y + btnR));
    }
    End();
    PopStyleVar(2);
    PopStyleColor();
}

static void DrawLicenseStatusOverlay(ImGuiIO& io) {
    if (g_LicenseStatusCode == LSC_OK || g_LicenseStatusCode == LSC_PENDING) return;

    const char* lbl;
    ImU32 col, bgCol, brdCol;
    switch (g_LicenseStatusCode) {
        case LSC_BANNED:
            lbl = "LICENSE BANNED";
            col = IM_COL32(255,75,75,255); bgCol = IM_COL32(28,6,6,225); brdCol = IM_COL32(190,40,40,150); break;
        case LSC_EXPIRED:
            lbl = "LICENSE EXPIRED";
            col = IM_COL32(255,168,38,255); bgCol = IM_COL32(26,15,3,225); brdCol = IM_COL32(185,115,18,150); break;
        default:
            lbl = "UPDATE REQUIRED";
            col = IM_COL32(110,155,255,255); bgCol = IM_COL32(8,12,28,225); brdCol = IM_COL32(75,100,200,150); break;
    }

    float lineH  = GetFontSize();
    bool  hasSub = !g_LicenseStatusMsg.empty();
    float boxW   = 230.0f;
    float boxH   = 12.0f + lineH + (hasSub ? lineH + 5.0f : 0.0f) + 12.0f;
    float bx     = io.DisplaySize.x - 18.0f - boxW;
    float by     = 18.0f;

    ImDrawList* dl = GetBackgroundDrawList();
    dl->AddRectFilled({bx,by},{bx+boxW,by+boxH}, bgCol, 10.0f);
    dl->AddRect      ({bx,by},{bx+boxW,by+boxH}, brdCol, 10.0f, 0, 1.2f);
    dl->AddRectFilled({bx,by+4},{bx+3,by+boxH-4}, col, 2.0f);
    dl->AddText({bx+11.0f, by+12.0f}, col, lbl);
    if (hasSub)
        dl->AddText({bx+11.0f, by+12.0f+lineH+5.0f},
                    IM_COL32(155,155,168,200), g_LicenseStatusMsg.c_str());
}

static void DrawLicenseBlockScreen(ImGuiIO& io) {
    SetNextWindowPos(ImVec2(0,0));
    SetNextWindowSize(io.DisplaySize);
    PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f,0.02f,0.04f,0.95f));
    Begin(O("##BlkOvl"), nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus);
    PopStyleColor();

    const char* mainTxt; const char* subTxt;
    ImU32 accentU; ImVec4 titleC;
    switch (g_LicenseStatusCode) {
        case LSC_BANNED:
            mainTxt = O("LICENSE BANNED"); subTxt = O("Your license has been banned. Contact the admin.");
            accentU = IM_COL32(200,38,38,255); titleC = ImVec4(1.0f,0.18f,0.18f,1.0f); break;
        case LSC_EXPIRED:
            mainTxt = O("LICENSE EXPIRED"); subTxt = O("Your license has expired. Please renew to continue.");
            accentU = IM_COL32(200,128,18,255); titleC = ImVec4(1.0f,0.62f,0.08f,1.0f); break;
        default:
            mainTxt = O("UPDATE REQUIRED"); subTxt = O("A newer version is required. Please update the mod.");
            accentU = IM_COL32(75,100,210,255); titleC = ImVec4(0.48f,0.64f,1.0f,1.0f); break;
    }

    float winW = 500.0f;
    SetNextWindowPos(ImVec2(io.DisplaySize.x*0.5f, io.DisplaySize.y*0.5f),
                     ImGuiCond_Always, ImVec2(0.5f,0.5f));
    SetNextWindowSize(ImVec2(winW,0), ImGuiCond_Always);
    PushStyleColor(ImGuiCol_WindowBg, IM_COL32(14,16,26,255));
    PushStyleVar(ImGuiStyleVar_WindowRounding,   20.0f);
    PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(32.0f,28.0f));
    PushStyleVar(ImGuiStyleVar_WindowBorderSize,  0.0f);
    if (Begin(O("##BlkCard"), nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImDrawList* d2 = GetWindowDrawList();
        ImVec2 wp2 = GetWindowPos(), ws2 = GetWindowSize();
        d2->AddRectFilled(wp2, {wp2.x+ws2.x,wp2.y+3}, accentU, 20.0f, ImDrawFlags_RoundCornersTop);
        d2->AddRectFilled(wp2, {wp2.x+ws2.x,wp2.y+22}, accentU, 20.0f, ImDrawFlags_RoundCornersTop);
        d2->AddRect(wp2, {wp2.x+ws2.x,wp2.y+ws2.y}, accentU, 20.0f, 0, 1.2f);

        Dummy(ImVec2(0,8));
        SetWindowFontScale(1.5f);
        ImVec2 ts = CalcTextSize(mainTxt);
        SetCursorPosX((ws2.x-64.0f-ts.x)*0.5f);
        TextColored(titleC, "%s", mainTxt);
        SetWindowFontScale(1.0f);
        Dummy(ImVec2(0,8));
        PushTextWrapPos(GetCursorPosX()+ws2.x-64.0f);
        TextColored(ImVec4(0.76f,0.76f,0.82f,1.0f), "%s", subTxt);
        if (!g_LicenseStatusMsg.empty()) {
            Dummy(ImVec2(0,5));
            TextColored(ImVec4(0.52f,0.52f,0.60f,1.0f), "%s", g_LicenseStatusMsg.c_str());
        }
        PopTextWrapPos();
        if (!g_ExpTime.empty() && g_ExpTime != "N/A") {
            Dummy(ImVec2(0,8));
            std::string expLine = std::string(O("Expiry: ")) + g_ExpTime;
            TextColored(ImVec4(0.42f,0.42f,0.50f,1.0f), "%s", expLine.c_str());
        }
        Dummy(ImVec2(0,6));
    }
    End();
    PopStyleVar(3); PopStyleColor();
    End();
}

static bool first_time = true;
INLINE void DrawLogin(ImGuiIO& io) {
    if (logged_in) return DrawMenu(io);

    SetNextWindowPos(ImVec2(0, 0));
    SetNextWindowSize(io.DisplaySize);
    PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.05f, 0.97f));
    Begin(O("##Overlay"), nullptr,
          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus);
    {
        ImDrawList* bgdl = GetWindowDrawList();
        float sp = 44.0f;
        for (float x = 0; x < io.DisplaySize.x; x += sp)
            for (float y = 0; y < io.DisplaySize.y; y += sp)
                bgdl->AddCircleFilled(ImVec2(x, y), 1.2f, IM_COL32(40, 55, 80, 50));
    }
    PopStyleColor();

    float cardW = 510.0f;
    const float cardH = 430.0f;

    if (!g_licenseKeyInputInitialized) {
        const std::string savedKey = persistent_string[O("key")];
        if (!savedKey.empty()) {
            std::snprintf(g_licenseKeyInput, sizeof(g_licenseKeyInput), "%s", savedKey.c_str());
        }
        g_licenseKeyInputInitialized = true;
    }

    SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                     ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    SetNextWindowSize(ImVec2(cardW, 0), ImGuiCond_Always);

    PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.08f, 0.13f, 1.0f));
    PushStyleVar(ImGuiStyleVar_WindowRounding, 22.0f);
    PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    Begin(O("##LoginCard"), nullptr,
          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize);

    ImDrawList* dl = GetWindowDrawList();
    ImVec2 wp = GetWindowPos();

    dl->AddRectFilled(ImVec2(wp.x-2,wp.y-2), ImVec2(wp.x+cardW+2,wp.y+cardH),
                      IM_COL32(0,200,180,14), 24.0f);
    dl->AddRect(wp, ImVec2(wp.x+cardW,wp.y+cardH),
                IM_COL32(0,200,180,80), 22.0f, 0, 1.5f);
    DrawGradientRect(dl, wp, ImVec2(wp.x+cardW,wp.y+3),
                     IM_COL32(0,200,180,255), IM_COL32(80,80,230,255), true);
    dl->AddRectFilled(wp, ImVec2(wp.x+cardW,wp.y+22),
                      IM_COL32(0,200,180,38), 22.0f, ImDrawFlags_RoundCornersTop);

    float iconCX = wp.x + cardW * 0.5f;
    float iconCY = wp.y + 72.0f;
    dl->AddCircleFilled(ImVec2(iconCX,iconCY), 42.0f, IM_COL32(0,200,180,10));
    dl->AddCircleFilled(ImVec2(iconCX,iconCY), 36.0f, IM_COL32(10,14,22,255));
    dl->AddCircle    (ImVec2(iconCX,iconCY), 36.0f, IM_COL32(0,200,180,135), 0, 2.0f);
    float kHX = iconCX - 9.0f, kHY = iconCY - 1.0f;
    dl->AddCircle(ImVec2(kHX,kHY), 10.0f, IM_COL32(0,225,205,215), 0, 2.2f);
    dl->AddLine(ImVec2(kHX+10.0f,kHY), ImVec2(kHX+29.0f,kHY), IM_COL32(0,225,205,215), 2.2f);
    dl->AddLine(ImVec2(kHX+21.0f,kHY), ImVec2(kHX+21.0f,kHY+7.0f), IM_COL32(0,225,205,215), 2.2f);
    dl->AddLine(ImVec2(kHX+29.0f,kHY), ImVec2(kHX+29.0f,kHY+5.0f), IM_COL32(0,225,205,215), 2.2f);

    dl->AddLine(ImVec2(wp.x+50.0f,wp.y+116.0f),
                ImVec2(wp.x+cardW-50.0f,wp.y+116.0f),
                IM_COL32(30,50,75,150), 1.0f);

    Dummy(ImVec2(cardW, 128.0f));

    if (!ERROR_MESSAGE.empty()) {
        SetCursorPosX(28.0f);
        ImVec2 ep = GetCursorScreenPos();
        float  ew = cardW - 56.0f;
        dl->AddRectFilled(ep, ImVec2(ep.x+ew,ep.y+44.0f), IM_COL32(40,8,8,210), 8.0f);
        dl->AddRect      (ep, ImVec2(ep.x+ew,ep.y+44.0f), IM_COL32(190,40,40,145), 8.0f, 0, 1.0f);
        SetCursorPosX(36.0f);
        PushTextWrapPos(wp.x+cardW-36.0f);
        TextColored(ImVec4(1.0f,0.35f,0.35f,1.0f), "%s", ERROR_MESSAGE.c_str());
        PopTextWrapPos();
        Dummy(ImVec2(0, 10.0f));
    }

    if (is_logging_in) {
        Dummy(ImVec2(0, 18.0f));
        static float sa = 0.0f;
        ImGuiIO& io2 = GetIO();
        sa += io2.DeltaTime * 4.5f;
        ImVec2 sc = {wp.x + cardW*0.5f, wp.y + (ERROR_MESSAGE.empty() ? 200.0f : 230.0f)};
        for (int i = 0; i < 12; i++) {
            float a = sa + i*IM_PI*2.0f/12.0f, al = (float)(12-i)/12.0f;
            dl->AddCircleFilled({sc.x+cosf(a)*25.0f,sc.y+sinf(a)*25.0f},
                                4.5f, IM_COL32(0,220,200,(int)(al*235)));
        }
        Dummy(ImVec2(0, 55.0f));
        const char* authStr = O("Authenticating...");
        SetCursorPosX((cardW-CalcTextSize(authStr).x)*0.5f);
        TextColored(ImVec4(0.0f,0.78f,0.68f,0.80f), O("Authenticating..."));
        Dummy(ImVec2(0, 28.0f));
    } else {
        const char* ins = O("Input your license key, then tap LOGIN");
        SetCursorPosX((cardW-CalcTextSize(ins).x)*0.5f);
        TextColored(ImVec4(0.43f,0.50f,0.62f,1.0f), O("Input your license key, then tap LOGIN"));

        Dummy(ImVec2(0, 18.0f));

        SetCursorPosX(36.0f);
        TextColored(ImVec4(0.58f,0.66f,0.78f,1.0f), O("LICENSE KEY"));
        Dummy(ImVec2(0, 6.0f));

        PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.035f,0.05f,0.08f,1.0f));
        PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.05f,0.08f,0.12f,1.0f));
        PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0.06f,0.10f,0.15f,1.0f));
        PushStyleColor(ImGuiCol_Border,         ImVec4(0.0f,0.72f,0.62f,0.50f));
        PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
        PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.2f);
        SetCursorPosX(36.0f);
        SetNextItemWidth(cardW - 72.0f);
        InputText(O("##LicenseKeyInput"), g_licenseKeyInput, sizeof(g_licenseKeyInput),
                  ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_AutoSelectAll);
        PopStyleVar(2);
        PopStyleColor(4);

        Dummy(ImVec2(0, 12.0f));

        bool AutoLogin = first_time && !persistent_string[O("key")].empty();

        SetCursorPosX(36.0f);
        PushStyleColor(ImGuiCol_Button,        ImVec4(0.04f,0.08f,0.12f,1.0f));
        PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.06f,0.14f,0.18f,1.0f));
        PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.03f,0.10f,0.13f,1.0f));
        PushStyleColor(ImGuiCol_Text,          ImVec4(0.60f,0.88f,0.84f,1.0f));
        PushStyleColor(ImGuiCol_Border,        ImVec4(0.0f,0.72f,0.62f,0.38f));
        PushStyleVar(ImGuiStyleVar_FrameRounding,   12.0f);
        PushStyleVar(ImGuiStyleVar_FrameBorderSize,  1.0f);
        bool pasteClicked = Button(O("PASTE FROM CLIPBOARD"), ImVec2(cardW-72.0f, 46.0f));

        if (pasteClicked) {
            JNIEnv* env;
            jint r = VM->GetEnv((void**)&env, JNI_VERSION_1_6);
            if (r == JNI_EDETACHED) {
                if (VM->AttachCurrentThread(&env, nullptr) != 0) {
                    ERROR_MESSAGE = O("Failed to attach thread to JVM");
                }
            } else if (r != JNI_OK) {
                ERROR_MESSAGE = O("Failed to get JNIEnv");
            }
            if (r == JNI_OK || r == JNI_EDETACHED) {
                const std::string pasted = NormalizeLicenseKey(getClipboard(env));
                std::snprintf(g_licenseKeyInput, sizeof(g_licenseKeyInput), "%s", pasted.c_str());
            }
        }

        Dummy(ImVec2(0, 12.0f));
        SetCursorPosX(36.0f);
        SetWindowFontScale(1.25f);
        bool clicked = Button(O("LOGIN"), ImVec2(cardW-72.0f, 62.0f));
        SetWindowFontScale(1.0f);

        if (AutoLogin || clicked) {
            if (DEBUG_BYPASS_LOGIN) {
                logged_in = true;
                g_menu.isOpen = true;
            } else {
                JNIEnv* env;
                jint r = VM->GetEnv((void**)&env, JNI_VERSION_1_6);
                if (r == JNI_EDETACHED) {
                    if (VM->AttachCurrentThread(&env, nullptr) != 0)
                        ERROR_MESSAGE = O("Failed to attach thread to JVM");
                } else if (r != JNI_OK) {
                    ERROR_MESSAGE = O("Failed to get JNIEnv");
                } else {
                    std::string keyToLogin = AutoLogin ? persistent_string[O("key")] : NormalizeLicenseKey(g_licenseKeyInput);
                    if (keyToLogin.empty()) keyToLogin = NormalizeLicenseKey(getClipboard(env));
                    std::snprintf(g_licenseKeyInput, sizeof(g_licenseKeyInput), "%s", keyToLogin.c_str());
                    std::thread([](std::string aid, std::string k) {
                        Login(aid, k);
                    }, getAndroidID(env), keyToLogin).detach();
                }
                first_time = false;
            }
        }
        PopStyleVar(2);
        PopStyleColor(5);
        Dummy(ImVec2(0, 24.0f));
    }

    End();
    PopStyleVar(3);
    PopStyleColor();
    End();
}

INLINE void SetupImgui() {
    PACKAGE_NAME = string(getcmdline());

    ImGui::CreateContext();

    auto& style = ImGui::GetStyle();
    auto& io    = ImGui::GetIO();

    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;

    switch_theme(current_theme);

    load_persistence();
    svConfig_Load();
    load_imgui_style();

    static string INI_PATH = O("/data/user_de/0/") + PACKAGE_NAME + O("/no_backup/.ini");
    io.IniFilename = persistent_bool["bImguiAutoSave"] ? INI_PATH.c_str() : nullptr;
    io.ConfigWindowsMoveFromTitleBarOnly = persistent_bool["bMoveOnlyWithTitleBar"];

    ImFontConfig font_cfg;
    font_cfg.SizePixels = persistent_float["fFontScale"];
    io.Fonts->AddFontDefault(&font_cfg);

    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init(O("#version 300 es"));

    bImguiSetup = true;
}

DEFINES(EGLBoolean, Draw, EGLDisplay dpy, EGLSurface surface) {
    eglQuerySurface(dpy, surface, EGL_WIDTH, &Width);
    eglQuerySurface(dpy, surface, EGL_HEIGHT, &Height);

    if (Width <= 0 || Height <= 0) return _Draw(dpy, surface);

    screenCenter = Vector2(Width / 2, Height / 2);

    if (!bImguiSetup) SetupImgui();

    ImGuiIO& io = ImGui::GetIO();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(Width, Height);
    ImGui::NewFrame();

    if (!is_segv_handler_active()) setup_global_segv_handler();
    if (IsExpired()) {
        DrawExpired(io);
    } else if ((!g_Token.empty() && !g_Auth.empty() && g_Token == g_Auth) || DEBUG_BYPASS_LOGIN) {
        {
            if (logged_in && g_ExpiryTimestamp > 0 && g_LicenseStatusCode == LSC_OK) {
                if ((int64_t)time(nullptr) >= g_ExpiryTimestamp) {
                    g_LicenseStatusCode = LSC_EXPIRED;
                    g_LicenseStatusMsg  = OO("License has expired").str();
                }
            }
            static time_t s_lastCheck = 0;
            time_t nowT = time(nullptr);
            if (logged_in && (nowT - s_lastCheck) > 1 && !g_StatusChecking) {
                RefreshLicenseStatus();
                s_lastCheck = nowT;
            }
        }
        if (g_LicenseStatusCode == LSC_BANNED || g_LicenseStatusCode == LSC_EXPIRED || g_LicenseStatusCode == LSC_VERSION_OLD) {
            DrawLicenseBlockScreen(io);
            DrawLicenseStatusOverlay(io);
        } else {
            static int s_notInGameFrames = 0;
            if (!g_isInGame) { s_notInGameFrames++; if (s_notInGameFrames > 8) g_menu.isOpen = false; }
            else             { s_notInGameFrames = 0; }
            if (g_isInGame) DrawFloatingButton(io);
            DrawMenu(io);

            {
                SetNextWindowPos(ImVec2(Width * 0.5f, Height - 60.0f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
                Begin(O("##PoweredBy"), nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize |
                      ImGuiWindowFlags_NoInputs);
                TextColored(ImColor(0, 255, 0, 255), O("Owner By @LYN4XP"));
                End();
            }

            if (g_autoPlayCalculating) DrawCalculating(io);
            DrawLicenseStatusOverlay(io);
        }
    } else {
        DrawLogin(io);
    }
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    ImGui_ClearHoverEffect();

    return _Draw(dpy, surface);
}

void __IMGUI__() {
    create_directory_recursive(CONC(O("/data/user_de/0/"), PACKAGE_NAME.c_str(), O("/no_backup")));
}

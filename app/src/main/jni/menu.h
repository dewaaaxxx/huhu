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
#include <Vector/Vectors.h>
#include <imgui/imgui.h>
#include "icons/icons.h"

using namespace ImGui;
using namespace std;

struct MenuState {
    bool isOpen      = false;
    bool isMinimized = false;   // collapses to header-only bar
    int currentTab = 0;
    float sidebarWidth = 520.0f;
    float animProgress = 0.0f;
    float menuAlpha = 0.0f;
    float menuScale = 0.9f;
    ImVec4 accentColor = ImVec4(0.35f, 0.65f, 0.95f, 1.0f);
};
static MenuState g_menu;
static bool g_isInGame = false; // set each frame by DrawESP; gates FloatingButton + Menu

// ── THEME SYSTEM ──────────────────────────────────────────────
struct MenuTheme {
    ImU32 bgMain;       // window background
    ImU32 bgHeader;     // header bar bg
    ImU32 bgContent;    // content area bg
    ImU32 accent;       // slider grab, toggle on, selected tab bg
    ImU32 accentHov;    // hovered accent
    ImU32 xBtn;         // X close button
    ImU32 xBtnHov;
    ImU32 separator;    // separator lines
    ImU32 textPrimary;
    ImU32 textSecondary;
    ImU32 sectionLabel; // section header text
};

static const MenuTheme THEMES[3] = {
    // 0 — Dark Red (default)
    {
        IM_COL32(21,21,21,255), IM_COL32(16,16,20,255), IM_COL32(21,21,21,255),
        IM_COL32(200,30,30,255), IM_COL32(230,50,50,255),
        IM_COL32(160,40,40,210), IM_COL32(220,40,40,255),
        IM_COL32(50,50,65,160), IM_COL32(230,230,240,255), IM_COL32(140,140,150,255),
        IM_COL32(158,158,178,255)
    },
    // 1 — Dark Glass (blue-gray glass)
    {
        IM_COL32(18,22,32,240), IM_COL32(14,18,28,250), IM_COL32(18,22,32,240),
        IM_COL32(60,140,220,255), IM_COL32(90,165,245,255),
        IM_COL32(40,100,180,210), IM_COL32(70,140,230,255),
        IM_COL32(60,80,120,160), IM_COL32(220,230,245,255), IM_COL32(130,150,180,255),
        IM_COL32(120,150,200,255)
    },
    // 2 — Dark Neon (cyan/green neon)
    {
        IM_COL32(12,18,18,255), IM_COL32(8,14,14,255), IM_COL32(12,18,18,255),
        IM_COL32(0,220,180,255), IM_COL32(30,255,210,255),
        IM_COL32(0,160,130,210), IM_COL32(0,220,180,255),
        IM_COL32(0,80,70,160), IM_COL32(210,255,245,255), IM_COL32(100,180,160,255),
        IM_COL32(80,200,170,255)
    },
};

static inline const MenuTheme& GetTheme() {
    int idx = persistent_int["iTheme"];
    if (idx < 0 || idx > 2) idx = 2;   // default: Dark Neon
    return THEMES[idx];
}

// g_ExpiryTimestamp (from keylogin.h) replaces hardcoded EXPIRY_TS

static bool DEBUG_BYPASS_LOGIN = false;

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

static bool SidebarButton(const char* label, GLuint iconTex, bool selected,
                           float width, float btnH = 0.0f) {
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);

    // Icon scales with button height — fills ~60% of button, min 36, max 48
    float iconSize = btnH > 0.0f ? ImClamp(btnH * 0.60f, 36.0f, 48.0f) : 40.0f;
    if (btnH <= 0.0f) btnH = iconSize + 20.0f;

    ImVec2 pos  = window->DC.CursorPos;
    ImVec2 size = ImVec2(width, btnH);

    const ImRect bb(pos, pos + size);
    ItemSize(size, style.FramePadding.y);
    if (!ItemAdd(bb, id)) return false;

    bool hovered, held;
    bool pressed = ButtonBehavior(bb, id, &hovered, &held);

    ImDrawList* dl = window->DrawList;
    const MenuTheme& T = GetTheme();

    float  vPad_      = (btnH - iconSize) * 0.5f;
    ImVec2 iconCenter = ImVec2(bb.Min.x + width * 0.5f, bb.Min.y + vPad_ + iconSize * 0.5f);
    float  iconBgSize = iconSize + 12.0f;

    // Hover highlight
    if (hovered && !selected)
        dl->AddRectFilled(bb.Min, bb.Max, IM_COL32(255,255,255,12), 10.0f);

    // Selected: accent rounded rect
    if (selected) {
        dl->AddRectFilled(
            ImVec2(iconCenter.x - iconBgSize * 0.5f, iconCenter.y - iconBgSize * 0.5f),
            ImVec2(iconCenter.x + iconBgSize * 0.5f, iconCenter.y + iconBgSize * 0.5f),
            T.accent, 12.0f);
    }

    // Draw icon texture — or gear shape if iconTex == 0
    if (iconTex) {
        ImVec2 iconMin = ImVec2(iconCenter.x - iconSize*0.5f, iconCenter.y - iconSize*0.5f);
        ImVec2 iconMax = ImVec2(iconCenter.x + iconSize*0.5f, iconCenter.y + iconSize*0.5f);
        dl->AddImage((void*)(intptr_t)iconTex, iconMin, iconMax, ImVec2(0,0), ImVec2(1,1));
    } else {
        // Gear icon drawn with primitives (for Settings tab)
        float  gr  = iconSize * 0.38f;  // outer radius
        float  ir  = iconSize * 0.20f;  // inner radius
        float  tr  = iconSize * 0.10f;  // tooth radius
        int    tc  = 8;                 // teeth count
        ImU32  col = selected ? IM_COL32(255,255,255,255) : IM_COL32(160,160,175,255);
        dl->AddCircle(iconCenter, ir, col, 0, 2.5f);
        for (int i = 0; i < tc; i++) {
            float a0 = (float)i / tc * IM_PI * 2.0f;
            float a1 = a0 + IM_PI / tc * 0.65f;
            ImVec2 p0(iconCenter.x + cosf(a0)*gr, iconCenter.y + sinf(a0)*gr);
            ImVec2 p1(iconCenter.x + cosf(a1)*gr, iconCenter.y + sinf(a1)*gr);
            ImVec2 p2(iconCenter.x + cosf(a1)*(gr+tr), iconCenter.y + sinf(a1)*(gr+tr));
            ImVec2 p3(iconCenter.x + cosf(a0)*(gr+tr), iconCenter.y + sinf(a0)*(gr+tr));
            dl->AddQuadFilled(p0, p1, p2, p3, col);
            dl->AddLine(ImVec2(iconCenter.x + cosf(a0)*ir, iconCenter.y + sinf(a0)*ir), p0, col, 2.0f);
        }
    }

    return pressed;
}

static bool ToggleSwitch(const char* label, bool* v) {
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);

    float scale = 1.5f; // 1.5f is with 50% bigger than writen values
    float height = 32.0f * scale;
    float width = 56.0f * scale;
    float radius = height * 0.5f;

    ImVec2 textSize = CalcTextSize(label);
    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size = ImVec2(GetContentRegionAvail().x, ImMax(height, textSize.y) + style.FramePadding.y * 2 + 10.0f);

    const ImRect bb(pos, pos + size);
    ItemSize(size, style.FramePadding.y);
    if (!ItemAdd(bb, id)) return false;

    bool hovered, held;
    bool pressed = ButtonBehavior(bb, id, &hovered, &held);
    if (pressed) *v = !*v;

    static std::map<ImGuiID, float> switchAnim;
    float& animT = switchAnim[id];
    float targetT = *v ? 1.0f : 0.0f;
    animT += (targetT - animT) * g.IO.DeltaTime * 14.0f;

    ImDrawList* dl = window->DrawList;
    const MenuTheme& T = GetTheme();
    
    if (hovered) {
        dl->AddRectFilled(bb.Min, bb.Max, IM_COL32(45, 45, 55, 100), 10.0f);
    }
    
    ImVec2 togglePos = ImVec2(bb.Max.x - width - 15.0f, bb.Min.y + (size.y - height) * 0.5f);
    ImVec2 toggleEnd = ImVec2(togglePos.x + width, togglePos.y + height);
    
    ImVec4 offColor  = ImVec4(0.27f, 0.27f, 0.31f, 1.0f);
    ImVec4 onColorV  = ImGui::ColorConvertU32ToFloat4(T.accent);
    ImVec4 bgColorV  = ImLerp(offColor, onColorV, animT);
    dl->AddRectFilled(togglePos, toggleEnd, ImColor(bgColorV), radius);
    
    float knobX = togglePos.x + radius + (width - height) * animT;
    float knobY = togglePos.y + radius;
    float knobR = radius - 4.0f;
    
    dl->AddCircleFilled(ImVec2(knobX, knobY), knobR + 2.0f, IM_COL32(0, 0, 0, 40));
    dl->AddCircleFilled(ImVec2(knobX, knobY), knobR, IM_COL32(255, 255, 255, 255));

    dl->AddText(ImVec2(bb.Min.x + 15.0f, bb.Min.y + (size.y - textSize.y) * 0.5f), IM_COL32(230, 230, 240, 255), label);

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

#include "mod/ButtonClicker.h"

static void DrawToggleButton(); // forward declaration — defined after DrawFloatingButton

static void DrawLiveStatusOverlay(ImGuiIO& io) {
    if (!HasFeature("autoplay") || !persistent_bool[O("bAutoPlay")]) return;

    const char* stateStr = "Idle";
    switch (AutoPlay::state) {
        case AutoPlay::SCANNING:   stateStr = "Scanning";   break;
        case AutoPlay::NOMINATING: stateStr = "Nominating"; break;
        case AutoPlay::EXECUTING:  stateStr = "Executing";  break;
        default:                   stateStr = "Idle";       break;
    }
    bool isPlaying = AutoPlay::bAutoPlaying;

    const float padH  = 24.0f;  // jarak dari tepi kiri layar
    const float padV  = 24.0f;  // jarak dari tepi bawah layar

    SetNextWindowPos(
        ImVec2(padH, io.DisplaySize.y - padV),
        ImGuiCond_Always,
        ImVec2(0.0f, 1.0f)   // anchor: kiri-bawah
    );

    PushStyleColor(ImGuiCol_WindowBg, IM_COL32(14, 14, 18, 185));
    PushStyleColor(ImGuiCol_Border,   IM_COL32(60, 60, 80, 120));
    PushStyleVar(ImGuiStyleVar_WindowRounding,  12.0f);
    PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    PushStyleVar(ImGuiStyleVar_WindowPadding,   ImVec2(14.0f, 10.0f));

    if (Begin(O("##LiveStatus"), nullptr,
              ImGuiWindowFlags_NoTitleBar   | ImGuiWindowFlags_NoResize    |
              ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoScrollbar |
              ImGuiWindowFlags_NoInputs     | ImGuiWindowFlags_NoSavedSettings |
              ImGuiWindowFlags_AlwaysAutoResize)) {

        // Accent bar di kiri window
        ImDrawList* dl = GetWindowDrawList();
        ImVec2 wp = GetWindowPos();
        ImVec2 ws = GetWindowSize();
        ImU32 accentCol = isPlaying
            ? IM_COL32(0, 210, 130, 255)
            : IM_COL32(200, 40, 40, 255);
        dl->AddRectFilled(wp, ImVec2(wp.x + 3.0f, wp.y + ws.y), accentCol, 12.0f, ImDrawFlags_RoundCornersLeft);

        SetWindowFontScale(0.88f);

        const ImVec4 dimCol   = ImGui::ColorConvertU32ToFloat4(IM_COL32(140, 140, 155, 255));
        const ImVec4 valGreen = ImGui::ColorConvertU32ToFloat4(IM_COL32(0,  210, 130, 255));
        const ImVec4 valCyan  = ImGui::ColorConvertU32ToFloat4(IM_COL32(0,  200, 255, 255));
        const ImVec4 valRed   = ImGui::ColorConvertU32ToFloat4(IM_COL32(220, 60,  60,  255));
        const ImVec4 valGold  = ImGui::ColorConvertU32ToFloat4(IM_COL32(255, 200,  40, 255));
        const ImVec4 valGray  = ImGui::ColorConvertU32ToFloat4(IM_COL32(130, 130, 145, 255));

        // Row 1: Auto Play ON / OFF
        TextColored(dimCol, O("Auto Play "));
        SameLine(0, 0);
        TextColored(isPlaying ? valGreen : valRed, isPlaying ? O("ON") : O("OFF"));

        // Row 2: State
        ImU32 stateCol = (AutoPlay::state != AutoPlay::IDLE)
            ? IM_COL32(0, 200, 255, 255) : IM_COL32(130, 130, 145, 255);
        TextColored(dimCol, O("State     "));
        SameLine(0, 0);
        TextColored(ImGui::ColorConvertU32ToFloat4(stateCol), stateStr);

        // ── Shot debug info (only when actively playing) ──────────────────
        if (isPlaying && AutoPlay::g_dbg.candidatesFound > 0) {
            Separator();

            // Row 3: Candidates found in Phase 1
            TextColored(dimCol, O("Candidates"));
            SameLine(0, 0);
            char cBuf[32];
            snprintf(cBuf, sizeof(cBuf), O(" %d"), AutoPlay::g_dbg.candidatesFound);
            TextColored(valCyan, cBuf);

            // Row 4: Tolerance window (0/1/2 out of 2)
            TextColored(dimCol, O("Tolerance "));
            SameLine(0, 0);
            int tp = AutoPlay::g_dbg.tolerancePts;
            ImVec4 tolCol = (tp == 2) ? valGreen : (tp == 1) ? valGold : valRed;
            char tBuf[32];
            snprintf(tBuf, sizeof(tBuf), O(" %d/2"), tp);
            TextColored(tolCol, tBuf);
            if (tp == 2) { SameLine(0,4); TextColored(valGreen, O("*BEST*")); }

            // Row 5: Quality score (lower = better shot)
            TextColored(dimCol, O("Quality   "));
            SameLine(0, 0);
            char qBuf[32];
            snprintf(qBuf, sizeof(qBuf), O(" %.1f"), AutoPlay::g_dbg.qualityScore);
            // Green < 150, Gold < 300, Red >= 300
            ImVec4 qCol = (AutoPlay::g_dbg.qualityScore < 150.0f) ? valGreen
                        : (AutoPlay::g_dbg.qualityScore < 300.0f) ? valGold
                        : valRed;
            TextColored(qCol, qBuf);

            // Row 6: Selection method
            TextColored(dimCol, O("Won by    "));
            SameLine(0, 0);
            if (AutoPlay::g_dbg.wonByTolerance)
                TextColored(valGold, O(" Tolerance"));
            else
                TextColored(valGray, O(" Quality"));
        }

        SetWindowFontScale(1.0f);
    }
    End();
    PopStyleVar(3);
    PopStyleColor(2);
}

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
          if (!g_isInGame) return;

        auto visualCue = sharedGameManager.mVisualCue();

        Ball::Classification myclass = sharedGameManager.getPlayerClassification();

        Table table = sharedGameManager.mTable;
        if (!table) return;

        auto tableProperties = table.mTableProperties();
        if (!tableProperties) return;

        auto& pockets = tableProperties.mPockets();

        GameStateManager gameStateManager = sharedGameManager.mStateManager;
        if (!gameStateManager) return;

        if (HasFeature("autoplay") && persistent_bool[O("bAutoPlay")]) {
            DrawToggleButton();
            AutoPlay::Update();
        }


        auto stateId = gameStateManager.getCurrentStateId();
        if (stateId == 6 || stateId == 8) return;

        // ── Cached simulation: run only when ball layout changes ─────────────
        // Running determineShotResult(false) every frame (60x/s) burns CPU.
        // Cache: compare cue ball position hash; re-run only when it changed.
        {
            static Point2D  s_lastCuePos     = { -9999.0, -9999.0 };
            static int      s_lastStateId    = -1;
            static uint32_t s_skipFrame      = 0;

            Point2D curCuePos = gPrediction->guiData.balls[0].initialPosition;
            double  dx        = curCuePos.x - s_lastCuePos.x;
            double  dy        = curCuePos.y - s_lastCuePos.y;
            bool    moved     = (dx*dx + dy*dy) > 0.25;  // 0.5 unit threshold
            bool    stChanged = (stateId != s_lastStateId);

            // Throttle: even if "moved", skip every other frame to save CPU
            bool doSim = (moved || stChanged) && (++s_skipFrame & 1u);

            if (stateId == 4 && doSim) {
                gPrediction->determineShotResult(false);
                s_lastCuePos  = curCuePos;
                s_lastStateId = stateId;
            }
            if (stateId == 7) {
                if (!HasFeature("esp") || !persistent_bool["bEnemyLine"]) return;
                if (doSim) {
                    gPrediction->determineShotResult(false);
                    s_lastCuePos  = curCuePos;
                    s_lastStateId = stateId;
                }
            }
        }

        if (HasFeature("esp") && persistent_bool[O("bESP_DrawPocketsShotState")]) {
            for (int i = 0; i < 6; i++) {
                if (gPrediction->guiData.pocketStatus[i]) {
                    auto screenPos = WorldToScreen(pockets[i]);
                    draw->AddCircle(ImVec2(screenPos.x, screenPos.y), 30, GREEN, 0, 5.f);
                }
            }
        }

        if (HasFeature("esp") && persistent_bool[O("bESP_DrawPredictionLine")]) {
            // ── Draw only KEY ball trajectories to cut AddLine call count ────
            // Full 16-ball draw: ~400+ AddLine/frame → severe lag at 60fps
            // New: cue ball (0) + target ball + 8-ball only = ~60 AddLine/frame
            int targetBallIdx = (g_CurrentCandidate.idx != -1) ? g_CurrentCandidate.idx : -1;
            float lineThick = (float)persistent_int[O("iLineThickness")];
            if (lineThick < 1.f) lineThick = 1.f;
            float circleR = lineThick + 1.f;
            if (circleR < 2.f) circleR = 2.f;

            for (int i = 0; i < gPrediction->guiData.ballsCount; i++) {
                // Only draw: cue ball (0), active target, 8-ball (8)
                bool isKey = (i == 0) || (i == targetBallIdx) || (i == 8);
                if (!isKey) continue;

                auto& ball = gPrediction->guiData.balls[i];
                if (ball.initialPosition == ball.predictedPosition) continue;

                // Draw trajectory line
                ImVec2 lastPos{};
                for (int j = 1; j < (int)ball.positions.size(); j++) {
                    auto point = WorldToScreen(ball.positions[j]);
                    if (lastPos.x || lastPos.y) draw->AddLine(lastPos, point, colors[i], lineThick);
                    lastPos = point;
                }
                // Draw start dot + predicted landing dot
                draw->AddCircleFilled(WorldToScreen(ball.initialPosition),   circleR, colors[i]);
                draw->AddCircleFilled(WorldToScreen(ball.predictedPosition), 16,      colors[i]);
            }
        }
    }
}

static void DrawSidebar(float sidebarW, float winH, float topOffset) {
    static GLuint play_icon_tex = LoadTextureFromMemory(play_icon_png, play_icon_png_len);
    static GLuint user_icon_tex = LoadTextureFromMemory(user_icon_png, user_icon_png_len);

    ImGuiContext& g  = *GImGui;
    ImDrawList*   dl = GetWindowDrawList();
    ImVec2        wp = GetWindowPos();

    const MenuTheme& T = GetTheme();

    // Vertical separator on right edge of sidebar
    dl->AddLine(
        ImVec2(wp.x + sidebarW - 1.0f, wp.y + topOffset),
        ImVec2(wp.x + sidebarW - 1.0f, wp.y + winH - 10.0f),
        T.separator, 1.0f
    );

    // Stacked tab buttons — fill available height with a small bottom margin
    constexpr int TAB_COUNT = 3;
    const float bottomPad = 20.0f;
    float availH = winH - topOffset - bottomPad;
    float btnH   = availH / (float)TAB_COUNT;

    SetCursorPos(ImVec2(0.0f, topOffset));
    if (SidebarButton("##Aim",      play_icon_tex, g_menu.currentTab == 0, sidebarW, btnH)) g_menu.currentTab = 0;
    if (SidebarButton("##User",     user_icon_tex, g_menu.currentTab == 1, sidebarW, btnH)) g_menu.currentTab = 1;
    if (SidebarButton("##Settings", 0,             g_menu.currentTab == 2, sidebarW, btnH)) g_menu.currentTab = 2;

}

// Reads an IL2CPP/Unity NSString (UTF-16 internal buffer at offset 0x14, length at 0x10)
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

// Shared position for DrawToggleButton and DrawFloatingButton (they move together)
// Both X and Y are free — user can drag to any corner of the screen.
static float g_sideBtnsX      = 0.0f;   // 0 = uninitialized → default right side
static float g_sideBtnsY      = 0.0f;   // 0 = uninitialized → default bottom-right
// Kept for linker compatibility — no longer used for animation
static float g_toggleRotAngle = 0.0f;
// Set true by AutoPlay when in SLOW scan state — shows CALCULATING overlay
static bool  g_autoPlayCalculating = false;

// ── svConfig ──────────────────────────────────────────────────────────────────
static void svConfig_Save() {
    std::string path = O("/data/user/0/") + PACKAGE_NAME + O("/files/svConfig.txt");
    FILE* f = fopen(path.c_str(), O("w"));
    if (!f) return;
    fprintf(f, O("iLineThickness=%d\n"),  persistent_int[O("iLineThickness")]);
      fprintf(f, O("iMenuSizeOffset=%d\n"), persistent_int[O("iMenuSizeOffset")]);
      fprintf(f, O("fAutoPlayPower=%.1f\n"),   persistent_float[O("fAutoPlayPower")]);
      fprintf(f, O("fAutoPlayDelay=%.2f\n"),   persistent_float[O("fAutoPlayShotDelayMax")]);
      fprintf(f, O("fFontScale=%.1f\n"),       persistent_float[O("fFontScale")]);
      fprintf(f, O("bManualPower=%d\n"),       (int)persistent_bool[O("bManualPower")]);
      fprintf(f, O("b9BallAimLock=%d\n"),      (int)persistent_bool[O("b9BallAimLock")]);
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
          float fv__; int bv__;
          if (sscanf(line, O("fAutoPlayPower=%f"),  &fv__) == 1) { persistent_float[O("fAutoPlayPower")]        = fv__; continue; }
          if (sscanf(line, O("fAutoPlayDelay=%f"),  &fv__) == 1) { persistent_float[O("fAutoPlayShotDelayMax")] = fv__; continue; }
          if (sscanf(line, O("fFontScale=%f"),      &fv__) == 1) { persistent_float[O("fFontScale")]            = fv__; continue; }
          if (sscanf(line, O("bManualPower=%d"),    &bv__) == 1) { persistent_bool[O("bManualPower")]           = (bool)bv__; continue; }
          if (sscanf(line, O("b9BallAimLock=%d"),   &bv__) == 1) { persistent_bool[O("b9BallAimLock")]          = (bool)bv__; }
    }
    fclose(f);
}

// ── CALCULATING overlay (shown during AutoPlay SLOW scan) ─────────────────────
static void DrawCalculating(ImGuiIO& io) {
    // Setăm poziția pe centrul ecranului (Width*0.5, Height*0.5)
    // Pivotul (0.5f, 0.5f) înseamnă că mijlocul ferestrei va fi fix pe coordonatele date
    SetNextWindowPos(ImVec2(Width * 0.5f, Height * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    
    // Auto-resize face ca fereastra să aibă dimensiunea textului automat
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


static void DrawContentArea(float winW, float winH) {
    bool need_save = false;
    
    ImDrawList* dl  = GetWindowDrawList();
    ImVec2      wp  = GetWindowPos();

    // ContentCh already starts below header — startY = 0
    float startY   = 0.0f;
    float contentW = winW;

    // Background for content area
    dl->AddRectFilled(
        ImVec2(wp.x, wp.y),
        ImVec2(wp.x + contentW, wp.y + winH),
        IM_COL32(21, 21, 21, 255), 0.0f
    );
    
    static const char* tabTitles[3] = { "AIM", "USER", "SETTINGS" };
    static const char* tabDescs[3]  = {
        "Auto play & draw settings",
        "Account info",
        "Menu configuration"
    };

    // --- HEADER TAB (VISUAL | ESP and draw settings) ---
    const char* currentTitle = tabTitles[g_menu.currentTab];
    const char* currentDesc  = tabDescs[g_menu.currentTab];
    float titlePadT = 14.0f;
    float titlePadB = 10.0f;

    ImVec2 ts1   = CalcTextSize(currentTitle);
    const char* sep = " | ";
    ImVec2 tsSep = CalcTextSize(sep);
    float totalW = ts1.x + tsSep.x + CalcTextSize(currentDesc).x;
    float startX = wp.x + (contentW - totalW) * 0.5f;
    float textY  = wp.y + startY + titlePadT;
    dl->AddText(ImVec2(startX,                   textY), IM_COL32(255,255,255,255), currentTitle);
    dl->AddText(ImVec2(startX + ts1.x,           textY), IM_COL32(90, 90,105,255),  sep);
    dl->AddText(ImVec2(startX + ts1.x + tsSep.x, textY), IM_COL32(130,130,145,255), currentDesc);

    float lineY   = startY + titlePadT + ts1.y + titlePadB;
    dl->AddLine(
        ImVec2(wp.x + 14.0f, wp.y + lineY),
        ImVec2(wp.x + contentW - 14.0f, wp.y + lineY),
        IM_COL32(60, 60, 75, 140), 1.0f
    );
    float headerH = lineY - startY + 8.0f;
    SetCursorPos(ImVec2(10.0f, headerH));

    // Inner scrollable area — only this child scrolls
    PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    BeginChild(O("##ContentArea"), ImVec2(contentW - 20.0f, winH - headerH - 8.0f), false);
    
    switch (g_menu.currentTab) {
        case 0: {
            const MenuTheme& T1 = GetTheme();
            ImDrawList* dl1 = GetWindowDrawList();

            // ── helper: section header ─────────────────────────────────────
            auto SectionHeader = [&](const char* title) {
                Dummy(ImVec2(0, 6));
                ImVec2 p = GetCursorScreenPos();
                float w  = GetContentRegionAvail().x;
                dl1->AddRectFilled(p, ImVec2(p.x + w, p.y + 26.0f),
                    IM_COL32(30, 30, 40, 200), 6.0f);
                dl1->AddRectFilled(p, ImVec2(p.x + 3.0f, p.y + 26.0f),
                    T1.accent, 3.0f);
                ImVec2 ts = CalcTextSize(title);
                dl1->AddText(ImVec2(p.x + 12.0f, p.y + (26.0f - ts.y) * 0.5f),
                    T1.textPrimary, title);
                Dummy(ImVec2(0, 26.0f));
                Dummy(ImVec2(0, 6));
            };

            // ── helper: StatusRow ──────────────────────────────────────────
            auto StatusRow = [&](const char* key, const char* val, ImU32 valCol) {
                float rowH2 = 36.0f;
                ImVec2 p2   = GetCursorScreenPos();
                float  w2   = GetContentRegionAvail().x;
                dl1->AddText(p2, T1.textSecondary, key);
                ImVec2 ks    = CalcTextSize(key);
                float colonX = p2.x + ks.x + 6.0f;
                dl1->AddText(ImVec2(colonX, p2.y), T1.textSecondary, ":");
                dl1->AddText(ImVec2(colonX + 14.0f, p2.y), valCol, val);
                Dummy(ImVec2(w2, rowH2));
            };

            // ═══════════════════════════════════════════════════
            // SECTION: Draw  (paling atas)
            // ═══════════════════════════════════════════════════
            if (HasFeature("esp")) {
                SectionHeader("Draw");
                need_save |= ToggleSwitch(O("Draw Lines"),       &persistent_bool[O("bESP_DrawPredictionLine")]);
                need_save |= ToggleSwitch(O("Draw Pockets"),     &persistent_bool[O("bESP_DrawPocketsShotState")]);
                need_save |= ToggleSwitch(O("Show Enemy Lines"), &persistent_bool["bEnemyLine"]);
            }

            // ═══════════════════════════════════════════════════
            // SECTION: Auto Play
            // ═══════════════════════════════════════════════════
            if (HasFeature("autoplay")) {
                Dummy(ImVec2(0, 8));
                SectionHeader("Auto Play");
                need_save |= ToggleSwitch(O("Auto Play"), &persistent_bool[O("bAutoPlay")]);
            }

            Dummy(ImVec2(0, 4));
            break;
        }

        case 1: {
            // ── helpers ──────────────────────────────────────────────────────
            auto DrawSectionHeader = [&](const char* title) {
                Dummy(ImVec2(0, 14));
                float avail = GetContentRegionAvail().x;
                ImVec2 p    = GetCursorScreenPos();
                float  fs   = GImGui->FontSize;
                ImVec2 ts   = CalcTextSize(title);
                float  lineY = p.y + fs * 0.5f;
                float  gap   = 8.0f;
                float  lineW = (avail - ts.x - gap * 2.0f) * 0.5f;
                ImDrawList* dl2 = GetWindowDrawList();
                dl2->AddLine(ImVec2(p.x,                      lineY), ImVec2(p.x + lineW,                      lineY), IM_COL32(60,60,75,160), 1.0f);
                dl2->AddLine(ImVec2(p.x + lineW + gap + ts.x + gap, lineY), ImVec2(p.x + avail, lineY), IM_COL32(60,60,75,160), 1.0f);
                SetCursorPosX(GetCursorPosX() + lineW + gap);
                TextColored(ImVec4(0.55f, 0.55f, 0.65f, 1.0f), "%s", title);
                Dummy(ImVec2(0, 6));
            };

            auto DrawInfoRow = [&](const char* key, const char* val) {
                TextColored(ImVec4(0.55f, 0.55f, 0.65f, 1.0f), "%s", key);
                SameLine();
                TextColored(ImVec4(0.90f, 0.90f, 0.95f, 1.0f), "%s", val);
                Dummy(ImVec2(0, 4));
            };

            // ── Device Info ───────────────────────────────────────────────────
            {
                static char s_manufacturer[PROP_VALUE_MAX] = {};
                static char s_model[PROP_VALUE_MAX] = {};
                static char s_abi[PROP_VALUE_MAX] = {};
                static char s_android[PROP_VALUE_MAX] = {};
                static bool s_props_loaded = false;
                
                if (!s_props_loaded) {
                    __system_property_get("ro.product.manufacturer", s_manufacturer);
                    __system_property_get("ro.product.model", s_model);
                    __system_property_get("ro.product.cpu.abi", s_abi);
                    __system_property_get("ro.build.version.release", s_android);
                    s_props_loaded = true;}
                    
                int64_t now_ts = (int64_t)time(nullptr);
                int64_t diff = (g_ExpiryTimestamp > 0) ? (g_ExpiryTimestamp - now_ts) : 0;
                char expireBuf[64];
                if (g_ExpiryTimestamp == 0) {
                    snprintf(expireBuf, sizeof(expireBuf), "%s", O("Lifetime"));
                } else if (diff > 0) {
                    int days  = (int)(diff / 86400);
                    int hours = (int)((diff % 86400) / 3600);
                    int mins  = (int)((diff % 3600)  / 60);
                    int secs  = (int)(diff % 60);
                    if (days > 0)
                        snprintf(expireBuf, sizeof(expireBuf), "%dd %dh %dm %ds", days, hours, mins, secs);
                    else if (hours > 0)
                        snprintf(expireBuf, sizeof(expireBuf), "%dh %dm %ds", hours, mins, secs);
                    else
                        snprintf(expireBuf, sizeof(expireBuf), "%dm %ds", mins, secs);
                } else {
                    snprintf(expireBuf, sizeof(expireBuf), "%s", O("EXPIRED"));}
                    
                DrawInfoRow(O("ManuFacturer: "), s_manufacturer);
                DrawInfoRow(O("Model: "), s_model);
                DrawInfoRow(O("Abi: "), s_abi);
                DrawInfoRow(O("Android: "), s_android);
                DrawInfoRow(O("Key: "), persistent_string["key"].c_str());
                {
                      TextColored(ImVec4(0.55f,0.55f,0.65f,1.0f), "%s", O("Expire: "));
                      SameLine();
                      int64_t secsLeft = (g_ExpiryTimestamp > 0) ? (g_ExpiryTimestamp - (int64_t)time(nullptr)) : 1;
                      ImVec4 expColor = (g_ExpiryTimestamp == 0) ? ImVec4(0.40f,0.90f,0.55f,1.0f) :
                                        (secsLeft <= 0)           ? ImVec4(1.00f,0.30f,0.30f,1.0f) :
                                        (secsLeft < 86400)        ? ImVec4(1.00f,0.40f,0.40f,1.0f) :
                                        (secsLeft < 7*86400)      ? ImVec4(1.00f,0.72f,0.18f,1.0f) :
                                                                    ImVec4(0.35f,0.92f,0.48f,1.0f);
                      TextColored(expColor, "%s", expireBuf);
                  }}

                // ── Sync — "* live sync..." hijau berkedip (ASCII only, no unicode) ──
                {
                    Dummy(ImVec2(0, 2));
                    float pulse = (sinf(ImGui::GetTime() * 3.5f) + 1.0f) * 0.5f;
                    float alpha = 0.55f + 0.45f * pulse;
                    TextColored(ImVec4(0.0f, 0.75f + 0.25f * pulse, 0.35f, alpha),
                        O("> Live Sync..."));
                    Dummy(ImVec2(0, 8));
                }

                // ── Change Key ──────────────────────────────────────────
                DrawSectionHeader(O("Update Key"));
                Dummy(ImVec2(0, 6));
                {
                    static char  s_newKey[128]  = {};
                    static bool  s_keyApplying  = false;
                    static std::string s_keyMsg = "";

                    // Indent for breathing room on both sides
                    const float hPad = 12.0f;
                    Indent(hPad);
                    float avW = GetContentRegionAvail().x - hPad;
                    // ── Row 1: InputText full width — no buttons on same row ──
                    PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 9.0f));
                    PushStyleColor(ImGuiCol_FrameBg, IM_COL32(22, 28, 50, 230));
                    SetNextItemWidth(avW);
                    InputText(O("##ChgKey"), s_newKey, sizeof(s_newKey));
                    PopStyleColor();
                    PopStyleVar();

                    // ── Row 2: Paste + Apply half-width — never overflow ──
                    Dummy(ImVec2(0, 5));
                    float btnW = (avW - 10.0f) * 0.5f;

                    PushStyleColor(ImGuiCol_Button,        IM_COL32(30, 40, 70, 220));
                    PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(45, 60, 110, 255));
                    PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 9.0f));
                    if (Button(O("Paste"), ImVec2(btnW, 0))) {
                        JNIEnv* penv = nullptr;
                        jint jr = VM->GetEnv((void**)&penv, JNI_VERSION_1_6);
                        if (jr == JNI_EDETACHED) VM->AttachCurrentThread(&penv, nullptr);
                        if (penv) {
                            std::string clip = getClipboard(penv);
                            if (!clip.empty()) {
                                strncpy(s_newKey, clip.c_str(), sizeof(s_newKey)-1);
                                s_newKey[sizeof(s_newKey)-1] = '\0';
                            }
                        }
                    }
                    PopStyleVar();
                    PopStyleColor(2);
                    SameLine(0, 10);

                    bool canApply = s_newKey[0] != '\0' && !s_keyApplying && !is_logging_in;
                    if (!canApply) BeginDisabled();
                    PushStyleColor(ImGuiCol_Button,        IM_COL32(30, 55, 110, 220));
                    PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(55, 90, 160, 255));
                    PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 9.0f));
                    if (Button(O("Apply"), ImVec2(btnW, 0))) {
                        s_keyMsg = "";
                        s_keyApplying = true;
                        std::string nk(s_newKey);
                        std::string hw = persistent_string["hwid"];
                        std::thread([nk, hw]() {
                            bool ok = Login(hw, nk);
                            if (ok) { s_keyMsg = OO("Key updated!").str(); RefreshLicenseStatus(); memset(s_newKey, 0, sizeof(s_newKey)); }
                            else    { s_keyMsg = ERROR_MESSAGE.empty() ? OO("Invalid key").str() : ERROR_MESSAGE; }
                            s_keyApplying = false;
                        }).detach();
                    }
                    PopStyleVar();    // FramePadding Apply
                    PopStyleColor(2); // Button colors Apply
                    if (!canApply) EndDisabled();

                    if (s_keyApplying || is_logging_in)
                        TextColored(ImVec4(0.9f,0.75f,0.2f,1.0f), O("Verifying..."));
                    else if (!s_keyMsg.empty()) {
                        bool ok2 = (s_keyMsg.find("updated") != std::string::npos);
                        TextColored(ok2 ? ImVec4(0.35f,0.9f,0.5f,1.0f) : ImVec4(1.0f,0.35f,0.35f,1.0f),
                                    "%s", s_keyMsg.c_str());
                    }
                    Unindent(hPad);
                }
                Dummy(ImVec2(0, 10));

            break;
        }

        case 2: {
            const MenuTheme& TT = GetTheme();
            auto accentF = ImGui::ColorConvertU32ToFloat4(TT.accent);

            // ── Theme Picker ─────────────────────────────────────────────
            Dummy(ImVec2(0, 10));
            TextColored(ImVec4(0.62f, 0.62f, 0.70f, 1.0f), O("Theme"));
            Dummy(ImVec2(0, 8));
            {
                struct ThemeOpt { const char* name; ImU32 preview; };
                static const ThemeOpt opts[3] = {
                    { "Dark Red",   IM_COL32(200,30,30,255)  },
                    { "Dark Glass", IM_COL32(60,140,220,255) },
                    { "Dark Neon",  IM_COL32(0,220,180,255)  },
                };
                int& th = persistent_int["iTheme"];
                if (th < 0 || th > 2) th = 0;
                float avail = GetContentRegionAvail().x;
                float gap   = 8.0f;
                float bW    = (avail - gap * 2.0f) / 3.0f;
                float bH    = 46.0f;
                PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
                for (int i = 0; i < 3; i++) {
                    if (i > 0) SameLine(0, gap);
                    bool isSel = (th == i);
                    ImVec4 bg4 = ImGui::ColorConvertU32ToFloat4(IM_COL32(28,28,36,255));
                    ImVec4 pr4 = ImGui::ColorConvertU32ToFloat4(opts[i].preview);
                    if (isSel) bg4 = ImVec4(bg4.x+0.06f, bg4.y+0.06f, bg4.z+0.06f, 1.0f);
                    PushStyleColor(ImGuiCol_Button,        bg4);
                    PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(bg4.x+0.05f,bg4.y+0.05f,bg4.z+0.05f,1.f));
                    PushStyleColor(ImGuiCol_ButtonActive,  bg4);
                    if (Button(opts[i].name, ImVec2(bW, bH))) { th = i; need_save = true; }
                    if (isSel) {
                        ImVec2 p = GetItemRectMin(), q = GetItemRectMax();
                        GetWindowDrawList()->AddRect(p, q, opts[i].preview, 10.0f, 0, 2.0f);
                        // colour dot
                        GetWindowDrawList()->AddCircleFilled(ImVec2(p.x+12,p.y+bH*0.5f), 5.0f, opts[i].preview);
                    }
                    PopStyleColor(3);
                }
                PopStyleVar();
            }

            // ── Line Thickness ───────────────────────────────────────────
            Dummy(ImVec2(0, 18));
            TextColored(ImVec4(0.62f, 0.62f, 0.70f, 1.0f), O("Settings"));
            Dummy(ImVec2(0, 8));
            TextColored(ImVec4(0.75f, 0.75f, 0.8f, 1.0f), O("Line Thickness"));
            Dummy(ImVec2(0, 6));
            {
                if (persistent_int[O("iLineThickness")] < 1) persistent_int[O("iLineThickness")] = 4;
                PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
                PushStyleVar(ImGuiStyleVar_GrabRounding,  10.0f);
                PushStyleColor(ImGuiCol_FrameBg,          ImVec4(0.12f,0.12f,0.15f,1.0f));
                PushStyleColor(ImGuiCol_SliderGrab,       accentF);
                PushStyleColor(ImGuiCol_SliderGrabActive, accentF);
                SetNextItemWidth(GetContentRegionAvail().x);
                need_save |= SliderInt(O("##lineThick"), &persistent_int[O("iLineThickness")], 1, 10, "%d");
                PopStyleColor(3); PopStyleVar(2);
            }

            // ── Menu Size ─────────────────────────────────────────────────
            Dummy(ImVec2(0, 14));
            TextColored(ImVec4(0.75f, 0.75f, 0.8f, 1.0f), O("Menu Size"));
            Dummy(ImVec2(0, 6));
            {
                int& menuSz = persistent_int[O("iMenuSizeOffset")];
                if (menuSz <= 0) menuSz = 15;
                PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
                PushStyleVar(ImGuiStyleVar_GrabRounding,  10.0f);
                PushStyleColor(ImGuiCol_FrameBg,          ImVec4(0.12f,0.12f,0.15f,1.0f));
                PushStyleColor(ImGuiCol_SliderGrab,       accentF);
                PushStyleColor(ImGuiCol_SliderGrabActive, accentF);
                SetNextItemWidth(GetContentRegionAvail().x);
                static char s_menuFmt[8];
                snprintf(s_menuFmt, sizeof(s_menuFmt), "%d", menuSz);
                need_save |= SliderInt(O("##menuSize"), &menuSz, 0, 20, s_menuFmt);
                PopStyleColor(3); PopStyleVar(2);
            }

              // ── Save Config ───────────────────────────────────────────────
              Dummy(ImVec2(0, 18));
            {
                ImVec4 ab = accentF;
                PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
                PushStyleColor(ImGuiCol_Button,        ImVec4(ab.x*0.55f,ab.y*0.55f,ab.z*0.55f,1.f));
                PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(ab.x*0.75f,ab.y*0.75f,ab.z*0.75f,1.f));
                PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(ab.x*0.40f,ab.y*0.40f,ab.z*0.40f,1.f));
                if (Button(O("Save Config"), ImVec2(GetContentRegionAvail().x, 52.0f)))
                    svConfig_Save();
                PopStyleColor(3); PopStyleVar();
            }
            break;
        }
    }
    
    if (need_save) save_persistence();
    
    EndChild();
    PopStyleColor();
}

static void DrawMinimizedIcon(ImGuiIO& io) {
      if (!g_menu.isOpen || !g_menu.isMinimized) return;

      static GLuint s_minLynTex = 0;
      if (!s_minLynTex) s_minLynTex = LoadTextureFromMemory(lyn4xp_logo_png, lyn4xp_logo_png_len);

      float iconSz = 54.0f;
      float padV   = 8.0f;
      float fpsH   = GetFontSize() + 10.0f;  // dynamic: never clips any font size
      float winW   = iconSz + 56.0f;         // wide enough for "999 FPS"
      float winH   = padV + iconSz + padV + fpsH + 14.0f;  // 14px bottom breathing room

      static float s_minPosY = 120.0f;
      float fixedX = io.DisplaySize.x - 4.0f - winW;  // 4px gap from right edge

      SetNextWindowPos(ImVec2(fixedX, s_minPosY), ImGuiCond_Always);
      SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_Always);

      PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
      PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
      PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

      if (Begin(O("##MinIcon"), nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {

          ImDrawList* dl = GetWindowDrawList();
          ImVec2 wp      = GetWindowPos();
          float  cx      = wp.x + winW * 0.5f;
          float  cy      = wp.y + padV + iconSz * 0.5f;
          float  r       = iconSz * 0.5f + 4.0f;

          // Background circle + glow
          dl->AddCircleFilled(ImVec2(cx, cy), r, IM_COL32(10, 12, 22, 210));
          dl->AddCircle(ImVec2(cx, cy), r, IM_COL32(0, 210, 180, 160), 0, 2.0f);

          // LYN4XP logo inside circle
          if (s_minLynTex) {
              ImVec2 iMin(cx - iconSz * 0.5f, cy - iconSz * 0.5f);
              ImVec2 iMax(cx + iconSz * 0.5f, cy + iconSz * 0.5f);
              dl->AddImageRounded((void*)(intptr_t)s_minLynTex, iMin, iMax,
                  ImVec2(0,0), ImVec2(1,1), IM_COL32(255,255,255,240), iconSz * 0.3f);
          }

          // FPS label below icon
          static char s_fpsBuf[16];
          snprintf(s_fpsBuf, sizeof(s_fpsBuf), "%.0f FPS", io.Framerate);
          ImVec2 fpsTz = CalcTextSize(s_fpsBuf);
          float  fpsX  = wp.x + (winW - fpsTz.x) * 0.5f;
          // Vertically centre text within the fpsH zone
          float  fpsZoneTop = wp.y + padV + iconSz + padV;
          float  fpsY       = fpsZoneTop + (fpsH - fpsTz.y) * 0.5f;
          dl->AddText(ImVec2(fpsX, fpsY), IM_COL32(0, 220, 180, 220), s_fpsBuf);

          // Invisible hit area for drag + tap-to-restore
          SetCursorPos(ImVec2(0, 0));
          InvisibleButton(O("##MinIconHit"), ImVec2(winW, winH));
          if (IsItemActive() && IsMouseDragging(ImGuiMouseButton_Left)) {
              s_minPosY += io.MouseDelta.y;
              s_minPosY = ImClamp(s_minPosY, 60.0f, io.DisplaySize.y - winH - 20.0f);
          }
          if (IsItemHovered() && IsMouseReleased(0) && ImGui::GetMouseDragDelta(0).y == 0.0f) {
              g_menu.isMinimized = false;
          }
      }
      End();
      PopStyleVar(2);
      PopStyleColor();
  }

  
INLINE void DrawMenu(ImGuiIO& io) {
    if ((!g_Token.empty() && !g_Auth.empty() && g_Token == g_Auth) || DEBUG_BYPASS_LOGIN) {
        if (is_segv_handler_active()) {
            jump_buffer_active = 1;
            if (!sigsetjmp(jump_buffer, 1)) DrawESP(GetBackgroundDrawList());
            jump_buffer_active = 0;
        }

        float targetAlpha = g_menu.isOpen ? 1.0f : 0.0f;
        if (g_menu.isOpen) {
            g_menu.menuAlpha += (1.0f - g_menu.menuAlpha) * io.DeltaTime * 12.0f;
        } else {
            g_menu.menuAlpha = 0.0f;
        }

        if (g_menu.menuAlpha > 0.01f && !g_menu.isMinimized) {
            // First-run defaults
            static bool s_menuSzInited = false;
            if (!s_menuSzInited) {
                s_menuSzInited = true;
                if (persistent_int[O("iMenuSizeOffset")] <= 0)
                    persistent_int[O("iMenuSizeOffset")] = 15;  // default size 15
                if (persistent_int["iTheme"] <= 0)
                    persistent_int["iTheme"] = 2;               // default Dark Neon
            }
            float sizeScale = 1.0f + (float)persistent_int[O("iMenuSizeOffset")] * 0.03f;
            if (sizeScale < 0.3f) sizeScale = 0.3f;
            float winW    = g_menu.sidebarWidth * sizeScale;
            float _hdrH   = (40.0f + 20.0f) * sizeScale; // header-only height
            float winH    = g_menu.isMinimized ? _hdrH : 420.0f * sizeScale;
            float sbW  = 115.0f * sizeScale;

            SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_Always);
            SetNextWindowPos(ImVec2(20.0f, 80.0f), ImGuiCond_Appearing);

            PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
            PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            PushStyleVar(ImGuiStyleVar_Alpha, g_menu.menuAlpha);

            ImGuiWindowFlags winFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                        ImGuiWindowFlags_NoResize    | ImGuiWindowFlags_NoNav;

            if (Begin(O("##MainMenu"), &g_menu.isOpen, winFlags)) {
                static GLuint lyn4xp_menu_tex = LoadTextureFromMemory(lyn4xp_logo_png, lyn4xp_logo_png_len);

                ImDrawList* bg = GetWindowDrawList();
                ImVec2 wp0 = GetWindowPos();
                const MenuTheme& T = GetTheme();
                bg->AddRectFilled(wp0, ImVec2(wp0.x + winW, wp0.y + winH), T.bgMain, 16.0f);

                // ── Header bar background ─────────────────────────────────
                float logoSz  = 40.0f * sizeScale;
                float logoPad = 10.0f * sizeScale;
                float headerH = logoSz + logoPad * 2.0f;
                bg->AddRectFilled(wp0, ImVec2(wp0.x + winW, wp0.y + headerH),
                    T.bgHeader, 16.0f, ImDrawFlags_RoundCornersTop);
                bg->AddLine(ImVec2(wp0.x, wp0.y + headerH),
                    ImVec2(wp0.x + winW, wp0.y + headerH), T.separator, 1.0f);

                // ── LYN4XP logo top-LEFT ──────────────────────────────────
                ImVec2 logoMin = ImVec2(wp0.x + logoPad, wp0.y + logoPad);
                ImVec2 logoMax = ImVec2(logoMin.x + logoSz, logoMin.y + logoSz);
                if (lyn4xp_menu_tex)
                    bg->AddImageRounded((void*)(intptr_t)lyn4xp_menu_tex, logoMin, logoMax,
                        ImVec2(0,0), ImVec2(1,1), IM_COL32(255,255,255,240), logoSz * 0.25f);

                // ── Header info text — full always, clip rect guards buttons ──
                {
                    static char s_fps[16];
                    snprintf(s_fps, sizeof(s_fps), "%.0f", io.Framerate);

                    static const char* s_prefix = "V.1.0 | 8BP 56.26.0 | x64 | FPS ";
                    ImVec2 prefSz = CalcTextSize(s_prefix);

                    float infoX    = logoMax.x + 8.0f;
                    float infoY    = wp0.y + (headerH - GetFontSize()) * 0.5f;
                    float xBtnLeft = wp0.x + winW - 52.0f * sizeScale;

                    // Clip rect: text cannot paint over buttons
                    bg->PushClipRect(ImVec2(infoX, wp0.y), ImVec2(xBtnLeft, wp0.y + headerH), true);
                    bg->AddText(ImVec2(infoX, infoY),
                        IM_COL32(160, 165, 180, 200), s_prefix);
                    bg->AddText(ImVec2(infoX + prefSz.x, infoY),
                        IM_COL32(0, 255, 140, 255), s_fps);
                    bg->PopClipRect();
                }

                // ── Buttons: X close and minimize — hidden when minimized ─────
                float xBtnSz = 32.0f * sizeScale;
                float xPad   = logoPad;
                ImVec2 xCtr  = ImVec2(wp0.x + winW - xPad - xBtnSz * 0.5f,
                                      wp0.y + logoPad + logoSz * 0.5f);

                if (!g_menu.isMinimized) {
                    // X close button
                    float mxD = io.MousePos.x - xCtr.x;
                    float myD = io.MousePos.y - xCtr.y;
                    bool  xHov = (mxD*mxD + myD*myD) <= (xBtnSz*0.5f)*(xBtnSz*0.5f);
                    if (xHov && io.MouseClicked[0]) g_menu.isOpen = false;
                    bg->AddCircleFilled(xCtr, xBtnSz * 0.5f, xHov ? T.xBtnHov : T.xBtn);
                    float xH = xBtnSz * 0.26f;
                    bg->AddLine(ImVec2(xCtr.x-xH,xCtr.y-xH), ImVec2(xCtr.x+xH,xCtr.y+xH),
                        IM_COL32(255,255,255,255), 2.5f);
                    bg->AddLine(ImVec2(xCtr.x+xH,xCtr.y-xH), ImVec2(xCtr.x-xH,xCtr.y+xH),
                        IM_COL32(255,255,255,255), 2.5f);

                    // Minimize button
                    ImVec2 minCtr(xCtr.x - xBtnSz - xPad * 0.8f, xCtr.y);
                    float dx = io.MousePos.x - minCtr.x, dy = io.MousePos.y - minCtr.y;
                    bool minHov = (dx*dx + dy*dy) <= (xBtnSz*0.5f)*(xBtnSz*0.5f);
                    if (minHov && io.MouseClicked[0]) g_menu.isMinimized = true;
                    bg->AddCircleFilled(minCtr, xBtnSz * 0.5f,
                        minHov ? IM_COL32(70,110,200,210) : IM_COL32(40,55,85,160));
                    float lH = xBtnSz * 0.22f;
                    bg->AddLine(ImVec2(minCtr.x-lH,minCtr.y+lH*0.4f),
                                ImVec2(minCtr.x+lH,minCtr.y+lH*0.4f),
                                IM_COL32(255,255,255,220), 2.5f);
                } else {
                    // Minimized: tap anywhere on the bar to restore
                    bool barHov = io.MousePos.x >= wp0.x && io.MousePos.x <= wp0.x + winW
                               && io.MousePos.y >= wp0.y && io.MousePos.y <= wp0.y + headerH;
                    if (barHov && io.MouseClicked[0]) g_menu.isMinimized = false;
                }

                if (!g_menu.isMinimized) { // hide sidebar+content when minimized
                // ── Sidebar: full height, NO scroll ──────────────────────
                SetCursorPos(ImVec2(0, 0));
                PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
                PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                BeginChild(O("##SidebarCh"), ImVec2(sbW, winH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                DrawSidebar(sbW, winH, headerH);
                EndChild();
                PopStyleVar();
                PopStyleColor();

                // ── Content: starts BELOW header, NO outer scroll ─────────
                SetCursorPos(ImVec2(sbW, headerH));
                PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
                PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                BeginChild(O("##ContentCh"), ImVec2(winW - sbW, winH - headerH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                DrawContentArea(winW - sbW, winH - headerH);
                EndChild();
                PopStyleVar();
                PopStyleColor();
                } // end !isMinimized
            }
            End();

            PopStyleVar(4);
            PopStyleColor();
        }
    }
}

// ــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــ //

static void DrawToggleButton() {
    if (g_menu.isOpen && !g_menu.isMinimized) return; // Hide when full menu open; still show when minimized

    ImGuiIO& io = GetIO();

    float button_size = 78.f;
    float windowWidth  = button_size + GetStyle().WindowPadding.x * 2.0f;
    float windowHeight = button_size + GetStyle().WindowPadding.y * 2.0f;
    const float rightMargin = 20.0f;

    // Default X: right side of screen (first time only)
    if (g_sideBtnsX <= 0.0f)
        g_sideBtnsX = io.DisplaySize.x - rightMargin;
    if (g_sideBtnsY <= 0.0f)
        g_sideBtnsY = io.DisplaySize.y - 150.0f;

    // Toggle button X: aligned with the shared X anchor (right edge of button group)
    float fixedX = ImClamp(g_sideBtnsX - windowWidth, 0.0f, io.DisplaySize.x - windowWidth);

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
            AutoPlay::bAutoPlaying = !AutoPlay::bAutoPlaying;
            if (AutoPlay::bAutoPlaying) AutoPlay::ClearState();
        }
        bool hov = IsItemHovered();

        float r = size.x * 0.5f;
        ImDrawList* dl = GetWindowDrawList();

        bool active = AutoPlay::bAutoPlaying;

        // ── Drawn icon — simple & clean ──────────────────────────────────────
        // Outer glow
        ImU32 glowCol = active
            ? IM_COL32(0, 220, 120, hov ? 90 : 40)
            : IM_COL32(180, 30, 30, hov ? 90 : 40);
        dl->AddCircleFilled(center, r + 6.0f, glowCol);

        // Main circle background
        ImU32 bgCol = active
            ? IM_COL32(12, 28, 20, 235)
            : IM_COL32(22, 12, 12, 235);
        dl->AddCircleFilled(center, r, bgCol);

        // Border ring
        ImU32 ringCol = active
            ? IM_COL32(0, 210, 120, hov ? 255 : 200)
            : IM_COL32(200, 30, 30, hov ? 255 : 180);
        dl->AddCircle(center, r, ringCol, 0, 3.0f);

        if (active) {
            // Pause icon — two vertical bars
            float bH  = r * 0.52f;
            float bW  = r * 0.16f;
            float gap = r * 0.13f;
            float lx  = center.x - gap * 0.5f - bW;
            float rx  = center.x + gap * 0.5f;
            float ty  = center.y - bH * 0.5f;
            dl->AddRectFilled(ImVec2(lx,    ty), ImVec2(lx+bW, ty+bH), IM_COL32(0, 230, 140, 255), 2.0f);
            dl->AddRectFilled(ImVec2(rx,    ty), ImVec2(rx+bW, ty+bH), IM_COL32(0, 230, 140, 255), 2.0f);
        } else {
            // Play icon — filled triangle ▶
            float hs = r * 0.36f;
            ImVec2 p0(center.x - hs * 0.45f, center.y - hs);
            ImVec2 p1(center.x - hs * 0.45f, center.y + hs);
            ImVec2 p2(center.x + hs * 1.0f,  center.y);
            dl->AddTriangleFilled(p0, p1, p2, IM_COL32(220, 60, 60, 255));
        }

        // Small status label below icon
        const char* statusLbl = active ? "ON" : "OFF";
        ImVec2 tSz = CalcTextSize(statusLbl);
        ImU32  tCol = active ? IM_COL32(0,220,120,230) : IM_COL32(200,60,60,230);
        dl->AddText(ImVec2(center.x - tSz.x * 0.5f, center.y + r * 0.62f), tCol, statusLbl);
    }
    End();
    PopStyleVar();
    PopStyleColor(2);
}

// ــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــ //

static void DrawFloatingButton(ImGuiIO& io) {
    if (g_menu.isOpen) return;

    // Simple floating button — clean circle design
    float btnR    = 38.0f;   // radius
    float winSize = btnR * 2.0f + 8.0f;
    const float rightMargin = 24.0f;

    // Initialise shared position to bottom-right (first frame only)
    if (g_sideBtnsX <= 0.0f)
        g_sideBtnsX = io.DisplaySize.x - rightMargin;
    if (g_sideBtnsY <= 0.0f)
        g_sideBtnsY = io.DisplaySize.y - 150.0f;

    // Floating button sits 100px ABOVE the toggle button, centred on same X anchor
    float toggleWidth = 78.f + (GetStyle().WindowPadding.x * 2.0f);
    float posX = ImClamp(g_sideBtnsX - toggleWidth + (toggleWidth - winSize) * 0.5f,
                         0.0f, io.DisplaySize.x - winSize);
    float posY = g_sideBtnsY - 100.0f;

    SetNextWindowPos(ImVec2(posX, posY), ImGuiCond_Always);
    SetNextWindowSize(ImVec2(winSize, winSize), ImGuiCond_Always);

    PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    if (Begin(O("##FloatBtn"), nullptr,
              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
              ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {

        ImDrawList* dl = GetWindowDrawList();
        ImVec2 center  = ImVec2(posX + winSize * 0.5f, posY + winSize * 0.5f);

        InvisibleButton(O("##FloatBtnHit"), ImVec2(winSize, winSize));

        // Drag: move BOTH X and Y freely anywhere on screen
        if (IsItemActive() && IsMouseDragging(ImGuiMouseButton_Left)) {
            g_sideBtnsX += io.MouseDelta.x;
            g_sideBtnsY += io.MouseDelta.y;
            // Clamp: keep buttons fully visible
            g_sideBtnsX = ImClamp(g_sideBtnsX, toggleWidth, io.DisplaySize.x - 4.0f);
            g_sideBtnsY = ImClamp(g_sideBtnsY, 140.0f,      io.DisplaySize.y - 80.0f);
        }

        // Tap opens menu — use total drag distance, not just Y
        ImVec2 dd = ImGui::GetMouseDragDelta(0);
        bool wasTap = (dd.x * dd.x + dd.y * dd.y) < 16.0f;  // <4px total movement
        if (IsItemHovered() && IsMouseReleased(0) && wasTap) {
            g_menu.isOpen = true;
        }

        bool hov = IsItemHovered();

        // Outer glow ring
        dl->AddCircle(center, btnR + 5.0f, IM_COL32(200, 30, 30, hov ? 120 : 60), 0, 2.0f);
        // Main filled circle — dark
        dl->AddCircleFilled(center, btnR, hov ? IM_COL32(38, 38, 48, 245) : IM_COL32(22, 22, 30, 230));
        // Red accent border
        dl->AddCircle(center, btnR, IM_COL32(200, 30, 30, hov ? 255 : 180), 0, 2.5f);

        // LYN4XP logo inside floating button
        static GLuint s_lyn4xp_float_tex = LoadTextureFromMemory(lyn4xp_logo_png, lyn4xp_logo_png_len);
        if (s_lyn4xp_float_tex) {
            float imgSz = btnR * 0.95f;
            ImVec2 iMin(center.x - imgSz, center.y - imgSz);
            ImVec2 iMax(center.x + imgSz, center.y + imgSz);
            dl->AddImageRounded((void*)(intptr_t)s_lyn4xp_float_tex, iMin, iMax,
                ImVec2(0,0), ImVec2(1,1), IM_COL32(255,255,255,230), btnR * 0.60f);
        }
    }
    End();
    PopStyleVar(2);
    PopStyleColor();
}

// ــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــــ //

// ── License status overlay — top-right, non-intrusive ───────────────────────
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

  // ── License block screen — shown when banned / expired / old version ──────────
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
      float cardH = 0.0f; // auto-size via AlwaysAutoResize

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

      // Card border glow
      dl->AddRectFilled(ImVec2(wp.x-2,wp.y-2), ImVec2(wp.x+cardW+2,wp.y+340),
                        IM_COL32(0,200,180,14), 24.0f);
      dl->AddRect(wp, ImVec2(wp.x+cardW,wp.y+340),
                  IM_COL32(0,200,180,80), 22.0f, 0, 1.5f);
      // Top neon bar
      DrawGradientRect(dl, wp, ImVec2(wp.x+cardW,wp.y+3),
                       IM_COL32(0,200,180,255), IM_COL32(80,80,230,255), true);
      dl->AddRectFilled(wp, ImVec2(wp.x+cardW,wp.y+22),
                        IM_COL32(0,200,180,38), 22.0f, ImDrawFlags_RoundCornersTop);

      // Icon circle — top center
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

      // Divider
      dl->AddLine(ImVec2(wp.x+50.0f,wp.y+116.0f),
                  ImVec2(wp.x+cardW-50.0f,wp.y+116.0f),
                  IM_COL32(30,50,75,150), 1.0f);

      // Top padding
      Dummy(ImVec2(cardW, 128.0f));

      // Error box
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
          sa += io.DeltaTime * 4.5f;
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
          const char* ins = O("Paste your license key, then tap LOGIN");
          SetCursorPosX((cardW-CalcTextSize(ins).x)*0.5f);
          TextColored(ImVec4(0.43f,0.50f,0.62f,1.0f), O("Paste your license key, then tap LOGIN"));

          Dummy(ImVec2(0, 22.0f));

          bool AutoLogin = first_time && !persistent_string["key"].empty();
          SetCursorPosX(36.0f);

          PushStyleColor(ImGuiCol_Button,        ImVec4(0.02f,0.18f,0.16f,1.0f));
          PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.04f,0.25f,0.22f,1.0f));
          PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.01f,0.12f,0.10f,1.0f));
          PushStyleColor(ImGuiCol_Text,          ImVec4(0.0f,0.90f,0.80f,1.0f));
          PushStyleColor(ImGuiCol_Border,        ImVec4(0.0f,0.72f,0.62f,0.60f));
          PushStyleVar(ImGuiStyleVar_FrameRounding,   14.0f);
          PushStyleVar(ImGuiStyleVar_FrameBorderSize,  1.5f);
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
                      std::thread([](std::string aid, std::string k) {
                          Login(aid, k);
                      }, getAndroidID(env), AutoLogin ? persistent_string["key"] : getClipboard(env)).detach();
                  }
                  first_time = false;
              }
          }
          PopStyleVar(2);
          PopStyleColor(5);
          Dummy(ImVec2(0, 32.0f));
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
    auto& io = ImGui::GetIO();

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
          // Periodic license status check every 5 minutes
          {
              // Per-frame local expiry check — instant lock when countdown hits 0
              if (logged_in && g_ExpiryTimestamp > 0 && g_LicenseStatusCode == LSC_OK) {
                  if ((int64_t)time(nullptr) >= g_ExpiryTimestamp) {
                      g_LicenseStatusCode = LSC_EXPIRED;
                      g_LicenseStatusMsg  = OO("License has expired").str();
                  }
              }
              static time_t s_lastCheck = 0;
              time_t nowT = time(nullptr);
              if (logged_in && (nowT - s_lastCheck) > 1 && !g_StatusChecking) {  // realtime: every 1s
                  RefreshLicenseStatus();
                  s_lastCheck = nowT;
              }
          }
          if (g_LicenseStatusCode == LSC_BANNED || g_LicenseStatusCode == LSC_EXPIRED || g_LicenseStatusCode == LSC_VERSION_OLD) {
              DrawLicenseBlockScreen(io);
              DrawLicenseStatusOverlay(io);
          } else {
              // Hysteresis: only close menu after several consecutive non-game frames
              static int s_notInGameFrames = 0;
              if (!g_isInGame) { s_notInGameFrames++; if (s_notInGameFrames > 8) { g_menu.isOpen = false; g_menu.isMinimized = false; } }
              else             { s_notInGameFrames = 0; }
              if (g_isInGame) DrawFloatingButton(io);
              DrawMinimizedIcon(io);
              DrawMenu(io);

  {
      SetNextWindowPos(ImVec2(Width * 0.5f, Height - 60.0f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
      Begin(O("##PoweredBy"), nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoInputs);
      TextColored(ImColor(0, 255, 0, 255), O("Owner By @xabi666"));
      End();
  }

              DrawLiveStatusOverlay(io);
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

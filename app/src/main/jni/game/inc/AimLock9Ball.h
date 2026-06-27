#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  9-Ball AimLock  — WIN-SHOT focused scan engine
//
//  9-Ball rules (Miniclip 8BP):
//    • Cue ball MUST contact the LOWEST numbered ball first
//    • Any ball may be pocketed on the shot
//    • Pocketing the 9-ball at any time (legally) = INSTANT WIN
//    • After first contact: at least one ball pocketed OR one ball hits cushion
//    • Cue ball scratch = foul (ball-in-hand for opponent)
//
//  Scan priority (WIN-FIRST):
//    1. FAST WIN  — ghost-ball geometry: cue → lowest → 9-ball (combo/direct)
//    2. SLOW WIN  — brute-force sweep, only accept 9-ball pocketed
//    3. SLOW LEGAL— brute-force sweep, accept any legal ball pocketed
//  All modes verify: cue stays on table + first hit = lowest ball.
// ─────────────────────────────────────────────────────────────────────────────

#include "Prediction.fast.h"
#include <imgui/imgui.h>
#include <algorithm>
#include "ScreenTable.h"

using namespace ImGui;

namespace AutoPlay {
    void Shoot(double angle, double power);
    bool isAnimationActive();
    void ClearState();
}
extern Candidate g_CurrentCandidate;
extern Point2D   lastFailedCuePos;

namespace AimLock9Ball {

    // ── State / Scan mode ─────────────────────────────────────────────────────
    enum St { ST_IDLE, ST_SCANNING }     state   = ST_IDLE;
    enum Sc { FAST_WIN, SLOW_WIN, SLOW_LEGAL } scan = FAST_WIN;

    bool  s_running    = true;
    bool  s_menuIsOpen = false;
    // Win shot probability 0–100, shown in HUD during scanning
    float s_winProb    = 0.0f;

    // ── Helpers ───────────────────────────────────────────────────────────────

    // Returns the index (1-9) of the lowest-numbered ball still on table.
    // Index 0 = cue ball, index 1 = ball 1, ..., index 9 = ball 9.
    static int getLowestIdx() {
        for (int i = 1; i < gPrediction->guiData.ballsCount; i++)
            if (gPrediction->guiData.balls[i].originalOnTable) return i;
        return -1;
    }

    static void doShoot(double angle, double power) {
        g_CurrentCandidate.angle = angle;
        g_CurrentCandidate.power = power;
        AutoPlay::Shoot(angle, power);
    }

    // Common result check after determineShotResult():
    //   - cue stays on table
    //   - first hit == lowestIdx
    //   - if winOnly: ball 9 must be pocketed
    //   - returns bestPotted (9 = win, >0 = legal, -1 = invalid)
    static int checkResult(int lowestIdx, uint nomPocket, bool winOnly) {
        if (!gPrediction->guiData.balls[0].onTable) return -1;
        auto firstHit = gPrediction->guiData.collision.firstHitBall;
        if (!firstHit || firstHit->index != lowestIdx) return -1;

        int bestPotted = -1;
        for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
            auto& b = gPrediction->guiData.balls[i];
            if (!b.originalOnTable || b.onTable) continue;        // not pocketed
            if (nomPocket < 6 && b.pocketIndex != (int)nomPocket) continue;
            if (i == 9) { bestPotted = 9; break; }                // WIN SHOT — take immediately
            if (!winOnly && bestPotted == -1) bestPotted = i;     // any legal ball (fallback)
        }
        return bestPotted;
    }

    // Commits a found candidate and shoots.
    static void commitAndShoot(int potted, double angle, double power,
                               const Candidate& base) {
        g_CurrentCandidate           = base;
        g_CurrentCandidate.idx       = potted;
        g_CurrentCandidate.angle     = angle;
        g_CurrentCandidate.power     = power;
        g_CurrentCandidate.pocketIndex =
            gPrediction->guiData.balls[potted].pocketIndex;
        doShoot(angle, power);
    }

    // ── FAST WIN SCAN ─────────────────────────────────────────────────────────
    // Ghost-ball geometry for two scenarios:
    //   A) lowestIdx != 9  →  cue → lowestBall → ball9 → pocket  (combo win)
    //   B) lowestIdx == 9  →  cue → ball9 → pocket               (direct win)
    // Small angular bracket applied around each geometric angle for robustness.
    static void ScanFastWin() {
        if (g_CurrentCandidate.idx != -1) return;
        if (gPrediction->guiData.balls[0].initialPosition == lastFailedCuePos) return;

        int lowestIdx = getLowestIdx();
        if (lowestIdx == -1) { scan = SLOW_WIN; return; }

        auto& cueBall    = gPrediction->guiData.balls[0];
        auto& lowestBall = gPrediction->guiData.balls[lowestIdx];
        if (!lowestBall.originalOnTable) { scan = SLOW_WIN; return; }

        auto  pockets   = getPockets();
        uint  nomPocket = sharedGameManager.getNominatedPocket();
        auto  spin      = sharedGameManager.getShotSpin();

        // Angular offsets for robustness (radians)
        static const double offsets[] = {
            0.0, 0.002, -0.002, 0.005, -0.005,
            0.008, -0.008, 0.012, -0.012, 0.018, -0.018
        };

        // ── Scenario A: combo win (lowest → 9-ball → pocket) ─────────────────
        bool have9 = (lowestIdx != 9) &&
                     gPrediction->guiData.ballsCount > 9 &&
                     gPrediction->guiData.balls[9].originalOnTable;

        if (have9) {
            auto& nineBall = gPrediction->guiData.balls[9];
            std::vector<Candidate> comboCands;

            for (int pidx = 0; pidx < (int)pockets.size(); pidx++) {
                if (nomPocket < 6 && pidx != (int)nomPocket) continue;
                Point2D pocket = pockets[pidx];

                // Ghost position for ball 9 → pocket
                Point2D to9   = pocket - nineBall.initialPosition;
                double  d9    = sqrt(to9.square());
                if (d9 < 0.1) continue;
                Point2D dir9  = to9 * (1.0 / d9);
                Point2D ghost9 = nineBall.initialPosition - dir9 * (2.0 * BALL_RADIUS);

                // Ghost position for lowest → ghost9
                Point2D toG9   = ghost9 - lowestBall.initialPosition;
                double  dLG9   = sqrt(toG9.square());
                if (dLG9 < 0.1) continue;
                Point2D dirLG9 = toG9 * (1.0 / dLG9);
                Point2D ghostL = lowestBall.initialPosition - dirLG9 * (2.0 * BALL_RADIUS);

                // Angle cue → ghostL
                Point2D shotVec = ghostL - cueBall.initialPosition;
                double  dCue    = sqrt(shotVec.square());
                if (dCue < 0.1) continue;
                double  angle   = atan2(shotVec.y, shotVec.x);
                if (angle < 0) angle += 2.0 * M_PI;

                // Power: enough for the full chain
                double chain = dCue + dLG9 + d9;
                double power = ImClamp(sqrt(2.0 * 196.0 * chain), 200.0, 866.0);

                // Negative score = highest priority (wins over Set B)
                comboCands.push_back({ lowestIdx, angle, chain - 9999.0, pidx, power });
            }

            std::sort(comboCands.begin(), comboCands.end());

            for (const auto& cand : comboCands) {
                double baseAngle = NumberUtils::normalizeDoublePrecision(
                    normalizeAngle(cand.angle));
                for (double off : offsets) {
                    double a = normalizeAngle(baseAngle + off);
                    gPrediction->determineShotResult(true, a, cand.power, spin, cand);

                    if (checkResult(lowestIdx, nomPocket, /*winOnly=*/true) == 9) {
                        s_winProb = 100.0f;
                        commitAndShoot(9, a, cand.power, cand);
                        return;
                    }
                }
            }
        }

        // ── Scenario B: direct win (lowestIdx == 9 or fallback) ──────────────
        // When lowestIdx == 9, the lowest ball IS ball 9 — just pocket it.
        int directTarget = (lowestIdx == 9) ? 9 : -1;
        if (directTarget == 9) {
            auto& ball9 = gPrediction->guiData.balls[9];
            std::vector<Candidate> directCands;

            for (int pidx = 0; pidx < (int)pockets.size(); pidx++) {
                if (nomPocket < 6 && pidx != (int)nomPocket) continue;
                Point2D pocket  = pockets[pidx];
                Point2D toPkt   = pocket - ball9.initialPosition;
                double  dist    = sqrt(toPkt.square());
                if (dist < 0.1) continue;

                Point2D dir   = toPkt * (1.0 / dist);
                Point2D ghost = ball9.initialPosition - dir * (2.0 * BALL_RADIUS);
                Point2D sv    = ghost - cueBall.initialPosition;
                double  dCue  = sqrt(sv.square());
                if (dCue < 0.1) continue;
                double  angle = atan2(sv.y, sv.x);
                if (angle < 0) angle += 2.0 * M_PI;

                double power = ImClamp(sqrt(2.0 * 196.0 * (dCue + dist)), 150.0, 866.0);
                directCands.push_back({ 9, angle, dCue + dist, pidx, power });
            }

            std::sort(directCands.begin(), directCands.end());

            for (const auto& cand : directCands) {
                double baseAngle = NumberUtils::normalizeDoublePrecision(
                    normalizeAngle(cand.angle));
                for (double off : offsets) {
                    double a = normalizeAngle(baseAngle + off);
                    gPrediction->determineShotResult(true, a, cand.power, spin, cand);

                    // lowestIdx == 9, so checkResult's firstHit check is against ball 9
                    if (checkResult(9, nomPocket, /*winOnly=*/true) == 9) {
                        s_winProb = 100.0f;
                        commitAndShoot(9, a, cand.power, cand);
                        return;
                    }
                }
            }
        }

        // Fast win scan exhausted — move to slow win scan
        s_winProb = 0.0f;
        lastFailedCuePos = cueBall.initialPosition;
        scan = SLOW_WIN;
    }

    // ── SLOW SCAN — brute-force, 12 steps/frame ───────────────────────────────
    // winOnly=true  → only accepts shots that pocket ball 9 (win)
    // winOnly=false → accepts any legal shot on the lowest ball
    static void ScanSlow(bool winOnly) {
        static double   curAngle  = 0.0;
        static bool     scanning  = false;
        static Point2D  lastPos   = { -1000.0, -1000.0 };
        static bool     lastWinOnly = true;

        if (g_CurrentCandidate.idx != -1) return;

        bool posChanged = (gPrediction->guiData.balls[0].initialPosition != lastPos);
        if (!scanning || posChanged || lastWinOnly != winOnly) {
            curAngle   = 0.0;
            scanning   = true;
            lastPos    = gPrediction->guiData.balls[0].initialPosition;
            lastWinOnly = winOnly;
        }

        constexpr double maxAng = 2.0 * M_PI;
        const double     step   = 0.003;
        auto spin      = sharedGameManager.getShotSpin();
        uint nomPocket = sharedGameManager.getNominatedPocket();
        int  lowestIdx = getLowestIdx();
        if (lowestIdx == -1) { scanning = false; state = ST_IDLE; return; }

        // Power ladder: try highest first (more chances for combos)
        static const double POWERS[] = { 866.0, 666.0, 466.0, 266.0, 100.0 };

        int steps = 0;
        while (steps < 12 && curAngle < maxAng) {
            double angle = curAngle;
            curAngle += step;
            steps++;

            // Update win probability: fraction of full circle scanned (inverted for "remaining chance")
            if (winOnly)
                s_winProb = ImClamp((float)((maxAng - curAngle) / maxAng * 80.0), 0.0f, 80.0f);

            for (double power : POWERS) {
                gPrediction->determineShotResult(true, angle, power, spin);

                int potted = checkResult(lowestIdx, nomPocket, winOnly);
                if (potted == -1) continue;

                s_winProb = (potted == 9) ? 100.0f : 50.0f;
                Candidate base = { lowestIdx, angle, 0.0,
                    gPrediction->guiData.balls[potted].pocketIndex, power };
                commitAndShoot(potted, angle, power, base);
                scanning = false;
                return;
            }
        }

        if (curAngle >= maxAng) {
            scanning = false;
            curAngle = 0.0;

            if (winOnly) {
                // No win shot found — try any legal shot
                s_winProb = 0.0f;
                scan = SLOW_LEGAL;
            } else {
                // No legal shot found at all
                state = ST_IDLE;
            }
        }
    }

    // ── Public API ────────────────────────────────────────────────────────────

    void Update() {
        if (!persistent_bool["b9BallAimLock"]) { state = ST_IDLE; return; }
        if (!s_running)                         { state = ST_IDLE; return; }

        Ball::Classification myclass = sharedGameManager.getPlayerClassification();
        if (myclass != Ball::Classification::NINE_BALL_RULE) return;

        if (AutoPlay::isAnimationActive()) return;

        if (!sharedGameManager.mStateManager().isPlayerTurn()) {
            state = ST_IDLE; return;
        }

        if (state == ST_IDLE) {
            state = ST_SCANNING;
            scan  = FAST_WIN;
        } else if (state == ST_SCANNING) {
            switch (scan) {
                case FAST_WIN:   ScanFastWin();          break;
                case SLOW_WIN:   ScanSlow(/*winOnly=*/true);  break;
                case SLOW_LEGAL: ScanSlow(/*winOnly=*/false); break;
            }
        }
    }

    // ── Draw HUD + Play/Pause overlay ─────────────────────────────────────────
    void Draw() {
        if (!persistent_bool["b9BallAimLock"]) return;

        Ball::Classification myclass = sharedGameManager.getPlayerClassification();
        if (myclass != Ball::Classification::NINE_BALL_RULE) return;

        ImGuiIO& io = ImGui::GetIO();

        // ── Play/Pause floating button (bottom-left area) ─────────────────────
        if (!s_menuIsOpen) {
            float r    = 32.0f;
            float padX = 14.0f;
            float padY = io.DisplaySize.y - 14.0f - r * 2.0f;

            SetNextWindowPos(ImVec2(padX, padY), ImGuiCond_Always);
            SetNextWindowSize(ImVec2(r * 2.0f + 48.0f, r * 2.0f), ImGuiCond_Always);
            PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0,0,0,0));
            PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0,0));
            PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            if (Begin("##9BBtn", nullptr,
                      ImGuiWindowFlags_NoTitleBar   | ImGuiWindowFlags_NoResize |
                      ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {

                ImDrawList* dl  = GetWindowDrawList();
                ImVec2 center   = ImVec2(padX + r, padY + r);

                if (InvisibleButton("##9BPlay", ImVec2(r * 2.0f, r * 2.0f))) {
                    s_running = !s_running;
                    if (s_running) {
                        AutoPlay::ClearState();
                        lastFailedCuePos = { -1000.0, -1000.0 };
                        state = ST_IDLE;
                        scan  = FAST_WIN;
                    }
                }
                bool hov = IsItemHovered();

                ImU32 ringCol = s_running ? IM_COL32(0,220,180,220) : IM_COL32(120,120,130,180);
                dl->AddCircleFilled(center, r, IM_COL32(14,18,18,230));
                dl->AddCircle(center, r, ringCol, 0, 2.0f);
                if (hov) dl->AddCircleFilled(center, r, IM_COL32(255,255,255,18));

                if (s_running) {
                    float bH = r * 0.55f, bW = r * 0.18f, gap = r * 0.14f;
                    float lx = center.x - gap * 0.5f - bW;
                    float rx = center.x + gap * 0.5f;
                    float ty = center.y - bH * 0.5f;
                    dl->AddRectFilled(ImVec2(lx, ty), ImVec2(lx+bW, ty+bH), IM_COL32(255,255,255,240));
                    dl->AddRectFilled(ImVec2(rx, ty), ImVec2(rx+bW, ty+bH), IM_COL32(255,255,255,240));
                } else {
                    float hs = r * 0.38f;
                    ImVec2 p0(center.x - hs * 0.5f, center.y - hs);
                    ImVec2 p1(center.x - hs * 0.5f, center.y + hs);
                    ImVec2 p2(center.x + hs,         center.y);
                    dl->AddTriangleFilled(p0, p1, p2, IM_COL32(0,220,180,240));
                }

                const char* lbl = s_running ? "9B ON" : "9B OFF";
                ImVec2 tSz = CalcTextSize(lbl);
                dl->AddText(ImVec2(center.x + r + 4.0f, center.y - tSz.y * 0.5f),
                    s_running ? IM_COL32(0,220,180,255) : IM_COL32(140,140,150,200), lbl);
            }
            End();
            PopStyleVar(2); PopStyleColor();
        }

        // ── Status HUD ────────────────────────────────────────────────────────
        if (s_menuIsOpen || !s_running) return;

        const char* label  = nullptr;
        ImU32       border = 0;
        ImVec4      col;

        // Build label string — scanning states include win probability
        static char s_labelBuf[32];
        bool showProb = false;

        switch (state) {
            case ST_SCANNING:
                switch (scan) {
                    case FAST_WIN:
                        label  = "9B SCAN WIN";
                        col    = ImVec4(1.0f, 0.85f, 0.0f, 1.0f);
                        border = IM_COL32(255, 210, 0, 255);
                        break;
                    case SLOW_WIN:
                        snprintf(s_labelBuf, sizeof(s_labelBuf), "WIN: %.0f%%", s_winProb);
                        label   = s_labelBuf;
                        showProb = true;
                        col    = ImVec4(1.0f, 0.55f, 0.05f, 1.0f);
                        border = IM_COL32(255, 140, 0, 255);
                        break;
                    case SLOW_LEGAL:
                        label  = "9B FALLBACK";
                        col    = ImVec4(0.6f, 0.6f, 0.7f, 1.0f);
                        border = IM_COL32(150, 150, 170, 255);
                        break;
                }
                break;
            case ST_IDLE:
                if (g_CurrentCandidate.idx == 9) {
                    label  = "9B WIN SHOT!";
                    col    = ImVec4(0.10f, 1.0f, 0.55f, 1.0f);
                    border = IM_COL32(0, 255, 140, 255);
                } else if (g_CurrentCandidate.idx != -1) {
                    label  = "9B LOCKED";
                    col    = ImVec4(0.15f, 0.92f, 0.32f, 1.0f);
                    border = IM_COL32(40, 220, 80, 255);
                } else return;
                break;
            default: return;
        }

        float hudY = io.DisplaySize.y - 14.0f - 64.0f - 32.0f - 4.0f;
        SetNextWindowPos(ImVec2(14.0f, hudY), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        PushStyleColor(ImGuiCol_WindowBg, IM_COL32(14,14,18,215));
        PushStyleColor(ImGuiCol_Border,   border);
        PushStyleVar (ImGuiStyleVar_WindowRounding,   8.0f);
        PushStyleVar (ImGuiStyleVar_WindowBorderSize, 1.5f);
        PushStyleVar (ImGuiStyleVar_WindowPadding,    ImVec2(10.0f, 6.0f));
        if (Begin("##9BHud", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                  ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
            TextColored(col, "%s", label);

            // Probability bar (shown during SLOW_WIN scan)
            if (showProb && state == ST_SCANNING) {
                float barW  = 120.0f;
                float barH  = 6.0f;
                ImVec2 bPos = GetCursorScreenPos();
                ImDrawList* dl2 = GetWindowDrawList();
                dl2->AddRectFilled(bPos, ImVec2(bPos.x + barW, bPos.y + barH),
                    IM_COL32(40, 40, 55, 200), 3.0f);
                float fill = ImClamp(s_winProb / 100.0f, 0.0f, 1.0f);
                if (fill > 0.01f) {
                    // Gradient: orange → green as probability rises
                    ImU32 barFill = IM_COL32(
                        (int)(255 - fill * 200),
                        (int)(100 + fill * 155),
                        0, 230);
                    dl2->AddRectFilled(bPos, ImVec2(bPos.x + barW * fill, bPos.y + barH),
                        barFill, 3.0f);
                }
                Dummy(ImVec2(barW, barH + 2.0f));
            }

            if (g_CurrentCandidate.idx != -1 && state == ST_IDLE) {
                SameLine(0, 8.0f);
                PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
                PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(6.0f, 2.0f));
                PushStyleColor(ImGuiCol_Button,        IM_COL32(30,30,42,255));
                PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(50,50,65,255));
                PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(20,20,30,255));
                if (Button("RESCAN")) {
                    AutoPlay::ClearState();
                    lastFailedCuePos = { -1000.0, -1000.0 };
                    state    = ST_IDLE;
                    scan     = FAST_WIN;
                    s_winProb = 0.0f;
                }
                PopStyleColor(3); PopStyleVar(2);
            }
        }
        End();
        PopStyleVar(3); PopStyleColor(2);
    }

} // namespace AimLock9Ball

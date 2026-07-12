#pragma once

#include "Prediction.fast.h"
#include <imgui/imgui.h>
#include <algorithm>

#include "ScreenTable.h"

// #include "PowerSlider.h"
#include "ButtonClicker.h"

using namespace ImGui;

constexpr double maxAngle = 360.0 / (180.0 / M_PI);

double normalizeAngle(double angle) {
    double newAngle = angle;
    if (newAngle >= maxAngle) newAngle = fmod(newAngle, maxAngle);
    else if (newAngle < 0) newAngle = maxAngle - fmod(-newAngle, maxAngle);
    return newAngle;
}

Candidate g_CurrentCandidate = { -1 };

extern void DrawEightBallLoading(ImDrawList*);

ImVec2 GetPocketScreenPos(int pocketIdx) {
    Table table = sharedGameManager.mTable;
    if (!table) return {};

    auto tableProperties = table.mTableProperties();
    if (!tableProperties) return {};

    auto& pockets = tableProperties.mPockets();
    return WorldToScreen(pockets[pocketIdx]);
}

bool IsShotValid() {
    auto& cand = g_CurrentCandidate;
    if (cand.idx == -1) return false;

    Ball::Classification myclass = sharedGameManager.getPlayerClassification();

    uint nominatedPocket = sharedGameManager.getNominatedPocket();
    if (nominatedPocket < 6 && cand.pocketIndex != nominatedPocket) return false;

    if (!gPrediction->guiData.balls[0].onTable) return false; // cue ball should not be pocketed
    if (!gPrediction->guiData.balls[cand.idx].originalOnTable) return false; // target ball was already potted
    if (gPrediction->guiData.balls[cand.idx].onTable) return false; // target ball was not potted
    if (gPrediction->guiData.balls[cand.idx].pocketIndex != cand.pocketIndex) return false; // target ball did not go into target pocket

    auto& ball8 = gPrediction->guiData.balls[8];
    if (myclass == Ball::Classification::ANY && ball8.originalOnTable && !ball8.onTable) return false;

    auto& firstHit = gPrediction->guiData.collision.firstHitBall;
    if (firstHit) {
        if (myclass == Ball::Classification::ANY) {
            if (firstHit->classification == Ball::Classification::EIGHT_BALL) return false;
        } else if (firstHit->classification != myclass) return false;
    }

    return true;
}

/* void UpdatePowerSlider() {
    static bool isSearchingExtraPower = false;
    static float savedLowestPower = 0.0f;
    static ImVec2 savedLowestPos = ImVec2(0, 0);
    static ImVec2 bestValidPos = ImVec2(0, 0);

    if (powerSlider.state != powerSlider.State::MOVING) {
        isSearchingExtraPower = false;
        return;
    }

    float currentPower = sharedGameManager.mVisualCue().getShotPower(true);
    bool isValid = IsShotValid();

    if (!isSearchingExtraPower) {
        if (isValid && currentPower < 666.0f) {
            isSearchingExtraPower = true;
            savedLowestPower = currentPower;
            savedLowestPos = powerSlider.CurrentPos;
            bestValidPos = powerSlider.CurrentPos;
            LOGI("Found valid shot at %f, searching for more power...", currentPower);
        }
    } else {
        if (isValid) bestValidPos = powerSlider.CurrentPos;

        if (!isValid) {
            float midX = bestValidPos.x;// + (powerSlider.CurrentPos.x - savedLowestPos.x) * 0.5f;
            float midY = bestValidPos.y;// + (powerSlider.CurrentPos.y - savedLowestPos.y) * 0.5f;
            
            powerSlider.CurrentPos = ImVec2(midX, midY);
            NativeTouchesMove(powerSlider.TouchIndex, midX, midY);
            
            LOGI("Shot invalid at %f. Backing off and ending.", currentPower);
            powerSlider.state = powerSlider.State::ENDING;
            isSearchingExtraPower = false;
        } else if (currentPower >= savedLowestPower + 80.0f) {
            LOGI("Reached +80 power (%f -> %f). Forcefully ending.", savedLowestPower, currentPower);
            powerSlider.state = powerSlider.State::ENDING;
            isSearchingExtraPower = false;
        }
    }
} */

Point2D lastFailedCuePos = { -1000.0, -1000.0 };
namespace AutoPlay {
    double lastSetAngle = 0.f;
    bool didSetAngle = false;
    bool bAutoPlaying = false;

    enum State {
        IDLE,           // Waiting for player turn or Autoplay to be enabled
        SCANNING,       // Searching for the best shot candidate (calculating physics)
        NOMINATING,     // Waiting for pocket nomination click to finish
        EXECUTING,      // Setting the angle and spin (waiting for visual cue to update)
    } state = IDLE;
    
    double pendingShotPower = 0.f;
    double pendingShotAngle = 0.f;
    int nominationFrameCounter = 0;
    
    enum ScanMode {
        FAST,
        SLOW,
    } scan = FAST;

    bool shouldAutoPlay() { return !didSetAngle || lastSetAngle == sharedGameManager.mVisualCue().mVisualGuide().mAimAngle(); }

    void setAimAngle(double angle) {
        lastSetAngle = angle;
        sharedGameManager.mVisualCue().mVisualGuide().mAimAngle(angle);
    }

    void takeShot(double angle, double power) {
        setAimAngle(angle);
        gPrediction->determineShotResult(false, angle, power);

        sharedGameManager.mVisualCue().mPower(ShotPowerToPower(power));
        M(void, libmain + 0x2dc0c58, void*)(F(void*, sharedGameManager + 0x3b0));
    }
    
    void ClearState() {
        g_CurrentCandidate.idx = -1;
        lastFailedCuePos = { -1000.0, -1000.0 };
    }
    
    void Shoot(double angle, double power = 0.f) {
        setAimAngle(angle);
        gPrediction->determineShotResult(false, angle, power);

        bool nominating = false;
        int nominationMode = sharedGameManager.getPocketNominationMode();
        auto myclass = sharedGameManager.getPlayerClassification();
        if ((nominationMode == 1 && myclass == Ball::Classification::EIGHT_BALL) || (nominationMode == 2 && myclass != Ball::Classification::ANY)) {
            if (g_CurrentCandidate.idx != -1 && sharedGameManager.getNominatedPocket() != g_CurrentCandidate.pocketIndex) {
                nominating = true;
            }
        }

        if (nominating) {
            pendingShotPower = power;
            pendingShotAngle = angle;
            state = NOMINATING;
            nominationFrameCounter = 0;
        } else {
            takeShot(angle, power);
            ClearState();
            state = IDLE;
        }
    }
    
    void ScanSlow(double angleStep = 0.01f) {
        static double currentScanAngle = 0.0;
        static bool isScanning = false;
        static Point2D lastScanCuePos = { -1000.0, -1000.0 };

        if (g_CurrentCandidate.idx != -1) return;
        
        // Reset if we just started or wrapped around, or if table changed
        if (!isScanning || gPrediction->guiData.balls[0].initialPosition != lastScanCuePos) {
            currentScanAngle = 0.0;
            isScanning = true;
            lastScanCuePos = gPrediction->guiData.balls[0].initialPosition;
        }

        Ball::Classification myclass = sharedGameManager.getPlayerClassification();
        uint nominatedPocket = sharedGameManager.getNominatedPocket();
        auto& cueBall = gPrediction->guiData.balls[0];
        
        int steps = 0;
        bool foundShot = false;
        
        // Naikkan dari 10 → 40 steps/frame. Dengan angleStep=0.003 dan 40 steps:
        // ~524 frame (~8 detik di 60fps) untuk full scan. Lebih cepat dari sebelumnya
        // (10 steps = ~2094 frame = ~35 detik) tanpa kehilangan akurasi.
        while (steps < 40 && currentScanAngle < maxAngle) {
            double angle = currentScanAngle;
            currentScanAngle += angleStep;
            steps++;

            std::vector<double> powers = {666.0, 466.0, 266.0, 100.0};
            for (double power : powers) {
                // WAJIB forceFullSimulation=true supaya scratch terdeteksi benar
                gPrediction->forceFullSimulation = true;
                gPrediction->determineShotResult(true, angle, power, sharedGameManager.getShotSpin());
                gPrediction->forceFullSimulation = false;
                
                // Cek scratch PERTAMA
                if (!gPrediction->guiData.balls[0].onTable) continue;

                // Safety margin pocket
                {
                    bool tooClose = false;
                    auto& cbf = gPrediction->guiData.balls[0];
                    for (const auto& pocket : getPockets()) {
                        double dx = cbf.predictedPosition.x - pocket.x;
                        double dy = cbf.predictedPosition.y - pocket.y;
                        if (dx*dx + dy*dy < 144.0) { tooClose = true; break; }
                    }
                    if (tooClose) continue;
                }
                
                bool isPotentiallyValid = false;
                int targetIdx = -1;

                bool bFoundLowestNumberedBall = false;
                int iFoundLowestNumberedBall = -1;
                bool isNineBallGame = myclass == Ball::Classification::NINE_BALL_RULE;

                if (isNineBallGame) {
                    for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                        auto& ball = gPrediction->guiData.balls[i];
                        if (!ball.originalOnTable) continue; // skip already potted

                        bFoundLowestNumberedBall = true;
                        iFoundLowestNumberedBall = i;
                        break;
                    }

                    auto firstHit = gPrediction->guiData.collision.firstHitBall;
                    if (!firstHit) continue;
                    
                    // Must hit lowest numbered ball first
                    if (firstHit->index != iFoundLowestNumberedBall) continue;

                    // Cue ball must stay on table
                    if (!gPrediction->guiData.balls[0].onTable) continue;

                    int bestPottedIdx = -1;
                    for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                        auto& ball = gPrediction->guiData.balls[i];
                        if (ball.originalOnTable && !ball.onTable) {
                            if (nominatedPocket < 6 && ball.pocketIndex != nominatedPocket) continue;
                            
                            if (i == 9) { bestPottedIdx = 9; break; }
                            if (bestPottedIdx == -1 || i == firstHit->index) bestPottedIdx = i;
                        }
                    }

                    if (bestPottedIdx == -1) continue;
                    targetIdx = bestPottedIdx;

                    LOGI("AutoPlay: 9ball: Found good angle %f with power %f", angle, power);
                    
                    g_CurrentCandidate.idx = targetIdx;
                    g_CurrentCandidate.angle = angle;
                    g_CurrentCandidate.power = power;
                    g_CurrentCandidate.pocketIndex = gPrediction->guiData.balls[targetIdx].pocketIndex;

                    foundShot = true;
                    Shoot(angle, power);
                    break;
                }

                // Check if ANY valid ball is potted
                for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                    auto& ball = gPrediction->guiData.balls[i];
                    if (ball.originalOnTable && !ball.onTable) { // Ball was potted
                        bool isValidTarget = false;
                        // Logic for valid target (Simplified)
                        // If table is open (ANY), any ball except 8-ball and Cue-ball is valid.
                        // If class is assigned, only that class is valid.
                        // Note: If on 8-ball, usually class logic might differ, but assuming standard flow:
                        
                        if (myclass == Ball::Classification::ANY) {
                            if (ball.classification != Ball::Classification::CUE_BALL && ball.classification != Ball::Classification::EIGHT_BALL) isValidTarget = true;
                        } else {
                            if (ball.classification == myclass) isValidTarget = true;
                        }
                        
                        if (nominatedPocket < 6 && ball.pocketIndex != nominatedPocket) isValidTarget = false;

                        if (isValidTarget) {
                            targetIdx = i;
                            break; // Found at least one valid potted ball
                        }
                    }
                }

                if (targetIdx != -1) {
                    if (!gPrediction->guiData.balls[0].onTable) continue;
                    if (!gPrediction->guiData.balls[8].onTable && myclass != Ball::Classification::EIGHT_BALL) continue;

                    auto firstHit = gPrediction->guiData.collision.firstHitBall;
                    if (!firstHit) continue;
                    
                    if (myclass == Ball::Classification::ANY) {
                        if (firstHit->classification == Ball::Classification::EIGHT_BALL) continue;
                    } else if (firstHit->classification != myclass) continue;

                    isPotentiallyValid = true;
                    // Store candidate info
                    g_CurrentCandidate.idx = targetIdx;
                    g_CurrentCandidate.angle = angle;
                    g_CurrentCandidate.power = power;
                    g_CurrentCandidate.pocketIndex = gPrediction->guiData.balls[targetIdx].pocketIndex;
                }

                if (isPotentiallyValid) {
                    LOGI("AutoPlaySlow: Found shot at angle %f power %f", angle, power);
                    foundShot = true;
                    Shoot(angle, power);
                    // Do not reset scanning here, so we can resume if this shot fails
                    break;
                }
            }

            if (foundShot) break;
        }

        if (!foundShot && currentScanAngle >= maxAngle) {
            LOGI("AutoPlaySlow: Finished scan, nothing found.");
            isScanning = false;
            currentScanAngle = 0.0;
            state = IDLE;
        }
    }
    
    void ScanFast(double angleStep = 0.1f) {
        if (g_CurrentCandidate.idx != -1) return;
        if (gPrediction->guiData.balls[0].initialPosition == lastFailedCuePos) return;

        double startingAngle = sharedGameManager.mVisualCue().mVisualGuide().mAimAngle();
        
        gPrediction->determineShotResult(true, startingAngle);
        std::vector<int> startingPottedBalls;
        for (int i = 0; i < gPrediction->guiData.ballsCount; i++) {
            Prediction::Ball& ball = gPrediction->guiData.balls[i];
            if (ball.originalOnTable && !ball.onTable) {
                startingPottedBalls.push_back(i);
            }
        }
        
        Ball::Classification myclass = sharedGameManager.getPlayerClassification();
        uint nominatedPocket = sharedGameManager.getNominatedPocket();
        
        std::vector<Candidate> candidates;
        
        auto pockets = getPockets();
        auto& cueBall = gPrediction->guiData.balls[0];
        
        // Identify candidate shots
        bool bFoundLowestNumberedBall = false;
        int iFoundLowestNumberedBall = -1;
        bool isNineBallGame = myclass == Ball::Classification::NINE_BALL_RULE;
        for (int i = 1; i < gPrediction->guiData.ballsCount; i++) { // ball[0](cue ball) never a candidate
            if (isNineBallGame && bFoundLowestNumberedBall) break;

            auto& ball = gPrediction->guiData.balls[i];
            if (!ball.originalOnTable) continue;

            if (!bFoundLowestNumberedBall) {
                bFoundLowestNumberedBall = true;
                iFoundLowestNumberedBall = i;
            }

            if (!isNineBallGame) {
                bool isACandidate = myclass == Ball::Classification::ANY ? ball.classification != Ball::Classification::EIGHT_BALL : ball.classification == myclass;
                if (!isACandidate) continue;
            }

            for (int pocketIdx = 0; pocketIdx < pockets.size(); pocketIdx++) {
                if (nominatedPocket < 6 && pocketIdx != nominatedPocket) continue;

                Point2D pocket = pockets[pocketIdx];
                Point2D toPocket = pocket - ball.initialPosition;
                double distTargetToPocket = sqrt(toPocket.square());
                if (distTargetToPocket < 0.1) continue;
                
                Point2D direction = toPocket * (1.0 / distTargetToPocket);
                Point2D ghostBallPos = ball.initialPosition - direction * (2.0 * BALL_RADIUS);
                Point2D shotLine = ghostBallPos - cueBall.initialPosition;
                double distCueToTarget = sqrt(shotLine.square());
                double angle = atan2(shotLine.y, shotLine.x);
                
                if (angle < 0) angle += 2 * M_PI;
                
                double score = distCueToTarget + distTargetToPocket;
                
                // Power formula: v = sqrt(2 * a * s) dengan a=196 (sliding deceleration).
                // Tapi ini asumsi energy konservasi penuh. Di handleBallBallCollision,
                // hanya komponen normal yang ditransfer — komponen tangensial hilang.
                // Faktor koreksi: 1.0 + sin²(cutAngle) * 0.4 untuk kompensasi energy loss.
                // Untuk simplifikasi: pakai faktor 1.3 flat (equivalent ~45° cut average).
                constexpr double slidingDeceleration = 196.0;
                double requiredVelocity = sqrt(2.0 * slidingDeceleration * score) * 1.3;
                
                double power = requiredVelocity;
                if (power > 666.0) power = 666.0;
                if (power < 80.0) power = 80.0;
                
                candidates.push_back({i, angle, score, pocketIdx, power});
            }
        }
        
        std::sort(candidates.begin(), candidates.end());
        
        // Angle offsets untuk kompensasi ghost ball inaccuracy (±1°, ±2°)
        static const double kDeltaAngles[] = {0.0, -0.0175, +0.0175, -0.035, +0.035};
        // Power factors: base dulu, lalu naik (shot jauh), lalu turun (shot dekat)
        static const double kPowerFactors[] = {1.0, 1.15, 0.85, 1.3, 0.7};

        bool foundShot = false;
        for (const auto& cand : candidates) {
            double baseAngle = NumberUtils::normalizeDoublePrecision(normalizeAngle(cand.angle));

            bool simOk = false;
            double confirmedAngle = baseAngle;
            double confirmedPower = cand.power;

            // WAJIB forceFullSimulation=true: simulasi berjalan penuh sampai semua bola
            // berhenti → scratch dan 8ball masuk terdeteksi benar.
            // Tanpa ini: early-return saat first hit → cue ball belum selesai bergerak
            // → balls[0].onTable=true walau sebenarnya scratch → shot lolos.
            gPrediction->forceFullSimulation = true;
            for (double dA : kDeltaAngles) {
                if (simOk) break;
                double tryAngle = NumberUtils::normalizeDoublePrecision(normalizeAngle(baseAngle + dA));
                for (double pf : kPowerFactors) {
                    double tryPower = std::min(std::max(cand.power * pf, 80.0), 666.0);
                    gPrediction->determineShotResult(true, tryAngle, tryPower, sharedGameManager.getShotSpin(), cand);

                    // Cek scratch PERTAMA
                    if (!gPrediction->guiData.balls[0].onTable) continue;

                    // Safety margin: tolak kalau cue ball akhir terlalu dekat pocket.
                    // POCKET_RADIUS=8.0, BALL_RADIUS=3.800475.
                    // Threshold = (POCKET_RADIUS * 1.5)^2 = ~144 — cover suction zone game.
                    {
                        bool tooClose = false;
                        auto& cbf = gPrediction->guiData.balls[0];
                        for (const auto& pocket : getPockets()) {
                            double dx = cbf.predictedPosition.x - pocket.x;
                            double dy = cbf.predictedPosition.y - pocket.y;
                            if (dx*dx + dy*dy < 144.0) { tooClose = true; break; }
                        }
                        if (tooClose) continue;
                    }

                    if (!gPrediction->firstHitIsTarget) continue;

                    // Nine ball path
                    if (myclass == Ball::Classification::NINE_BALL_RULE) {
                        auto firstHit = gPrediction->guiData.collision.firstHitBall;
                        if (!firstHit || firstHit->index != cand.idx) continue;

                        int bestPottedIdx = -1;
                        for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                            auto& ball = gPrediction->guiData.balls[i];
                            if (ball.originalOnTable && !ball.onTable) {
                                if (nominatedPocket < 6 && ball.pocketIndex != nominatedPocket) continue;
                                if (i == 9) { bestPottedIdx = 9; break; }
                                if (bestPottedIdx == -1 || i == cand.idx) bestPottedIdx = i;
                            }
                        }
                        if (bestPottedIdx == -1) continue;

                        confirmedAngle = tryAngle;
                        confirmedPower = tryPower;
                        simOk = true;

                        gPrediction->forceFullSimulation = false;
                        g_CurrentCandidate = cand;
                        g_CurrentCandidate.idx = bestPottedIdx;
                        g_CurrentCandidate.angle = confirmedAngle;
                        g_CurrentCandidate.power = confirmedPower;
                        g_CurrentCandidate.pocketIndex = gPrediction->guiData.balls[bestPottedIdx].pocketIndex;
                        foundShot = true;
                        Shoot(confirmedAngle, confirmedPower);
                        goto fastScanDone;
                    }

                    // 8ball / regular path
                    if (gPrediction->guiData.balls[cand.idx].onTable) continue;
                    if (gPrediction->guiData.balls[cand.idx].pocketIndex != cand.pocketIndex) continue;

                    {
                        bool isAngleGood = false;
                        for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                            auto& ball = gPrediction->guiData.balls[i];
                            bool match = (myclass == Ball::Classification::ANY)
                                ? (ball.classification != Ball::Classification::CUE_BALL && ball.classification != Ball::Classification::EIGHT_BALL)
                                : (ball.classification == myclass);
                            if (match && ball.originalOnTable && !ball.onTable) { isAngleGood = true; break; }
                        }

                        if (isAngleGood && gPrediction->guiData.collision.firstHitBall) {
                            auto fh = gPrediction->guiData.collision.firstHitBall;
                            if (myclass != Ball::Classification::ANY && fh->classification != myclass) isAngleGood = false;
                            else if (myclass == Ball::Classification::ANY && fh->classification == Ball::Classification::EIGHT_BALL) isAngleGood = false;
                        }

                        // 8ball masuk prematur
                        auto& ball8 = gPrediction->guiData.balls[8];
                        if (isAngleGood && ball8.originalOnTable && !ball8.onTable && myclass != Ball::Classification::EIGHT_BALL) isAngleGood = false;

                        if (!isAngleGood) continue;
                    }

                    confirmedAngle = tryAngle;
                    confirmedPower = tryPower;
                    simOk = true;
                    break;
                }
            }
            gPrediction->forceFullSimulation = false;
            if (!simOk) continue;

            LOGI("AutoPlay: FAST - Ball %d angle %f power %f", cand.idx, confirmedAngle, confirmedPower);
            g_CurrentCandidate = cand;
            g_CurrentCandidate.angle = confirmedAngle;
            g_CurrentCandidate.power = confirmedPower;
            foundShot = true;
            Shoot(confirmedAngle, confirmedPower);
            break;
        }
        fastScanDone:

        if (!foundShot) {
            lastFailedCuePos = cueBall.initialPosition;
            LOGI("AutoPlay: No good angle found after smart scan.");
            scan = SLOW;
        }
    }

    void DrawToggleButton() {
        ImGuiIO& io = GetIO();
        float padding = 30.0f;
        int buttons = 1;
        float button_size = ImGui::GetFrameHeight() * 2.3f;
        float windowWidth = button_size * buttons + (buttons > 1 ? GetStyle().ItemSpacing.x * (buttons - 1) : 0) + GetStyle().WindowPadding.x * 2;
        float windowHeight = button_size + GetStyle().WindowPadding.y * 2;

        SetNextWindowPos(ImVec2(io.DisplaySize.x - 35 - windowWidth, io.DisplaySize.y - 20 - windowHeight), ImGuiCond_Always);
        SetNextWindowPos(ImVec2(io.DisplaySize.x - 155 - windowWidth, io.DisplaySize.y - 20 - windowHeight), ImGuiCond_Always);
        SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_Always);
        
        PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
        PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
        PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);
        
        if (Begin("AutoPlay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
            auto DrawPlayPauseButton = [&](bool isPause) -> bool {
                ImVec2 pos = GetCursorScreenPos();
                ImVec2 size(button_size, button_size);
                ImVec2 end(pos.x + size.x, pos.y + size.y);
                ImVec2 center(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
                
                // Since we are in a window with the bg set, we can just use standard button with colors
                PushStyleColor(ImGuiCol_Button, IM_COL32(50, 50, 50, 180));
                PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 80, 80, 200));
                PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(100, 100, 100, 200));
                PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
                
                bool clicked = Button("##AutoPlayBtn", size);
                
                ImDrawList* dl = GetWindowDrawList();
                float h = size.y * 0.4f;
                float w = h * 0.8f;

                if (isPause) {
                    float bar_w = w * 0.35f;
                    float gap = w * 0.3f;
                    dl->AddRectFilled(ImVec2(center.x - gap/2 - bar_w, center.y - h/2), ImVec2(center.x - gap/2, center.y + h/2), IM_COL32(255, 255, 255, 180));
                    dl->AddRectFilled(ImVec2(center.x + gap/2, center.y - h/2), ImVec2(center.x + gap/2 + bar_w, center.y + h/2), IM_COL32(255, 255, 255, 180));
                } else {
                    float off_x = h * 0.3f;
                    dl->AddTriangleFilled(ImVec2(center.x - off_x, center.y - h/2), ImVec2(center.x - off_x, center.y + h/2), ImVec2(center.x + off_x * 1.5f, center.y), IM_COL32(255, 255, 255, 180));
                }
                
                GetForegroundDrawList()->AddRect(pos, end, IM_COL32(200, 200, 200, 255), 5.0f, 0, 2.0f);
                
                PopStyleColor(4);
                return clicked;
            };

            if (DrawPlayPauseButton(bAutoPlaying)) {
                bAutoPlaying = !bAutoPlaying;
                if (bAutoPlaying) ClearState();
                // if (!bAutoPlaying && powerSlider.Active) powerSlider.Cancel();
            }
        } End();

        PopStyleVar();
        PopStyleColor(2);
    }

    bool isAnimationActive() {
        auto visualCue = sharedGameManager.mVisualCue();
        if (!visualCue) return true;
        
        auto _powerBarView = F(ptr, visualCue + 0x510);
        if (!_powerBarView) return true;

        auto activeAction = M(ptr, libmain + 0x2de6f30, ptr)(_powerBarView); // CCAction getActiveAction
        if (activeAction) {
            // auto tag = F(uint, activeAction + 0x18); // 668 hiding 667 showing
            // LOGI("tag %u %d %p", tag, tag, tag);
            return true;
        }

        return false;
    }
    
    static double g_turnStartTime = 0.0;
    static bool g_turnTimerStarted = false;

    void Update() {
        buttonClicker.Update();
        DrawToggleButton();

        // Animation timeout: paksa lanjut setelah 1.5 detik supaya tidak stuck
        if (!g_turnTimerStarted) { g_turnStartTime = ImGui::GetTime(); g_turnTimerStarted = true; }
        bool animStuck = (ImGui::GetTime() - g_turnStartTime > 1.5);
        if (isAnimationActive() && !animStuck) return;

        if (!bAutoPlaying || !sharedGameManager.mStateManager().isPlayerTurn()) {
            // Reset state saat giliran berakhir — jangan warisi kandidat dari turn lama
            g_CurrentCandidate.idx = -1;
            lastFailedCuePos = { -1000.0, -1000.0 };
            state = IDLE;
            scan = FAST;
            g_turnTimerStarted = false;
            return;
        }

        if (state == IDLE) {
            state = SCANNING;
            scan = FAST;
        } if (state == SCANNING) {
            if (scan == FAST) ScanFast();
            if (scan == SLOW) {
                DrawEightBallLoading(GetForegroundDrawList());
                ScanSlow(0.003f);
            }
        } if (state == NOMINATING) {
            nominationFrameCounter++;
            if (nominationFrameCounter == 10) {
                buttonClicker.Click(GetPocketScreenPos(g_CurrentCandidate.pocketIndex));
            }
            if (nominationFrameCounter > 20 && !buttonClicker.Active) {
                takeShot(pendingShotAngle, pendingShotPower);
                ClearState();
                state = IDLE;
            }
        }

        /* if (bAutoPlaying && sharedGameManager.mStateManager().isPlayerTurn()) {
            if (powerSlider.Active) {
                UpdateTouchSimulation();
                powerSlider.Update();
            } else Start();
        } */

        // if (!bAutoPlaying && powerSlider.Active) powerSlider.Update(); // for TestAutoPlay
    }
};

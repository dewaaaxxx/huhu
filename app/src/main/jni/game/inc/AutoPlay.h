#pragma once

#include "Prediction.fast.h"
#include <imgui/imgui.h>
#include <algorithm>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include "ScreenTable.h"
#include "mod/ButtonClicker.h"

using namespace ImGui;

constexpr double maxAngle = 360.0 / (180.0 / M_PI);

double normalizeAngle(double angle) {
    double newAngle = angle;
    if (newAngle >= maxAngle) newAngle = fmod(newAngle, maxAngle);
    else if (newAngle < 0) newAngle = maxAngle - fmod(-newAngle, maxAngle);
    return newAngle;
}

Candidate g_CurrentCandidate = { -1 };

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

    if (!gPrediction->guiData.balls[0].onTable) return false;
    if (!gPrediction->guiData.balls[cand.idx].originalOnTable) return false;
    if (gPrediction->guiData.balls[cand.idx].onTable) return false;
    if (gPrediction->guiData.balls[cand.idx].pocketIndex != cand.pocketIndex) return false;

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

Point2D lastFailedCuePos = { -1000.0, -1000.0 };

namespace AutoPlay {
    double lastSetAngle = 0.f;
    bool didSetAngle = false;
    bool bAutoPlaying = false;

    enum State {
        IDLE,
        SCANNING,
        WAITING,
        NOMINATING,
        EXECUTING,
    } state = IDLE;
    
    double pendingShotPower = 0.f;
    double pendingShotAngle = 0.f;
    int nominationFrameCounter = 0;

    double shotDelayTimer      = 0.0;
    double pendingExecuteAngle = 0.0;
    double pendingExecutePower = 0.0;

    // SAFE MODE: smooth aim — aim glides to target instead of snapping
    double aimSmoothTarget  = 0.0;
    double aimSmoothCurrent = 0.0;
    bool   aimSmoothActive  = false;
    
    enum ScanMode {
        FAST,
        SLOW,
    } scan = FAST;

    // ── Background thread infrastructure ────────────────────────────────────────
    struct ScanResult {
        bool found = false;
        double angle = 0.0;
        double power = 0.0;
        Candidate candidate = {-1};
    };
    static inline std::atomic<bool> g_threadRunning{false};
    static inline std::atomic<bool> g_threadShouldStop{false};
    static inline std::atomic<bool> g_threadDone{false};
    static inline ScanResult        g_threadResult{};
    static inline std::thread       g_scanThread{};
    static inline bool               g_autoPlayCalculating{false};

    // Capture posisi semua bola ke snapshot (1x per giliran)
    void TakeSnapshot() {
        Table table = sharedGameManager.mTable;
        if (!table) return;
        auto& balls = table.mBalls();
        if (!balls) return;

        g_sceneSnapshot.ballsCount = balls.Count;
        for (int i = 0; i < g_sceneSnapshot.ballsCount && i < MAX_BALLS_COUNT; i++) {
            auto& dst = g_sceneSnapshot.balls[i];
            dst.index           = i;
            dst.state           = balls[i].state();
            dst.originalOnTable = balls[i].isOnTable();
            dst.classification  = balls[i].classification();
            dst.position        = balls[i].position();
        }
        g_sceneSnapshot.tableBounds = table.mTableCollisionBounds();
          // FIX-4c: Capture real pocket positions (different table types have different pockets)
          auto tableProps__ = table.mTableProperties();
          if (tableProps__) {
              auto& pks = tableProps__.mPockets();
              for (int i = 0; i < TABLE_POCKETS_COUNT; i++) g_sceneSnapshot.pockets[i] = pks[i];
              g_sceneSnapshot.hasPockets = true;
          } else { g_sceneSnapshot.hasPockets = false; }
          g_sceneSnapshot.valid = true;
    }

    // Stop dan bersihkan thread yang sedang berjalan
    void StopScanThread() {
        if (g_threadRunning.load(std::memory_order_relaxed)) {
            g_threadShouldStop.store(true, std::memory_order_relaxed);
        }
        if (g_scanThread.joinable()) g_scanThread.join();
        g_threadRunning.store(false,  std::memory_order_relaxed);
        g_threadShouldStop.store(false, std::memory_order_relaxed);
        g_threadDone.store(false,     std::memory_order_relaxed);
        g_threadResult.found = false;
        g_sceneSnapshot.valid = false;
    }

    // ── 2-Phase Background Scan Thread ──────────────────────────────────────────
    // Phase 1: scan kasar (step 0.05, 2 power) → temukan ~80% tembakan dalam <0.1 detik
    // Phase 2: scan halus (step 0.015, 4 power) → temukan sisa (bank shots, klaster)
    static void ScanThreadFunc(Ball::Classification myclass, uint nominatedPocket,
                               Vec2d spin, int strat) {
        g_useSnapshot = true;  // thread_local: hanya berlaku di thread ini

        Prediction threadPred;  // instance prediction TERPISAH, tidak ganggu main thread
        bool isNineBall = (myclass == Ball::Classification::NINE_BALL_RULE);

        constexpr double coarsePowers[] = {466.0, 666.0};
        constexpr double finePowers[]   = {666.0, 466.0, 266.0, 100.0};

        struct Phase { double step; const double* powers; int count; };
        const Phase phases[] = {
            { 0.05,  coarsePowers, 2 },
            { 0.015, finePowers,   4 }
        };

        for (const auto& ph : phases) {
            if (g_threadShouldStop.load(std::memory_order_relaxed)) return;

            for (double angle = 0.0; angle < maxAngle; angle += ph.step) {
                if (g_threadShouldStop.load(std::memory_order_relaxed)) return;

                // SAFE: micro-sleep every angle step to throttle CPU & prevent heat
                std::this_thread::sleep_for(std::chrono::microseconds(300));
                for (int pi = 0; pi < ph.count; pi++) {
                    double power = ph.powers[pi];
                    threadPred.determineShotResult(true, angle, power, spin);

                    auto* firstHit = threadPred.guiData.collision.firstHitBall;
                    if (!firstHit) continue;
                    if (!threadPred.guiData.balls[0].onTable) continue;

                    if (isNineBall) {
                        int lowestIdx = -1;
                        for (int i = 1; i < threadPred.guiData.ballsCount; i++) {
                            if (threadPred.guiData.balls[i].originalOnTable) { lowestIdx = i; break; }
                        }
                        if (firstHit->index != lowestIdx) continue;

                        int bestPotted = -1;
                        for (int i = 1; i < threadPred.guiData.ballsCount; i++) {
                            auto& b = threadPred.guiData.balls[i];
                            if (!b.originalOnTable || b.onTable) continue;
                            if (nominatedPocket < 6 && (uint)b.pocketIndex != nominatedPocket) continue;
                            if (i == 9) { bestPotted = 9; break; }
                            if (bestPotted == -1 || i == firstHit->index) bestPotted = i;
                        }
                        if (bestPotted == -1) continue;
                        if (strat == 1 && bestPotted != 9) continue;

                        ScanResult r;
                        r.found = true; r.angle = angle; r.power = power;
                        r.candidate = { bestPotted, angle, 0.0,
                                        threadPred.guiData.balls[bestPotted].pocketIndex, power };
                        g_threadResult = r;
                        g_threadDone.store(true, std::memory_order_release);
                        g_threadRunning.store(false, std::memory_order_release);
                        return;
                    }

                    // ── 8-ball validation ──
                    if (myclass == Ball::Classification::ANY) {
                        if (firstHit->classification == Ball::Classification::EIGHT_BALL) continue;
                    } else {
                        if (firstHit->classification != myclass) continue;
                    }

                    auto& ball8 = threadPred.guiData.balls[8];
                    if (myclass != Ball::Classification::EIGHT_BALL
                        && ball8.originalOnTable && !ball8.onTable) continue;

                    int targetIdx = -1;
                    for (int i = 1; i < threadPred.guiData.ballsCount; i++) {
                        auto& b = threadPred.guiData.balls[i];
                        if (!b.originalOnTable || b.onTable) continue;
                        bool match = (myclass == Ball::Classification::ANY)
                            ? (b.classification != Ball::Classification::CUE_BALL
                               && b.classification != Ball::Classification::EIGHT_BALL)
                            : (b.classification == myclass);
                        if (!match) continue;
                        if (nominatedPocket < 6 && (uint)b.pocketIndex != nominatedPocket) continue;
                        targetIdx = i; break;
                    }
                    if (targetIdx == -1) continue;
                    if (strat == 1 && myclass == Ball::Classification::EIGHT_BALL
                        && targetIdx != 8) continue;

                    ScanResult r;
                    r.found = true; r.angle = angle; r.power = power;
                    r.candidate = { targetIdx, angle, 0.0,
                                    threadPred.guiData.balls[targetIdx].pocketIndex, power };
                    g_threadResult = r;
                    g_threadDone.store(true, std::memory_order_release);
                    g_threadRunning.store(false, std::memory_order_release);
                    return;
                }
            }
        }

        // Tidak ada tembakan ditemukan
        g_threadDone.store(true, std::memory_order_release);
        g_threadRunning.store(false, std::memory_order_release);
    }
    // ────────────────────────────────────────────────────────────────────────────

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
        double scannedPower = power; // validated scan power
        // Apply manual power override
        if (persistent_bool["bManualPower"]) {
            double gamePower = sharedGameManager.mVisualCue().mPower();
            power = gamePower * 666.0;
            if (power < 50.0)  power = 50.0;
            if (power > 666.0) power = 666.0;
        } else if (persistent_bool["bAutoPlayFixedPower"]) {
            float pct = persistent_float["fAutoPlayPower"];
            if (pct < 0.0f) pct = 0.0f;
            if (pct > 1.0f) pct = 1.0f;
            power = 50.0 + (double)pct * (666.0 - 50.0);
        }

        // FIX-1: Re-validate with overridden power; if invalid, revert to scan power
        if (power != scannedPower && g_CurrentCandidate.idx != -1) {
            gPrediction->determineShotResult(true, angle, power,
                sharedGameManager.getShotSpin(), g_CurrentCandidate);
            if (!IsShotValid()) { power = scannedPower; }
        }
        // SAFE: ±2% random power jitter — human hands are never perfectly consistent
        { double j = 1.0 + ((double)(rand() % 40) - 20.0) / 1000.0; double jp = power * j; if (jp >= 50.0 && jp <= 1000.0) power = jp; }
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

    void PrepareShot(double angle, double power) {
        float minD = persistent_float["fAutoPlayShotDelay"];
        float maxD = persistent_float["fAutoPlayShotDelayMax"];
        // SAFE: default to human-like 0.7–1.9s delay if not configured
        if (minD < 0.1f) minD = 0.7f;
        if (maxD < minD + 0.3f) maxD = minD + 1.2f;

        float delay;
        if (maxD < 0.05f) {
            delay = 0.0f;
        } else if (maxD <= minD + 0.001f) {
            delay = minD;
        } else {
            float r = (float)(rand() % 10000) / 10000.0f;
            delay = minD + r * (maxD - minD);
        }

        if (delay > 0.0f) {
            pendingExecuteAngle = angle;
            pendingExecutePower = power;
            shotDelayTimer = (double)delay;
            // SAFE: start smooth aim glide toward target angle
            aimSmoothCurrent = sharedGameManager.mVisualCue().mVisualGuide().mAimAngle();
            aimSmoothTarget  = angle;
            aimSmoothActive  = true;
            state = WAITING;
        } else {
            Shoot(angle, power);
        }
    }
    
    void ScanSlow(double angleStep = 0.01f) {
        static double currentScanAngle = 0.0;
        static bool isScanning = false;
        static Point2D lastScanCuePos = { -1000.0, -1000.0 };

        if (g_CurrentCandidate.idx != -1) return;
        
        if (!isScanning || gPrediction->guiData.balls[0].initialPosition != lastScanCuePos) {
            currentScanAngle = 0.0;
            isScanning = true;
            lastScanCuePos = gPrediction->guiData.balls[0].initialPosition;
        }

        Ball::Classification myclass = sharedGameManager.getPlayerClassification();
        uint nominatedPocket = sharedGameManager.getNominatedPocket();
        
        int steps = 0;
        bool foundShot = false;
        
        while (steps < 30 && currentScanAngle < maxAngle) {
            double angle = currentScanAngle;
            currentScanAngle += angleStep;
            steps++;

            constexpr double powers[] = {666.0, 466.0, 266.0, 100.0};
            for (double power : powers) {
                gPrediction->determineShotResult(true, angle, power, sharedGameManager.getShotSpin());
                
                bool isPotentiallyValid = false;
                int targetIdx = -1;
                bool isNineBallGame = myclass == Ball::Classification::NINE_BALL_RULE;

                if (isNineBallGame) {
                    int iFoundLowestNumberedBall = -1;
                    for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                        if (gPrediction->guiData.balls[i].originalOnTable) {
                            iFoundLowestNumberedBall = i;
                            break;
                        }
                    }

                    auto firstHit = gPrediction->guiData.collision.firstHitBall;
                    if (!firstHit || firstHit->index != iFoundLowestNumberedBall) continue;
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
                    g_CurrentCandidate.idx = bestPottedIdx;
                    g_CurrentCandidate.angle = angle;
                    g_CurrentCandidate.power = power;
                    g_CurrentCandidate.pocketIndex = gPrediction->guiData.balls[bestPottedIdx].pocketIndex;

                    foundShot = true;
                    PrepareShot(angle, power);
                    break;
                }

                for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                    auto& ball = gPrediction->guiData.balls[i];
                    if (ball.originalOnTable && !ball.onTable) {
                        bool isValidTarget = false;
                        if (myclass == Ball::Classification::ANY) {
                            if (ball.classification != Ball::Classification::CUE_BALL && ball.classification != Ball::Classification::EIGHT_BALL) isValidTarget = true;
                        } else {
                            if (ball.classification == myclass) isValidTarget = true;
                        }
                        if (nominatedPocket < 6 && ball.pocketIndex != nominatedPocket) isValidTarget = false;
                        if (isValidTarget) { targetIdx = i; break; }
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
                    g_CurrentCandidate.idx = targetIdx;
                    g_CurrentCandidate.angle = angle;
                    g_CurrentCandidate.power = power;
                    g_CurrentCandidate.pocketIndex = gPrediction->guiData.balls[targetIdx].pocketIndex;
                }

                if (isPotentiallyValid) {
                    foundShot = true;
                    PrepareShot(angle, power);
                    break;
                }
            }
            if (foundShot) break;
        }

        if (!foundShot && currentScanAngle >= maxAngle) {
            isScanning = false;
            currentScanAngle = 0.0;
            state = IDLE;
        }
    }
    
    void ScanFast(double angleStep = 0.1f) {
        if (g_CurrentCandidate.idx != -1) return;
        if (gPrediction->guiData.balls[0].initialPosition == lastFailedCuePos) return;

        static std::vector<Candidate> fastCandidates;
        static int fastCandidateOffset = 0;
        static Point2D lastFastCuePos = { -1000.0, -1000.0 };
        static Ball::Classification lastFastClass = Ball::Classification::ERR_CLASSIFICATION;

        auto& cueBall = gPrediction->guiData.balls[0];
        Ball::Classification myclass = sharedGameManager.getPlayerClassification();
        uint nominatedPocket = sharedGameManager.getNominatedPocket();
        bool isNineBallGame = myclass == Ball::Classification::NINE_BALL_RULE;

        // Rebuild candidate list only when cue ball moves or class changes
        if (cueBall.initialPosition != lastFastCuePos || myclass != lastFastClass) {
            lastFastCuePos = cueBall.initialPosition;
            lastFastClass = myclass;
            fastCandidateOffset = 0;
            fastCandidates.clear();

            auto pockets = getPockets();
            bool bFoundLowestNumberedBall = false;

            for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                if (isNineBallGame && bFoundLowestNumberedBall) break;
                auto& ball = gPrediction->guiData.balls[i];
                if (!ball.originalOnTable) continue;
                if (!bFoundLowestNumberedBall) bFoundLowestNumberedBall = true;

                if (!isNineBallGame) {
                    bool isACandidate = myclass == Ball::Classification::ANY
                        ? ball.classification != Ball::Classification::EIGHT_BALL
                        : ball.classification == myclass;
                    if (!isACandidate) continue;
                }

                for (int pocketIdx = 0; pocketIdx < (int)pockets.size(); pocketIdx++) {
                    if (nominatedPocket < 6 && pocketIdx != (int)nominatedPocket) continue;
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
                    constexpr double slidingDeceleration = 196.0;
                    double power = sqrt(2.0 * slidingDeceleration * score);
                    if (power > 666.0) power = 666.0;
                    fastCandidates.push_back({i, angle, score, pocketIdx, power});
                }
            }
            std::sort(fastCandidates.begin(), fastCandidates.end());
        }

        // Process up to 20 candidates per frame to avoid single-frame spike
        constexpr int MAX_FAST_PER_FRAME = 12; // SAFE: reduced to avoid frame spikes & heat
        int processed = 0;
        bool foundShot = false;

        while (fastCandidateOffset < (int)fastCandidates.size() && processed < MAX_FAST_PER_FRAME) {
            Candidate cand = fastCandidates[fastCandidateOffset++]; // mutable copy (FIX-2 needs to update power)
            processed++;

            double angle = NumberUtils::normalizeDoublePrecision(normalizeAngle(cand.angle));
            // FIX-2: Try estimated power first, then fallbacks if estimate is off
            constexpr double fbPowers[] = {666.0, 466.0, 266.0, 100.0};
            bool scanHit = false;
            auto spin__ = sharedGameManager.getShotSpin();
            gPrediction->determineShotResult(true, angle, cand.power, spin__, cand);
            if (gPrediction->firstHitIsTarget && gPrediction->guiData.balls[0].onTable) {
                scanHit = true;
            } else {
                for (double fp : fbPowers) {
                    if (std::abs(fp - cand.power) < 1.0) continue;
                    gPrediction->determineShotResult(true, angle, fp, spin__, cand);
                    if (gPrediction->firstHitIsTarget && gPrediction->guiData.balls[0].onTable) {
                        cand.power = fp; scanHit = true; break;
                    }
                }
            }
            if (!scanHit) continue;


            if (isNineBallGame) {
                auto firstHit = gPrediction->guiData.collision.firstHitBall;
                if (!firstHit || firstHit->index != cand.idx) continue;

                int bestPottedIdx = -1;
                for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                    auto& ball = gPrediction->guiData.balls[i];
                    if (ball.originalOnTable && !ball.onTable) {
                        if (nominatedPocket < 6 && ball.pocketIndex != (int)nominatedPocket) continue;
                        if (i == 9) { bestPottedIdx = 9; break; }
                        if (bestPottedIdx == -1 || i == cand.idx) bestPottedIdx = i;
                    }
                }
                if (bestPottedIdx == -1) continue;

                int strat9 = persistent_int["i9BallStrat"];
                if (strat9 == 1 && bestPottedIdx != 9) continue;

                g_CurrentCandidate = cand;
                g_CurrentCandidate.idx = bestPottedIdx;
                g_CurrentCandidate.pocketIndex = gPrediction->guiData.balls[bestPottedIdx].pocketIndex;
                foundShot = true;
                PrepareShot(angle, cand.power);
                break;
            }

            if (gPrediction->guiData.balls[cand.idx].onTable) continue;
            if (gPrediction->guiData.balls[cand.idx].pocketIndex != cand.pocketIndex) continue;

            bool isAngleGood = false;
            for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                Prediction::Ball& ball = gPrediction->guiData.balls[i];
                bool match = (myclass == Ball::Classification::ANY)
                    ? (ball.classification != Ball::Classification::CUE_BALL && ball.classification != Ball::Classification::EIGHT_BALL)
                    : (ball.classification == myclass);
                if (match && ball.originalOnTable && !ball.onTable) isAngleGood = true;
            }

            if (isAngleGood && gPrediction->guiData.collision.firstHitBall) {
                auto firstHit = gPrediction->guiData.collision.firstHitBall;
                if (myclass != Ball::Classification::ANY && firstHit->classification != myclass) isAngleGood = false;
                else if (myclass == Ball::Classification::ANY && firstHit->classification == Ball::Classification::EIGHT_BALL) isAngleGood = false;
            }

            if (isAngleGood && !gPrediction->guiData.balls[0].onTable) isAngleGood = false;

            auto& eightBallRef = gPrediction->guiData.balls[8];
            if (isAngleGood && (eightBallRef.originalOnTable && !eightBallRef.onTable) && myclass != Ball::Classification::EIGHT_BALL) isAngleGood = false;

            int strat8 = persistent_int["i9BallStrat"];
            if (isAngleGood && strat8 == 1 && myclass == Ball::Classification::EIGHT_BALL) {
                if (cand.idx != 8) isAngleGood = false;
            }

            if (isAngleGood) {
                g_CurrentCandidate = cand;
                foundShot = true;
                PrepareShot(angle, cand.power);
                break;
            }
        }

        if (!foundShot && fastCandidateOffset >= (int)fastCandidates.size()) {
            lastFailedCuePos = cueBall.initialPosition;
            scan = SLOW;
        }
    }

    bool isAnimationActive() {
        auto visualCue = sharedGameManager.mVisualCue();
        if (!visualCue) return true;
        auto _powerBarView = F(ptr, visualCue + 0x510);
        if (!_powerBarView) return true;
        
        // إصلاح خطأ المقارنة بـ nullptr
        uintptr_t activeAction = M(uintptr_t, libmain + 0x2de6f30, ptr)(_powerBarView);
        return (activeAction != 0); 
    }
    
    void Update() {
        buttonClicker.Update();

        if (isAnimationActive()) return;

        if (!bAutoPlaying || !sharedGameManager.mStateManager().isPlayerTurn()) {
            // Giliran habis/bukan giliran kita — stop thread kalau masih jalan
            if (g_threadRunning.load(std::memory_order_relaxed)) StopScanThread();
            state = IDLE;
            scan  = FAST;
            return;
        }

        if (state == IDLE) {
            // Don't start scanning if cue ball hasn't moved since last failed scan
            if (gPrediction->guiData.balls[0].initialPosition == lastFailedCuePos) return;
            state = SCANNING;
            scan  = FAST;
        } else if (state == SCANNING) {
            if (scan == FAST) {
                ScanFast();
            } else if (scan == SLOW) {
                int speed = persistent_int["iAutoPlay_Speed"];
                if (speed == 3) {
                    // Turbo: langsung skip ke FAST lagi
                    StopScanThread();
                    scan  = FAST;
                    state = IDLE;
                    return;
                }

                g_autoPlayCalculating = true;  // show CALCULATING overlay

                // ── Launch background thread (hanya sekali per sesi scan) ──
                if (!g_threadRunning.load(std::memory_order_acquire)
                    && !g_threadDone.load(std::memory_order_acquire)) {

                    TakeSnapshot();
                    auto myclass  = sharedGameManager.getPlayerClassification();
                    auto nomPocket = sharedGameManager.getNominatedPocket();
                    auto spin     = sharedGameManager.getShotSpin();
                    int  strat    = persistent_int["i9BallStrat"];

                    g_threadResult.found = false;
                    g_threadShouldStop.store(false, std::memory_order_relaxed);
                    g_threadDone.store(false,       std::memory_order_relaxed);
                    g_threadRunning.store(true,     std::memory_order_relaxed);

                    if (g_scanThread.joinable()) g_scanThread.join();
                    g_scanThread = std::thread(ScanThreadFunc,
                                              myclass, nomPocket, spin, strat);
                }

                // ── Cek apakah thread sudah selesai ──
                if (g_threadDone.load(std::memory_order_acquire)) {
                    if (g_scanThread.joinable()) g_scanThread.join();

                    if (g_threadResult.found) {
                        g_CurrentCandidate = g_threadResult.candidate;
                        PrepareShot(g_threadResult.angle, g_threadResult.power);
                    } else {
                        // Thread selesai tapi tidak ada tembakan → tetap IDLE
                        lastFailedCuePos = gPrediction->guiData.balls[0].initialPosition;
                    }

                    g_threadRunning.store(false, std::memory_order_relaxed);
                    g_threadDone.store(false,    std::memory_order_relaxed);
                    g_threadResult.found = false;
                    g_sceneSnapshot.valid = false;
                    g_autoPlayCalculating = false;
                    state = IDLE;
                    scan  = FAST;
                }
                // (Selagi thread berjalan, Update() selesai tanpa blokir frame)
            }
        } else if (state == WAITING) {
            double dt = (double)ImGui::GetIO().DeltaTime;
            // SAFE: glide aim toward target smoothly during the delay
            if (aimSmoothActive) {
                double diff = aimSmoothTarget - aimSmoothCurrent;
                if (diff >  M_PI) diff -= 2.0 * M_PI;
                if (diff < -M_PI) diff += 2.0 * M_PI;
                aimSmoothCurrent += diff * std::min(1.0, dt * 9.0);
                sharedGameManager.mVisualCue().mVisualGuide().mAimAngle(aimSmoothCurrent);
            }
            shotDelayTimer -= dt;
            if (shotDelayTimer <= 0.0) {
                shotDelayTimer  = 0.0;
                aimSmoothActive = false;
                Shoot(pendingExecuteAngle, pendingExecutePower);
            }
        } else if (state == NOMINATING) {
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
    }
};


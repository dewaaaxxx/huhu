#pragma once

#include "Prediction.fast.h"
#include <imgui/imgui.h>
#include <algorithm>

#include "ScreenTable.h"

//#include "mod/PowerSlider.h"
#include "PhysicsModel.h"

using namespace ImGui;

constexpr double maxAngle = 360.0 / (180.0 / M_PI);

static double EaseInOutCubic(double t) {
    return t < 0.5 ? 4 * t * t * t : 1.0 - pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

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

struct PhysicalValidator {
    // Validate shot using realistic physics
    static bool validatePhysicalShot(const Prediction& pred, double power) {
        // Check cue ball survival
        if (!pred.guiData.balls[0].onTable) return false;
        
        // Check target ball delivery
        int pottedBalls = 0;
        for (int i = 1; i < pred.guiData.ballsCount; i++) {
            if (pred.guiData.balls[i].originalOnTable && !pred.guiData.balls[i].onTable) {
                pottedBalls++;
            }
        }
        
        // Ensure at least one ball was potted
        return pottedBalls > 0;
    }

    // Calculate realistic power based on distance and friction
    static double calculateRealisticPower(
        const Point2D& cueBallPos,
        const Point2D& targetBallPos,
        const Point2D& pocketPos,
        const FrictionProperties& friction
    ) {
        Point2D toTarget = targetBallPos - cueBallPos;
        double distToCueBall = std::sqrt(toTarget.square());
        
        Point2D toPocket = pocketPos - targetBallPos;
        double distToPocket = std::sqrt(toPocket.square());
        
        // Total distance ball must travel
        double totalDistance = distToCueBall + distToPocket;
        
        // Physics: v² = 2 * a * s, where a = friction deceleration
        double frictionDeceleration = 196.0; // sliding deceleration engine
        double requiredVelocity = std::sqrt(2.0 * frictionDeceleration * totalDistance);
        
        // Cap at max power
        return std::min(requiredVelocity, 666.0);
    }

    // Factor in spin effects on power requirements
    static double adjustPowerForSpin(double basePower, const Vec2d& shotSpin, double spinFactor = 0.15) {
        double spinMagnitude = std::sqrt(shotSpin.x * shotSpin.x + shotSpin.y * shotSpin.y);
        double adjustment = 1.0 + (spinMagnitude * spinFactor);
        return std::min(basePower * adjustment, 666.0);
    }

    // Validate pocket accessibility
    static bool isPocketAccessible(
        const Point2D& targetBallPos,
        const Point2D& pocketPos,
        double ballRadius = BALL_RADIUS
    ) {
        Point2D delta = pocketPos - targetBallPos;
        double distance = std::sqrt(delta.square());
        
        // Ball must be close enough to pocket to enter
        constexpr double POCKET_MARGIN = 2.0;
        return distance < POCKET_MARGIN && distance > ballRadius * 0.5;
    }
};

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

Point2D lastFailedCuePos = { -1000.0, -1000.0 };
Point2D lastSetCuePos = {-1000, -1000};

namespace AutoPlay {
    double lastSetAngle = 0.f;
    bool didSetAngle = false;
    bool bAutoPlaying = false;
    
    static FrictionProperties cachedFriction = {0.2, 0.0111, 0.025, 0.0014577259475218659, 196, 10.878, 9.8};

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
    
    enum HumanState {
        HUM_IDLE,
        HUM_THINKING,
        HUM_OVERSHOOTING,
        HUM_CORRECTING,
        HUM_HOLDING,
        HUM_STABILIZING,
        HUM_PULLING,
        HUM_DELAY_BEFORE_SHOT,
    };

    enum SpinPreset { SPIN_TOP = 0, SPIN_BOTTOM, SPIN_LEFT, SPIN_RIGHT, SPIN_CENTER };
    
    // ── Variabel Human State ──
    static inline HumanState humanState = HUM_IDLE;
    static inline double stateStartTime = 0;
    static inline double targetAngle = 0, startAngle = 0, currentOvershootTarget = 0;
    static inline double overshootOffset = 0;
    static inline double aimDuration = 0.8, pullDuration = 0.6;
    static inline double stabilizeDuration = 0.3;
    static inline double startPower = 0, targetPower = 0;
    static inline bool humanShotLocked = false;
    static inline bool g_PredictionLocked = false;
    static inline bool humanNeedsNomination = false;
    static inline int humanNominationPocket = -1;
    static inline SpinPreset spinPreset = SPIN_CENTER;
    static inline bool bAutoSpin = false;
    // FIX ROOT CAUSE: Spin yang dipakai saat scan HARUS SAMA dengan spin saat tembak.
    // Kalau berbeda: simulasi scan → hasil A, simulasi display dengan spin lain → hasil B
    // → prediction line kelihatan masuk sebelum tembak, tapi setelah tembak meleset.
    // Solusi: lock spin pada saat scan dimulai, gunakan spin yang sama untuk semua
    // determineShotResult call (scan, display, tembak) sampai shot selesai.
    static inline Vec2d lockedShotSpin = {0.0, 0.0};
    static inline bool spinIsLocked = false;
    // FIX POWER: Power yang dipakai scan harus sama persis dengan yang ditembak.
    // confirmedPower dari scan disimpan di g_CurrentCandidate.power dan di targetPower.
    // Konversi ke mPower() via ShotPowerToPower() sudah benar — tidak perlu diubah.
    // Yang penting: power sweep di scan harus cover range yang realistis
    // berdasarkan getShotPower() (skala simulasi), bukan mPower() (skala 0-1).
    static bool g_postShotLock = false;
    static double g_postShotAngle = 0.0;
    static double g_postShotPower = 0.0;
    static int g_postShotFrames = 0;
    
    // ── Random engine ──
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<> humanDelayDist(0.15, 0.4);
    
    static double nowSec() {
        auto now = std::chrono::steady_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration<double>(duration).count();
    }

    void applyAutoSpin() {
    if (!bAutoSpin) return;
    Vec2d spin = {0.0, 0.0};
    constexpr double s = 0.7;
    switch (spinPreset) {
        case SPIN_TOP:    spin = {0.0,  s}; break;
        case SPIN_BOTTOM: spin = {0.0, -s}; break;
        case SPIN_LEFT:   spin = {-s,  0.0}; break;
        case SPIN_RIGHT:  spin = { s,  0.0}; break;
        case SPIN_CENTER: spin = {0.0, 0.0}; break;
    }
    sharedGameManager.mVisualEnglishControl().mEnglish(spin);
    }

    bool shouldAutoPlay() { return !didSetAngle || lastSetAngle == sharedGameManager.mVisualCue().mVisualGuide().mAimAngle(); }

    void setAimAngle(double angle) {
        if (!sharedGameManager) return;
        auto vc = sharedGameManager.mVisualCue();
        if (!vc) return;
        auto vg = vc.mVisualGuide();
        if (!vg) return;
        lastSetCuePos = gPrediction->guiData.balls[0].initialPosition;
        vg.mAimAngle(angle);
    }

    void takeShot(double angle, double power) {
        targetAngle = angle;
        targetPower = power;
        startAngle = sharedGameManager.mVisualCue().mVisualGuide().mAimAngle();
        stateStartTime = nowSec();
        humanState = HUM_THINKING;
    }
    
    void triggerShot() {
        g_postShotLock = true;
        g_postShotAngle = targetAngle;
        // BUG FIX: pendingShotPower tidak selalu sync dengan targetPower.
        // takeShot() set targetPower, tapi triggerShot() baca pendingShotPower → power salah/lebih pelan.
        // Pakai targetPower yang di-set oleh takeShot() dan di-hold sepanjang human state machine.
        g_postShotPower = targetPower;
        g_postShotFrames = 15;
        M(void, libmain + 0x2dc0c58, void*)(F(void*, sharedGameManager + 0x3b0));
    }
    
    void ClearState() {
        g_CurrentCandidate.idx = -1;
        lastFailedCuePos = { -1000.0, -1000.0 };
        state = IDLE;
        humanState = HUM_IDLE;
        spinIsLocked = false; // Unlock spin supaya scan berikutnya lock spin fresh
    }
    
    void Shoot(double angle, double power = 0.f) {
        setAimAngle(angle);
        gPrediction->determineShotResult(true, angle, power);

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
            pendingShotPower = power;
            pendingShotAngle = angle;
            takeShot(angle, power);
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
            // Lock spin saat mulai scan baru
            if (!spinIsLocked) {
                if (bAutoSpin) applyAutoSpin();
                lockedShotSpin = sharedGameManager.getShotSpin();
                spinIsLocked = true;
            }
        }

        Ball::Classification myclass = sharedGameManager.getPlayerClassification();
        uint nominatedPocket = sharedGameManager.getNominatedPocket();
        auto& cueBall = gPrediction->guiData.balls[0];
        
        int steps = 0;
        bool foundShot = false;
        
        // Scan 10 angles per frame
        while (steps < 20 && currentScanAngle < maxAngle) {
            double angle = currentScanAngle;
            currentScanAngle += angleStep;
            steps++;

            // 🎱 Physics-based power progression
            std::vector<double> powers;
            for (double p = 666.0; p >= 100.0; p -= 100.0) {
                powers.push_back(p);
            }
            
            
            for (double power : powers) {
                gPrediction->determineShotResult(true, angle, power, lockedShotSpin);
                
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

                    // 🎱 Physical validation
                    if (PhysicalValidator::validatePhysicalShot(*gPrediction, power)) {
                        isPotentiallyValid = true;
                        g_CurrentCandidate.idx = targetIdx;
                        g_CurrentCandidate.angle = angle;
                        g_CurrentCandidate.power = power;
                        g_CurrentCandidate.pocketIndex = gPrediction->guiData.balls[targetIdx].pocketIndex;
                    }
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
        
        if (!spinIsLocked) {
            if (bAutoSpin) applyAutoSpin();
            lockedShotSpin = sharedGameManager.getShotSpin();
            spinIsLocked = true;
        }

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
                
                // 🎱 Physics-based power calculation
                double basePower = PhysicalValidator::calculateRealisticPower(
                    cueBall.initialPosition,
                    ball.initialPosition,
                    pocket,
                    cachedFriction
                );
                
                // Adjust for spin
                auto spin = sharedGameManager.getShotSpin();
                double power = PhysicalValidator::adjustPowerForSpin(basePower, spin);
                
                if (power > 666.0) power = 666.0;
                
                candidates.push_back({i, angle, score, pocketIdx, power});
            }
        }
        
        std::sort(candidates.begin(), candidates.end());
        
        // ================================================================
        // VALIDASI DENGAN POWER SWEEP + ANGLE REFINEMENT
        // ================================================================
        static const double kAngleOffsets[] = {0.0, -0.0175, +0.0175, -0.035, +0.035};
        // Power sweep: kompensasi energy loss di collision dan jarak berbeda-beda.
        // Base power dulu, lalu naik (shot jauh), lalu turun (shot dekat).
        static const double kPowerFactors[] = {1.0, 1.15, 0.85, 1.3, 0.7};
        
        bool foundShot = false;
        for (const auto& cand : candidates) {
            double baseAngle = NumberUtils::normalizeDoublePrecision(normalizeAngle(cand.angle));
        
            bool simOk = false;
            double confirmedAngle = baseAngle;
            double confirmedPower = cand.power;
        
            for (double dA : kAngleOffsets) {
                if (simOk) break;
                double tryAngle = NumberUtils::normalizeDoublePrecision(normalizeAngle(baseAngle + dA));
                for (double pf : kPowerFactors) {
                    double tryPower = std::min(std::max(cand.power * pf, 80.0), 666.0);
                    gPrediction->determineShotResult(true, tryAngle, tryPower, lockedShotSpin, cand);
        
                    if (!gPrediction->firstHitIsTarget) continue;

                    if (!gPrediction->guiData.balls[0].onTable) continue; // cue ball should not be pocketed
        
                    if (myclass == Ball::Classification::NINE_BALL_RULE) {
                        auto firstHit = gPrediction->guiData.collision.firstHitBall;
                        if (!firstHit) continue;
                        // Must hit the target ball (which is the lowest numbered ball) first
                        if (firstHit->index != cand.idx) continue;
                   
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
                        int effectiveTargetIdx = bestPottedIdx;
        
                        if (nominatedPocket < 6 && gPrediction->guiData.balls[effectiveTargetIdx].pocketIndex != nominatedPocket) continue;
        
                        confirmedAngle = tryAngle;
                        confirmedPower = tryPower;
                        simOk = true;
        
                        LOGI("AutoPlay: 9ball: angle %f power %f", confirmedAngle, confirmedPower);
                        g_CurrentCandidate = cand;
                        g_CurrentCandidate.idx = effectiveTargetIdx;
                        g_CurrentCandidate.angle = confirmedAngle;
                        g_CurrentCandidate.power = confirmedPower;
                        g_CurrentCandidate.pocketIndex = gPrediction->guiData.balls[effectiveTargetIdx].pocketIndex;
                        foundShot = true;
                        Shoot(confirmedAngle, confirmedPower);
                        break;   // ← TAMBAH INI
                    }
        
                    // ================================================================
                    // 3. 8-BALL / REGULAR PATH
                    // ================================================================
                    // CEK: target ball kepotong
                    if (gPrediction->guiData.balls[cand.idx].onTable) continue; // target ball was not potted
                    if (gPrediction->guiData.balls[cand.idx].pocketIndex != cand.pocketIndex) continue; // target ball did not go into target pocket
        
                    // CEK: apakah ada bola sendiri yang kepotong?
                    std::vector<int> currentPottedBalls;
                    bool isAngleGood = false;
                    for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                        Prediction::Ball& ball = gPrediction->guiData.balls[i];
                        bool match = (myclass == Ball::Classification::ANY)
                            ? (ball.classification != Ball::Classification::CUE_BALL && ball.classification != Ball::Classification::EIGHT_BALL)
                            : (ball.classification == myclass);
        
                        if (match && ball.originalOnTable && !ball.onTable) {
                            currentPottedBalls.push_back(i);
                            isAngleGood = true;
                        }
                    }
        
                    // CEK: first hit classification
                    if (isAngleGood && gPrediction->guiData.collision.firstHitBall) {
                         auto firstHit = gPrediction->guiData.collision.firstHitBall;
                         if (myclass != Ball::Classification::ANY && firstHit->classification != myclass) isAngleGood = false;
                         else if (myclass == Ball::Classification::ANY && firstHit->classification == Ball::Classification::EIGHT_BALL) isAngleGood = false;
                    }
        
                    // CEK: cue ball masih di meja
                    auto& cueBallRef = gPrediction->guiData.balls[0];
                    if (isAngleGood && cueBallRef.originalOnTable && !cueBallRef.onTable) isAngleGood = false;
        
                    // CEK: 8-ball tidak masuk premature
                    auto& eightBallRef = gPrediction->guiData.balls[8];
                    bool isEightBallPotted = eightBallRef.originalOnTable && !eightBallRef.onTable;
                    if (isAngleGood && isEightBallPotted && myclass != Ball::Classification::EIGHT_BALL) isAngleGood = false;
        
                    if (!isAngleGood) continue;
        
                    // ================================================================
                    // 4. LULUS SEMUA VALIDASI → TEMBAK
                    // ================================================================
                    confirmedAngle = tryAngle;
                    confirmedPower = tryPower;
                    simOk = true;
                    break;
                }
                if (simOk || foundShot) break;  // ← BREAK ANGLE LOOP
            }
        
            if (!simOk) continue;
        
            LOGI("AutoPlay: Found good angle %f power %f", confirmedAngle, confirmedPower);
            g_CurrentCandidate = cand;
            g_CurrentCandidate.angle = confirmedAngle;
            g_CurrentCandidate.power = confirmedPower;
            foundShot = true;
            Shoot(confirmedAngle, confirmedPower);
            break;
        };

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
    
    void Update() {
        buttonClicker.Update();
  //      powerSlider.Update();

        if (isAnimationActive()) return;

        if (!persistent_bool[O("bAutoPlay")] || !bAutoPlaying || !sharedGameManager.mStateManager().isPlayerTurn()) {
            // Kalau human state machine sedang jalan, jangan interrupt
            if (humanState != HUM_IDLE) return;
            // Kalau sedang EXECUTING (nomination → shot), jangan reset
            g_CurrentCandidate.idx = -1;
            if (state == EXECUTING) return;
            state = IDLE;
            NativeTouchesEnd(5, 0, 0);   // Joystick
        //    NativeTouchesEnd(10, 0, 0);  // Slider
            return;
        }

        if (state == IDLE) {
                        // BUG FIX #2: Kalau humanState != HUM_IDLE, artinya human state machine
            // sedang jalan (aim, pull, dll). Jangan langsung SCANNING lagi —
            // biarkan human machine selesai dulu. Tanpa guard ini, IDLE langsung
            // jadi SCANNING → ScanFast lagi → Shoot lagi → loop selamanya.
            if (humanState == HUM_IDLE) {
                state = SCANNING;
                scan = FAST;
            }
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
                uint nominatedPocket = sharedGameManager.getNominatedPocket();
                if (nominatedPocket == (uint)g_CurrentCandidate.pocketIndex) {
                    // Nominasi confirmed — re-validasi shot dengan full simulation
                    
                    gPrediction->determineShotResult(true, pendingShotAngle, pendingShotPower,
                                                     lockedShotSpin, g_CurrentCandidate);
                    

                    // Scratch check setelah nominasi
                    if (!gPrediction->guiData.balls[0].onTable) {
                        LOGI("[AUTOPLAY] Post-nomination scratch detected, cancelling");
                        ClearState();
                        return;
                    }

                    // Update pocketIndex dari simulasi fresh
                    if (g_CurrentCandidate.idx >= 0 && g_CurrentCandidate.idx < gPrediction->guiData.ballsCount) {
                        int freshPocket = gPrediction->guiData.balls[g_CurrentCandidate.idx].pocketIndex;
                        if (freshPocket >= 0 && freshPocket < 6) {
                            g_CurrentCandidate.pocketIndex = freshPocket;
                        }
                    }

                    // Cek bola target masih valid ke pocket yang dinominasi
                    if (g_CurrentCandidate.pocketIndex != (int)nominatedPocket) {
                        LOGI("[AUTOPLAY] Target pocket mismatch after nomination, cancelling");
                        ClearState();
                        return;
                    }

                    // Start human state machine
                    applyAutoSpin();
                    startAngle = sharedGameManager.mVisualCue().mVisualGuide().mAimAngle();
                    targetAngle = pendingShotAngle;
                    targetPower = pendingShotPower;
                    humanShotLocked = true;
                    humanState = HUM_THINKING;
                    stateStartTime = nowSec() + 0.3;
                    state = EXECUTING; // EXECUTING biar tidak di-reset oleh isPlayerTurn check
                } else {
                    // Retry setiap 30 frame, timeout 150 frame
                    if (nominationFrameCounter % 30 == 0) {
                        if (nominationFrameCounter > 150) {
                            LOGI("[AUTOPLAY] Nomination timeout, resetting");
                            ClearState();
                            lastFailedCuePos = gPrediction->guiData.balls[0].initialPosition;
                        } else {
                            LOGI("[AUTOPLAY] Nomination retry #%d", nominationFrameCounter / 30);
                            buttonClicker.Click(GetPocketScreenPos(g_CurrentCandidate.pocketIndex));
                        }
                    }
                }
            }
        }
            // ─── HUMAN STATE MACHINE ────────────────────────────────────────────
        // ─── HIDE PREDICTION LINES DURING HUMAN STATE ──────────────────────────
        if (humanState != HUM_IDLE) {
        double now = nowSec();

        auto UpdateJoystickVisuals = [&](double angle) {
            float jX = Width * 0.83f;
            float jY = Height * 0.82f;
            float jR = 65.0f;
            float tX = jX + cos(angle) * jR;
            float tY = jY + sin(angle) * jR;
            NativeTouchesMove(5, tX, tY);

            ImDrawList* fg = ImGui::GetForegroundDrawList();
    if (fg) {
        fg->AddCircleFilled(ImVec2(tX, tY), 10.0f, IM_COL32(255, 255, 255, 100));
        fg->AddCircle(ImVec2(tX, tY), 10.0f, IM_COL32(255, 255, 255, 200), 0.0f, 2.0f);
    }
        };
    
        // 1. HUM_THINKING (0.5s pause)
        if (humanState == HUM_THINKING) {
            if (now >= stateStartTime) {
                overshootOffset = (gen() % 2 == 0 ? 1 : -1) * 0.058;
                currentOvershootTarget = targetAngle + overshootOffset;
                stateStartTime = now;
                humanState = HUM_OVERSHOOTING;
                NativeTouchesBegin(5, Width * 0.83f, Height * 0.82f);
            }
            return;
        }

        // 2. ROTATION (1.1s smooth sweep to overshoot)
        if (humanState == HUM_OVERSHOOTING) {
            double t = (now - stateStartTime) / 1.1;
            if (t >= 1.0) {
                setAimAngle(currentOvershootTarget);
                UpdateJoystickVisuals(currentOvershootTarget);
                stateStartTime = now;
                humanState = HUM_CORRECTING;
            } else {
                double ease = EaseInOutCubic(t);
                double normalizedStart = normalizeAngle(startAngle);
                double normalizedTarget = normalizeAngle(currentOvershootTarget);
                double delta = normalizedTarget - normalizedStart;
                if (delta > M_PI) delta -= 2.0 * M_PI; if (delta < -M_PI) delta += 2.0 * M_PI;
                double curAngle = normalizedStart + delta * ease;
                setAimAngle(curAngle);
                UpdateJoystickVisuals(curAngle);
            }
            gPrediction->determineShotResult(true, targetAngle, pendingShotPower, lockedShotSpin, g_CurrentCandidate);
            return;
        }

        // 3. ELASTIC SNAP BACK (0.35s)
        if (humanState == HUM_CORRECTING) {
            double t = (now - stateStartTime) / 0.35;
            double dirSign = (overshootOffset > 0) ? 1.0 : -1.0;
            double nudgeAngle = targetAngle + dirSign * (1.5 * M_PI / 180.0);
            
            if (t >= 1.0) {
                setAimAngle(nudgeAngle);
                UpdateJoystickVisuals(nudgeAngle);
                stateStartTime = now;
                humanState = HUM_HOLDING;
            } else {
                double ease = EaseInOutCubic(t);
                double normalizedStart = normalizeAngle(currentOvershootTarget);
                double normalizedTarget = normalizeAngle(nudgeAngle);
                double delta = normalizedTarget - normalizedStart;
                if (delta > M_PI) delta -= 2.0 * M_PI; if (delta < -M_PI) delta += 2.0 * M_PI;
                double curAngle = normalizedStart + delta * ease;
                setAimAngle(curAngle);
                UpdateJoystickVisuals(curAngle);
            }
            gPrediction->determineShotResult(true, targetAngle, pendingShotPower, lockedShotSpin, g_CurrentCandidate);
            return;
        }

        // 3b. HOLD TOUCH AT TARGET (0.40s slow nudge/adjustment to exact target + hold)
        if (humanState == HUM_HOLDING) {
            double t = (now - stateStartTime) / 0.40;
            double dirSign = (overshootOffset > 0) ? 1.0 : -1.0;
            double nudgeAngle = targetAngle + dirSign * (1.5 * M_PI / 180.0);
            
            if (t >= 1.0) {
                setAimAngle(targetAngle);
                UpdateJoystickVisuals(targetAngle);
                
                float jX = Width * 0.83f;
                float jY = Height * 0.82f;
                float jR = 65.0f;
                // Keep joystick held at target - DO NOT release yet!
                // Releasing here causes 0.4s+0.85s gap with no joystick touch,
                // during which the game resets aim direction → wrong angle on shot.
                NativeTouchesMove(5, jX + (float)cos(targetAngle) * jR, 
                                     jY + (float)sin(targetAngle) * jR);
                stateStartTime = now;
                humanState = HUM_STABILIZING;
            } else {
                double ease = sin(t * M_PI_2); // Ease-out to slow down at target
                double normalizedStart = normalizeAngle(nudgeAngle);
                double normalizedTarget = normalizeAngle(targetAngle);
                double delta = normalizedTarget - normalizedStart;
                if (delta > M_PI) delta -= 2.0 * M_PI; if (delta < -M_PI) delta += 2.0 * M_PI;
                double curAngle = normalizedStart + delta * ease;
                setAimAngle(curAngle);
                UpdateJoystickVisuals(curAngle);
            }
            gPrediction->determineShotResult(true, targetAngle, pendingShotPower, lockedShotSpin, g_CurrentCandidate);
            return;
        }

        // 4. STABILIZE & LOCK (0.4s) - joystick still held from HUM_HOLDING
        if (humanState == HUM_STABILIZING) {
            NativeTouchesMove(5, Width * 0.83f + cos(targetAngle) * 65.0f,
                                 Height * 0.82f + sin(targetAngle) * 65.0f);
            setAimAngle(targetAngle);
    
            if (now - stateStartTime >= 0.4) {
                NativeTouchesEnd(5, Width * 0.83f + cos(targetAngle) * 65.0f,
                                    Height * 0.82f + sin(targetAngle) * 65.0f);    
                stateStartTime = now;
                humanState = HUM_PULLING;
            }
            return;
        }

        // 5. POWER PULL (0.85s smooth) via simulated slider touch
        if (humanState == HUM_PULLING) {
            setAimAngle(targetAngle);

            gPrediction->determineShotResult(true, targetAngle, targetPower,
                                             lockedShotSpin, g_CurrentCandidate);
                                            
            sharedGameManager.mVisualCue().mPower(ShotPowerToPower(targetPower));

            stateStartTime = now;
            humanState = HUM_DELAY_BEFORE_SHOT;
            return;
        }

        // 6. FINAL HUMAN PAUSE (0.4s) then FIRE!
        if (humanState == HUM_DELAY_BEFORE_SHOT) {
            setAimAngle(targetAngle);
            if (now - stateStartTime >= 0.4) {
                setAimAngle(targetAngle);
                sharedGameManager.mVisualCue().mPower(ShotPowerToPower(targetPower));
                // FIRE SHOT
                triggerShot();
                humanShotLocked = false;
                ClearState();
                state = IDLE; humanState = HUM_IDLE;
            }
            return;
        }
    }
    bool isPlayerTurn = sharedGameManager.mStateManager().isPlayerTurn();
    if (isPlayerTurn && bAutoSpin) applyAutoSpin();

    // PREDICTION LINES:
    // Di Prediction ini, lines digambar berdasarkan parameter `isAuto` di determineShotResult:
    //   isAuto=false → fastCalc=false → positions di-track → lines TAMPIL
    //   isAuto=true  → fastCalc=true  → positions tidak di-track → lines HILANG
    //
    // - Saat humanState aktif (lagi aiming/pulling): panggil isAuto=true → lines hilang
    // - Saat humanState HUM_IDLE (setelah shot selesai): panggil isAuto=false → lines tampil
    if (humanState != HUM_IDLE) {
        // Lines hilang selama human state machine jalan.
        // Pakai lockedShotSpin supaya simulasi konsisten dengan yang dipilih saat scan.
        gPrediction->determineShotResult(true, targetAngle, targetPower, lockedShotSpin);
    } else if (isPlayerTurn && g_CurrentCandidate.idx == -1) {
        // Setelah shot selesai: unlock spin supaya scan berikutnya bisa lock fresh.
        spinIsLocked = false;
        // Lines tampil lagi, ikuti aim angle real-time.
        if (gPrediction && sharedGameManager) {
            double curAngle = sharedGameManager.mVisualCue().mVisualGuide().mAimAngle();
            // FIX POWER: mPower() return skala game (0.0–1.0), sedangkan
            // determineShotResult expect skala simulasi (0–666 = langsung velocity).
            // getShotPower() sudah return skala simulasi yang benar.
            // mPower() mentah menyebabkan display lines pakai power jauh lebih kecil
            // dari yang dipakai saat scan → trajectory berbeda → lines meleset.
            double curPower = sharedGameManager.mVisualCue().getShotPower();
            if (curPower < 10.0) curPower = 400.0; // fallback kalau belum ada power
            gPrediction->determineShotResult(false, curAngle, curPower, lockedShotSpin);
        }
    }
    }
};

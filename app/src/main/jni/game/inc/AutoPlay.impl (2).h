#include "AutoPlayy.h"
#include "Prediction.fast.h"
extern ButtonClicker buttonClicker;
#include "PowerSlider.h"
extern PowerSlider powerSlider;
#include <math.h>
#include <random>
// --- Static Helpers ---

// AutoPlay.h - di dalam class AutoPlay, bagian public
static inline bool IsShotValid() {
    if (AutoPlay::g_CurrentCandidate.idx == -1) return false;
    if (!gPrediction || !gPrediction->guiData.balls[0].onTable) return false;
    if (gPrediction->guiData.balls[AutoPlay::g_CurrentCandidate.idx].onTable) return false;
    
    uint nominatedPocket = sharedGameManager.getNominatedPocket();
    if (nominatedPocket < 6 && AutoPlay::g_CurrentCandidate.pocketIndex != nominatedPocket) return false;
    
    auto firstHit = gPrediction->guiData.collision.firstHitBall;
    if (!firstHit) return false;
    
    Ball::Classification myclass = sharedGameManager.getPlayerClassification();
    if (myclass != Ball::Classification::ANY && firstHit->classification != myclass) return false;
    if (myclass == Ball::Classification::ANY && firstHit->classification == Ball::Classification::EIGHT_BALL) return false;
    
    return true;
}

static double EaseInOutCubic(double t) {
    return t < 0.5 ? 4 * t * t * t : 1.0 - pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

static double DistToSegmentSq(const Point2D& p, const Point2D& a, const Point2D& b) {
    Point2D v = b - a;
    Point2D w = p - a;
    double c1 = w.x * v.x + w.y * v.y; // dot product w . v
    if (c1 <= 0) return (p - a).square();
    double c2 = v.x * v.x + v.y * v.y; // dot product v . v
    if (c2 <= c1) return (p - b).square();
    double t = c1 / c2;
    Point2D closest = { a.x + t * v.x, a.y + t * v.y };
    return (p - closest).square();
}

// Global random engine
static std::random_device rd;
static std::mt19937 gen(rd());
static std::uniform_real_distribution<> humanDelayDist(0.15, 0.4);

static bool bAimedThisTurn = false;
static Point2D lastCuePosWhenAimed = { -1000.0, -1000.0 };


// ==================== CORE IMPLEMENTATIONS ====================

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

constexpr double maxAngle = 360.0 / (180.0 / M_PI);

double normalizeAngle(double angle) {
    double newAngle = angle;
    if (newAngle >= maxAngle) newAngle = fmod(newAngle, maxAngle);
    else if (newAngle < 0) newAngle = maxAngle - fmod(-newAngle, maxAngle);
    return newAngle;
}


static double CalculateRequiredPower(double totalDist) {
    // AIMX Physics Sync: v = sqrt(2 * a * s) where a = 196.0
    double p = sqrt(totalDist * 2.0 * 196.0); 
    if (p < 100.0) p = 100.0; 
    if (p > 666.0) p = 666.0;
    return p;
}

ImVec2 GetPocketScreenPos(int pocketIdx) {
    Table table = sharedGameManager.mTable;
    if (!table) return {};

    auto tableProperties = table.mTableProperties();
    if (!tableProperties) return {};

    if (pocketIdx < 0 || pocketIdx >= 6) return {};

    auto& pockets = tableProperties.mPockets();
    return WorldToScreen(pockets[pocketIdx]);
}

void AutoPlay::applyAutoSpin() {
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

std::vector<Point2D> AutoPlay::getPockets() {
    auto pts = ::getPockets();
    return std::vector<Point2D>(pts.begin(), pts.end());
}

static inline double g_lastSyncAngle = -999.0;
static inline double g_shotCooldownEnd = 0.0; // Prevents re-scan during shot animation
static int fastShotState = 0;                 // Fast shot sequence state (0: aiming, 1: stabilize, 2: delay before fire)
static inline Point2D lastScanSlowCuePos = {-1000, -1000};

// Persistent Scanning State (Accessible to Update() and Scan functions)
static double currentScanAngle = 0.0;
static bool isScanningInProgress = false;
static AutoPlay::FastScanState fs;

// Animation State for Smooth Power Pull
static double anim_CurrentPower = 0.0;
static double anim_TargetPower = 0.0;
static double anim_TargetAngle = 0.0;
static bool anim_IsPulling = false;
static long long anim_StartTime = 0;
static bool anim_RotationDone = false;     // Prevents Phase 2 running every frame
static bool anim_TouchStarted = false;     // Ensures joystick touch always starts reliably
static double g_lastFastShotTime = 0.0;   // Cooldown to prevent double-shot

static bool g_postShotLock = false;
static double g_postShotAngle = 0.0;
static double g_postShotPower = 0.0;
static int g_postShotFrames = 0;

static bool g_postAimLock = false;
static double g_postAimAngle = 0.0;
static double g_postAimPower = 0.0;
static int g_postAimFrames = 0;

void AutoPlay::ClearState() {
    g_CurrentCandidate.idx = -1;
    lastFailedCuePos = {-1000, -1000};
    lastSetCuePos = {-1000, -1000};
    humanNeedsNomination = false;
    humanNominationPocket = -1;
    g_autoPlayCalculating = false;
    g_PredictionLocked = false;
    g_lastSyncAngle = -999.0;
    humanState = HUM_IDLE;
    humanShotLocked = false;
    bShowAutoPlayLines = false;
    state = IDLE; // CRITICAL: Reset state machine
    fastShotState = 0;
    anim_IsPulling = false;
    anim_RotationDone = false;
    anim_TouchStarted = false;
    fs.isInitiated = false;

    if (!g_postShotLock) {
        setPower(0.0);
    }

    if (anim_TouchStarted) {
        NativeTouchesEnd(5, Width * 0.83f, Height * 0.82f);
    }

    if (powerSlider.Active) {
        float sliderXPercent = persistent_float[O("fPowerBarXPercent")];
        float sliderX = Width * sliderXPercent;
        if (persistent_int[O("iPowerBarSide")] == 1) {
            sliderX = Width * (1.0f - sliderXPercent);
        }
        float sliderYStart = Height * persistent_float[O("fPowerBarYStartPercent")];
        NativeTouchesEnd(powerSlider.TouchIndex, sliderX, sliderYStart);
        powerSlider.Active = false;
        powerSlider.state = PowerSlider::IDLE;
    }

    if (buttonClicker.Active) {
        NativeTouchesEnd(buttonClicker.TouchIndex, buttonClicker.ClickPos.x, buttonClicker.ClickPos.y);
        buttonClicker.Active = false;
        buttonClicker.state = ButtonClicker::IDLE;
    }

    // BUG FIX #2 & #3: Hapus g_shotCooldownEnd dari sini.
    // ClearState() dipanggil dari banyak path (g_postShotLock, abort handler, dll)
    // yang bukan "shot baru ditembak". Reset cooldown di sini menyebabkan
    // cooldown 2.0s restart dari nol setiap ClearState() dipanggil,
    // sehingga total block bisa 4.5s+ (additive dengan g_lastFastShotTime).
    // Cooldown hanya di-set di triggerShot() dan fastShotState==3 — tempat yang tepat.
    // g_shotCooldownEnd = AutoPlay::nowSec() + 2.0; // REMOVED
}

void AutoPlay::setAimAngle(double angle) {
    if (!sharedGameManager) return;
    auto vc = sharedGameManager.mVisualCue();
    if (!vc) return;
    auto vg = vc.mVisualGuide();
    if (!vg) return;
    lastSetCuePos = gPrediction->guiData.balls[0].initialPosition;
    vg.mAimAngle(angle);
}

void AutoPlay::setPower(double power) {
    if (!sharedGameManager) return;
    auto vc = sharedGameManager.mVisualCue();
    if (!vc) return;
    vc.mPower(ShotPowerToPower(power));
}

double AutoPlay::getCurrentPower() {
    if (!sharedGameManager) return 0.0;
    auto vc = sharedGameManager.mVisualCue();
    if (!vc) return 0.0;
    return vc.mPower();
}

void AutoPlay::takeShot(double angle, double power, bool preserveStartAngle) {
    anim_TargetAngle = angle;
    anim_TargetPower = power;
    anim_CurrentPower = 0.0;
    anim_IsPulling = true;
    anim_StartTime = 0;
    fastShotState = 0;
    anim_RotationDone = false;
    anim_TouchStarted = false;
    
    // FAST MODE ROTATION START
    // If preserveStartAngle is true, caller has already set startAngle correctly
    // (e.g. post-nomination where visual cue angle may be stale/wrong).
    // Only read from visual cue when NOT preserving.
    if (!preserveStartAngle) {
        if (sharedGameManager && sharedGameManager.mVisualCue() && sharedGameManager.mVisualCue().mVisualGuide()) {
            startAngle = sharedGameManager.mVisualCue().mVisualGuide().mAimAngle();
        } else {
            startAngle = angle;
        }
    }
    stateStartTime = nowSec(); 
}

void AutoPlay::triggerShot() {
    g_postShotLock = true;
    g_postShotAngle = (automationSpeed == SPEED_HUMAN) ? targetAngle : anim_TargetAngle;
    g_postShotPower = (automationSpeed == SPEED_HUMAN) ? pendingShotPower : anim_TargetPower;
    g_postShotFrames = 15;
    M(void, libmain + 0x2dc0c58, void*)(F(void*, sharedGameManager + 0x3b0));
    // BUG FIX #2: set cooldown di sini — setelah shot benar-benar ditembak,
    // bukan di ClearState() yang dipanggil dari berbagai path non-shot.
    g_shotCooldownEnd = AutoPlay::nowSec() + 0.5;
}

bool AutoPlay::IsAnimationActive() {
    auto visualCue = sharedGameManager.mVisualCue();
    if (!visualCue) return false;
    auto _powerBarView = F(ptr, visualCue + 0x510);
    if (!_powerBarView) return false;
    return (M(ptr, libmain + 0x2de6f30, ptr)(_powerBarView) != 0);
}

void AutoPlay::Shoot(double angle, double power) {
    applyAutoSpin(); // AIMX SYNC: Apply spin BEFORE simulation so visuals match
    
    // Check if it's a break shot and override power to maximum!
    bool isBreakPosition = false;
    if (gPrediction && gPrediction->guiData.ballsCount >= 15) {
        int racked = 0;
        for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
            auto& b = gPrediction->guiData.balls[i];
            if (b.initialPosition.x < 70.0 || b.initialPosition.x > 120.0) racked++;
        }
        if (racked >= 13) isBreakPosition = true;
    }
    if (isBreakPosition) {
        power = (double)powerMax;
    }

    // STRICT 4-DECIMAL SYNC: Normalize before simulation
    angle = NumberUtils::normalizeDoublePrecision(angle);
    power = NumberUtils::normalizeDoublePrecision(power);

    
    gPrediction->determineShotResult(true, angle, power, sharedGameManager.getShotSpin());
    

    bool nominating = false;
    int nominationMode = sharedGameManager.getPocketNominationMode();
    auto myclass = sharedGameManager.getPlayerClassification();
    
    // Use the EXACT angle and power that were simulated
    pendingShotPower = power;
    pendingShotAngle = angle;
    
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
        
        // Record if we need to return to human aiming after nomination
        humanNeedsNomination = (automationSpeed == SPEED_HUMAN && playStyle != STYLE_INSTANT);
        return; 
    }

    // --- AUTO AIM MODE ---
    if (currentMode == MODE_AUTO_AIM) {
        applyAutoSpin();
        if (playStyle == STYLE_INSTANT) {
            // FAST Auto Aim: set instantly, lock to stabilize, and return to IDLE
            setAimAngle(angle);
            setPower(power);
            bAimedThisTurn = true;
            lastCuePosWhenAimed = gPrediction->guiData.balls[0].initialPosition;
            g_postAimLock = true;
            g_postAimAngle = angle;
            g_postAimPower = power;
            g_postAimFrames = 20; // Stabilize visual guide for 20 frames
            ClearState();
            state = IDLE;
        } else if (automationSpeed == SPEED_HUMAN) {
            humanShotLocked = true;
            state = EXECUTING;
            humanState = HUM_THINKING;
            stateStartTime = nowSec() + 0.5;
            startAngle = sharedGameManager.mVisualCue().mVisualGuide().mAimAngle();
            targetAngle = angle;
            pendingShotPower = power;
        } else {
            // FAST Auto Aim: set instantly, lock to stabilize, and return to IDLE
            setAimAngle(angle);
            setPower(power);
            bAimedThisTurn = true;
            lastCuePosWhenAimed = gPrediction->guiData.balls[0].initialPosition;
            g_postAimLock = true;
            g_postAimAngle = angle;
            g_postAimPower = power;
            g_postAimFrames = 20; // Stabilize visual guide for 20 frames
            ClearState();
            state = IDLE;
        }
        return;
    }

    // --- AUTO PLAY MODE ONLY (FAST & HUMAN) ---
    if (automationSpeed == SPEED_HUMAN || automationSpeed == SPEED_FAST) {
        applyAutoSpin();
        
        startAngle = sharedGameManager.mVisualCue().mVisualGuide().mAimAngle();
        targetAngle = angle;
        pendingShotPower = power;
        
        // STEP 1: LOCK lines on confirmed shot - Matches Slow Mode Accuracy
        g_PredictionLocked = true;
        bShowAutoPlayLines = false; 

        // Run final simulation to show the exact locked shot line
        
        gPrediction->determineShotResult(true, angle, power, sharedGameManager.getShotSpin(), g_CurrentCandidate);
        

        if (playStyle == STYLE_INSTANT) {
            takeShot(angle, power);
            state = EXECUTING;
        } else if (automationSpeed == SPEED_HUMAN) {
            humanShotLocked = true;
            state = EXECUTING;
            humanState = HUM_THINKING;
            stateStartTime = nowSec() + 0.5;
        } else {
            // FAST MODE: Transition to EXECUTING state so animation is visible
            takeShot(angle, power);
            state = EXECUTING; 
        }
        return;
    }
}

void AutoPlay::ScanSlow(double angleStep) {
    if (g_CurrentCandidate.idx != -1) { g_autoPlayCalculating = false; return; }

    bShowAutoPlayLines = !persistent_bool[O("bDisableFlicker")];

    Point2D currentCuePos = gPrediction->guiData.balls[0].initialPosition;
    double distSq = (currentCuePos - lastScanSlowCuePos).square();

    if (!isScanningInProgress || distSq > 0.0025) { // 0.05 units threshold
        currentScanAngle = 0.0; 
        isScanningInProgress = true; 
        lastScanSlowCuePos = currentCuePos;
    }

    Ball::Classification myclass = sharedGameManager.getPlayerClassification();
    uint nominatedPocket = sharedGameManager.getNominatedPocket();

    bool onlyEightBallLeft = false;
    if (myclass == Ball::Classification::SOLID) {
        bool hasSolids = false;
        for (int k = 1; k < 8; k++) {
            if (gPrediction->guiData.balls[k].originalOnTable) { hasSolids = true; break; }
        }
        if (!hasSolids) onlyEightBallLeft = true;
    } else if (myclass == Ball::Classification::STRIPE) {
        bool hasStripes = false;
        for (int k = 9; k <= 15; k++) {
            if (gPrediction->guiData.balls[k].originalOnTable) { hasStripes = true; break; }
        }
        if (!hasStripes) onlyEightBallLeft = true;
    } else if (myclass == Ball::Classification::EIGHT_BALL) {
        onlyEightBallLeft = true;
    } else if (myclass == Ball::Classification::ANY) {
        bool hasOthers = false;
        for (int k = 1; k <= 15; k++) {
            if (k == 8) continue;
            if (gPrediction->guiData.balls[k].originalOnTable) { hasOthers = true; break; }
        }
        if (!hasOthers) onlyEightBallLeft = true;
    }

    int steps = 0;
    bool found = false;
    static Candidate bestCandidate = {-1, 0.0, 0.0, -1, 0.0};
    static int bestScore = -1;
    
    // Reset on new scan
    if (currentScanAngle == 0.0 || !isScanningInProgress) {
        bestScore = -1;
    }

    // Evaluate 1 step per frame for fast speed and high accuracy
    int maxSteps = 1;

    while (steps < maxSteps && currentScanAngle < 2.0 * M_PI) {
        double angle = currentScanAngle;
        sweepAngle = angle;
        currentScanAngle += angleStep;
        steps++;

        std::vector<double> powers = { CalculateRequiredPower(150.0), CalculateRequiredPower(350.0), (double)powerMax };
        for (double power : powers) {
            // HUMAN/FAST: Always show lines during scan for flicker effect
            bool doSim = true;
            if (cleanTableMode == CLEAN_ALL_BALLS) {
                doSim = (automationSpeed == SPEED_HUMAN); // No lag in Clean Table mode
            }
            gPrediction->determineShotResult(true, angle, power, sharedGameManager.getShotSpin());
            

            if (!gPrediction->guiData.balls[0].onTable) continue; // Skip scratches

            int tot = 0, own = 0; bool hasLegal = false, p8 = false;
            for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                if (gPrediction->guiData.balls[i].originalOnTable && !gPrediction->guiData.balls[i].onTable) {
                    tot++; if (i == 8) p8 = true;
                    if (onlyEightBallLeft) {
                        if (i == 8) {
                            if (nominatedPocket >= 6 || gPrediction->guiData.balls[i].pocketIndex == nominatedPocket) {
                                hasLegal = true; own++;
                            }
                        }
                    } else {
                        bool m = (myclass == Ball::Classification::ANY) ? (gPrediction->guiData.balls[i].classification != Ball::Classification::EIGHT_BALL) : (gPrediction->guiData.balls[i].classification == myclass);
                        if (m) {
                            if (nominatedPocket >= 6 || gPrediction->guiData.balls[i].pocketIndex == nominatedPocket) {
                                hasLegal = true; own++;
                            }
                        }
                    }
                }
            }

            if (!hasLegal) continue;
            auto firstHit = gPrediction->guiData.collision.firstHitBall;
            if (!firstHit) continue;

            if (onlyEightBallLeft) {
                if (firstHit->index != 8) continue;
            } else {
                if (myclass == Ball::Classification::ANY && firstHit->classification == Ball::Classification::EIGHT_BALL) continue;
                if (myclass != Ball::Classification::ANY && firstHit->classification != myclass) continue;
                if (p8) continue; // Prevent potting the 8-ball early if it's not our target
            }

            // Scratch already checked at line 395 (ball[0].onTable).
            // p8 check above already prevents accidental 8-ball pocket.
            // No additional nearScratch heuristic needed - trust simulation result.

            int currentScore = (own * 100);

            if (currentScore > bestScore) {
                int bestPottedIdx = -1;
                for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                    if (gPrediction->guiData.balls[i].originalOnTable && !gPrediction->guiData.balls[i].onTable) {
                        if (nominatedPocket < 6 && gPrediction->guiData.balls[i].pocketIndex != nominatedPocket) continue;
                        if (onlyEightBallLeft) {
                            if (i == 8) bestPottedIdx = 8;
                        } else {
                            bool m = (myclass == Ball::Classification::ANY) ? (gPrediction->guiData.balls[i].classification != Ball::Classification::EIGHT_BALL) : (gPrediction->guiData.balls[i].classification == myclass);
                            if (m) {
                                if (bestPottedIdx == -1 || i == gPrediction->guiData.collision.firstHitBall->index) bestPottedIdx = i;
                            }
                        }
                    }
                }

                if (bestPottedIdx != -1) {
                    bestScore = currentScore;
                    bestCandidate = {bestPottedIdx, angle, (double)currentScore, (int)gPrediction->guiData.balls[bestPottedIdx].pocketIndex, power};
                    if (cleanTableMode == CLEAN_YOUR_BALLS && own >= 1) { found = true; break; }
                }
            }
        }
        if (found) break;
    }
    
    if (found || currentScanAngle >= 2.0 * M_PI) {
        isScanningInProgress = false; 
        currentScanAngle = 0.0; 
        scan = FAST; 
        g_autoPlayCalculating = false;
        bShowAutoPlayLines = false;
        if (bestScore != -1) {
            g_CurrentCandidate = bestCandidate;
            Shoot(bestCandidate.angle, bestCandidate.power);
        } else {
            state = IDLE;
        }
        bestScore = -1;
    }
}



void AutoPlay::ScanFast(double angleStep) {
    if (g_CurrentCandidate.idx != -1) return;
    
    // 🔥 TAMBAHKAN INI
    if (!gPrediction || gPrediction->guiData.ballsCount == 0) {
        LOGI("HAHAHA : ScanFast skipped: ballsCount=%d", 
             gPrediction ? gPrediction->guiData.ballsCount : -1);
        return;
    }    
    bShowAutoPlayLines = !persistent_bool[O("bDisableFlicker")];
    static double fastSweepAngle = 0.0;
    
    Prediction::SceneData savedGuiData = gPrediction->guiData;
    
    auto& cueBall = gPrediction->guiData.balls[0];
    double distSq = (cueBall.initialPosition - fs.scanCuePos).square();
    
    if (!fs.isInitiated || distSq > 0.0025) {
        fs.raw.clear();
        fs.evals.clear();
        fs.evalIndex = 0;
        fs.scanCuePos = cueBall.initialPosition;
        fs.isInitiated = true;
        fastSweepAngle = 0.0;

        if (automationSpeed == SPEED_HUMAN && humanShotLocked)
            return;
        
        if (currentMode == MODE_AUTO_AIM && bAimedThisTurn) return;

        Ball::Classification myclass = sharedGameManager.getPlayerClassification();
        uint nominatedPocket = sharedGameManager.getNominatedPocket();
        bool isNineBallGame = (myclass == Ball::Classification::NINE_BALL_RULE);

        auto pockets = getPockets();

        // Calculate onlyEightBallLeft ONCE at the start of scan based on player classification
        bool onlyEightBallLeft = false;
        if (myclass == Ball::Classification::SOLID) {
            bool hasSolids = false;
            for (int k = 1; k < 8; k++) {
                if (gPrediction->guiData.balls[k].originalOnTable) { hasSolids = true; break; }
            }
            if (!hasSolids) onlyEightBallLeft = true;
        } else if (myclass == Ball::Classification::STRIPE) {
            bool hasStripes = false;
            for (int k = 9; k <= 15; k++) {
                if (gPrediction->guiData.balls[k].originalOnTable) { hasStripes = true; break; }
            }
            if (!hasStripes) onlyEightBallLeft = true;
        } else if (myclass == Ball::Classification::EIGHT_BALL) {
            onlyEightBallLeft = true;
        } else if (myclass == Ball::Classification::ANY) {
            bool hasOthers = false;
            for (int k = 1; k <= 15; k++) {
                if (k == 8) continue;
                if (gPrediction->guiData.balls[k].originalOnTable) { hasOthers = true; break; }
            }
            if (!hasOthers) onlyEightBallLeft = true;
        }

        std::vector<Candidate> directRaw;
        std::vector<Candidate> specialRaw;

        // Candidate generation
        bool bFoundLowestNumberedBall = false;
        int iFoundLowestNumberedBall = -1;
        for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
            if (isNineBallGame && bFoundLowestNumberedBall) break;
            auto& ball = gPrediction->guiData.balls[i];
            if (!ball.originalOnTable) continue;
            if (!bFoundLowestNumberedBall) {
                bFoundLowestNumberedBall = true;
                iFoundLowestNumberedBall = i;
            }
            if (!isNineBallGame) {
                bool isACandidate = false;
                if (onlyEightBallLeft) {
                    isACandidate = (i == 8);
                } else {
                    isACandidate = (myclass == Ball::Classification::ANY) ? (ball.classification != Ball::Classification::EIGHT_BALL) : (ball.classification == myclass);
                }
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

                // 1. Direct Shot
                {
                    Point2D shotLine = ghostBallPos - cueBall.initialPosition;
                    double distCueToTarget = sqrt(shotLine.square());
                    double angle = atan2(shotLine.y, shotLine.x);
                    if (angle < 0) angle += 2 * M_PI;
                    double score = distCueToTarget + distTargetToPocket;
                    double power = CalculateRequiredPower(score);
                    specialRaw.push_back({i, angle, score, pocketIdx, power});



                }

                // 2. Bank Shot (Cushion target)
                if (bCushionShot) {
                    for (int side = 0; side < 4; side++) {
                        Point2D mp;
                        switch(side) {
                            case 0: mp = {pocket.x, -134.6 - pocket.y}; break;
                            case 1: mp = {pocket.x, 134.6 - pocket.y}; break;
                            case 2: mp = {-261.6 - pocket.x, pocket.y}; break;
                            case 3: mp = {261.6 - pocket.x, pocket.y}; break;
                        }
                        Point2D toMir = mp - ball.initialPosition;
                        double distP = sqrt(toMir.square());
                        if (distP > 0.1) {
                            Point2D ghost = ball.initialPosition - (toMir * (1.0 / distP)) * (2.0 * BALL_RADIUS);
                            Point2D shot = ghost - cueBall.initialPosition;
                            double distC = sqrt(shot.square());
                            double angle = atan2(shot.y, shot.x);
                            if (angle < 0) angle += 2 * M_PI;
                            double score = distC + distP + 100.0;
                            double power = CalculateRequiredPower(distC + distP) * 1.25;
                            if (power > (double)powerMax) power = (double)powerMax;
                            if (power < (double)powerMin) power = (double)powerMin;
                            specialRaw.push_back({i, angle, score, pocketIdx, power});
;
                        }
                    }
                }

                // 3. Kick Shot (Cushion cue)
                if (bCushionShot) {
                    for (int side = 0; side < 4; side++) {
                        Point2D mg;
                        switch(side) {
                            case 0: mg = {ghostBallPos.x, -134.6 - ghostBallPos.y}; break;
                            case 1: mg = {ghostBallPos.x, 134.6 - ghostBallPos.y}; break;
                            case 2: mg = {-261.6 - ghostBallPos.x, ghostBallPos.y}; break;
                            case 3: mg = {261.6 - ghostBallPos.x, ghostBallPos.y}; break;
                        }
                        Point2D shot = mg - cueBall.initialPosition;
                        double distC = sqrt(shot.square());
                        double angle = atan2(shot.y, shot.x);
                        if (angle < 0) angle += 2 * M_PI;
                        double score = distC + distTargetToPocket + 150.0;
                        double power = CalculateRequiredPower(distC + distTargetToPocket) * 1.30;
                        if (power > (double)powerMax) power = (double)powerMax;
                        if (power < (double)powerMin) power = (double)powerMin;
                        specialRaw.push_back({i, angle, score, pocketIdx, power});
                        
                    }
                }

                // 4. Combination Shot (A -> B -> pocket)
                for (int j = 1; j < gPrediction->guiData.ballsCount; j++) {
                    if (j == i) continue;
                    auto& ballB = gPrediction->guiData.balls[j];
                    if (!ballB.originalOnTable) continue;
                    
                    bool isB_Valid = false;
                    if (isNineBallGame) {
                        isB_Valid = true; 
                    } else {
                        if (onlyEightBallLeft) {
                            isB_Valid = false;
                        } else {
                            isB_Valid = (myclass == Ball::Classification::ANY) ? 
                                         (ballB.classification != Ball::Classification::EIGHT_BALL) : 
                                         (ballB.classification == myclass);
                        }
                    }
                    if (!isB_Valid) continue;

                    Point2D toPocketB = pocket - ballB.initialPosition;
                    double distBToPocket = sqrt(toPocketB.square());
                    if (distBToPocket < 0.1) continue;
                    Point2D directionB = toPocketB * (1.0 / distBToPocket);
                    Point2D ghostB = ballB.initialPosition - directionB * (2.0 * BALL_RADIUS);
                    
                    Point2D toGhostB = ghostB - ball.initialPosition;
                    double distAToGhostB = sqrt(toGhostB.square());
                    if (distAToGhostB < 0.1) continue;
                    Point2D directionA = toGhostB * (1.0 / distAToGhostB);
                    Point2D ghostA = ball.initialPosition - directionA * (2.0 * BALL_RADIUS);
                    
                    Point2D shotLine = ghostA - cueBall.initialPosition;
                    double distCueToA = sqrt(shotLine.square());
                    double angle = atan2(shotLine.y, shotLine.x);
                    if (angle < 0) angle += 2 * M_PI;
                    double score = distCueToA + distAToGhostB + distBToPocket + 80.0;
                    double power = CalculateRequiredPower(distCueToA + distAToGhostB + distBToPocket) * 1.1;
                    if (power > (double)powerMax) power = (double)powerMax;
                    if (power < (double)powerMin) power = (double)powerMin;
                    specialRaw.push_back({i, angle, score, pocketIdx, power});

                }

                // 5. Kiss / Carom Shot (Cue -> A -> deflects to B -> pocket)
                for (int j = 1; j < gPrediction->guiData.ballsCount; j++) {
                    if (j == i) continue;
                    auto& ballB = gPrediction->guiData.balls[j];
                    if (!ballB.originalOnTable) continue;
                    
                    bool isB_Valid = false;
                    if (isNineBallGame) {
                        isB_Valid = true; 
                    } else {
                        if (onlyEightBallLeft) {
                            isB_Valid = false;
                        } else {
                            isB_Valid = (myclass == Ball::Classification::ANY) ? 
                                         (ballB.classification != Ball::Classification::EIGHT_BALL) : 
                                         (ballB.classification == myclass);
                        }
                    }
                    if (!isB_Valid) continue;

                    Point2D toPocketB = pocket - ballB.initialPosition;
                    double distBToPocket = sqrt(toPocketB.square());
                    if (distBToPocket < 0.1) continue;
                    Point2D directionB = toPocketB * (1.0 / distBToPocket);
                    Point2D ghostB = ballB.initialPosition - directionB * (2.0 * BALL_RADIUS);
                    
                    Point2D d = ghostB - ball.initialPosition;
                    double distD = sqrt(d.square());
                    if (distD < 2.0 * BALL_RADIUS) continue;

                    double ratio = (2.0 * BALL_RADIUS) / distD;
                    if (ratio > 1.0) ratio = 1.0;
                    double theta = acos(ratio);
                    double angleD = atan2(d.y, d.x);

                    for (int sign : {-1, 1}) {
                        double angleU = angleD + sign * theta;
                        Point2D u = {cos(angleU), sin(angleU)};
                        Point2D ghostA = ball.initialPosition + u * (2.0 * BALL_RADIUS);
                        
                        Point2D shotLine = ghostA - cueBall.initialPosition;
                        double distCueToA = sqrt(shotLine.square());
                        double angle = atan2(shotLine.y, shotLine.x);
                        if (angle < 0) angle += 2 * M_PI;
                        double score = distCueToA + distD + distBToPocket + 120.0;
                        double power = CalculateRequiredPower(distCueToA + distD + distBToPocket) * 1.2;
                        if (power > (double)powerMax) power = (double)powerMax;
                        if (power < (double)powerMin) power = (double)powerMin;
                        specialRaw.push_back({i, angle, score, pocketIdx, power});

                    }
                }
            }
        }

        fs.raw.clear();
        fs.raw.insert(fs.raw.end(), directRaw.begin(), directRaw.end());
        fs.raw.insert(fs.raw.end(), specialRaw.begin(), specialRaw.end());
        
        // Sort the entire combined list by score first so we prioritize the best direct AND special/difficult shots!
        std::sort(fs.raw.begin(), fs.raw.end(), [](const Candidate& a, const Candidate& b) {
            return a.score < b.score;
        });
        
        // Limit candidates to evaluate (take top 60 for rich search space including cushions/combos)
        if (fs.raw.size() > 60) {
            fs.raw.resize(60);
        }

        // Sort final raw candidates by angle for smooth sequential guidelines sweep
        std::sort(fs.raw.begin(), fs.raw.end(), [](const Candidate& a, const Candidate& b) {
            return a.angle < b.angle;
        });
    }

    if (automationSpeed == SPEED_HUMAN && humanShotLocked) return;

    Ball::Classification myclass = sharedGameManager.getPlayerClassification();
    uint nominatedPocket = sharedGameManager.getNominatedPocket();
    bool isNineBallGame = (myclass == Ball::Classification::NINE_BALL_RULE);

    // Calculate onlyEightBallLeft for evaluation
    bool onlyEightBallLeft = false;
    if (myclass == Ball::Classification::SOLID) {
        bool hasSolids = false;
        for (int k = 1; k < 8; k++) {
            if (gPrediction->guiData.balls[k].originalOnTable) { hasSolids = true; break; }
        }
        if (!hasSolids) onlyEightBallLeft = true;
    } else if (myclass == Ball::Classification::STRIPE) {
        bool hasStripes = false;
        for (int k = 9; k <= 15; k++) {
            if (gPrediction->guiData.balls[k].originalOnTable) { hasStripes = true; break; }
        }
        if (!hasStripes) onlyEightBallLeft = true;
    } else if (myclass == Ball::Classification::EIGHT_BALL) {
        onlyEightBallLeft = true;
    } else if (myclass == Ball::Classification::ANY) {
        bool hasOthers = false;
        for (int k = 1; k <= 15; k++) {
            if (k == 8) continue;
            if (gPrediction->guiData.balls[k].originalOnTable) { hasOthers = true; break; }
        }
        if (!hasOthers) onlyEightBallLeft = true;
    }

    int stepsInThisFrame = 0;
    const int maxStepsPerFrame = 1; // 1 candidate evaluation per frame = completely lag-free & smooth!

    while (stepsInThisFrame < maxStepsPerFrame && fs.evalIndex < fs.raw.size()) {
        auto raw = fs.raw[fs.evalIndex++];
        stepsInThisFrame++;

        double angle = NumberUtils::normalizeDoublePrecision(normalizeAngle(raw.angle));
        int originalIdx = raw.idx;
        
        bool evaluatedValid = false;
        double testPower = (automationSpeed == SPEED_FAST) ? (double)powerMax : raw.power;
        
        // Try testPower (powerMax in Fast Mode) first
        
        gPrediction->determineShotResult(true, angle, testPower, sharedGameManager.getShotSpin(), raw);
        
        
        bool isPotted = false;
        if (isNineBallGame) {
            for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                if (gPrediction->guiData.balls[i].originalOnTable && !gPrediction->guiData.balls[i].onTable) {
                    isPotted = true; break;
                }
            }
        } else {
            if (onlyEightBallLeft) {
                isPotted = !gPrediction->guiData.balls[8].onTable && (nominatedPocket >= 6 || gPrediction->guiData.balls[8].pocketIndex == nominatedPocket);
                if (isPotted) {
                    raw.idx = 8;
                }
            } else {
                for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                    if (i == 8) continue;
                    bool match = (myclass == Ball::Classification::ANY) ? 
                                 (gPrediction->guiData.balls[i].classification != Ball::Classification::EIGHT_BALL) : 
                                 (gPrediction->guiData.balls[i].classification == myclass);
                    if (match && !gPrediction->guiData.balls[i].onTable) {
                        if (nominatedPocket >= 6 || gPrediction->guiData.balls[i].pocketIndex == nominatedPocket) {
                            isPotted = true;
                            raw.idx = i;
                            break;
                        }
                    }
                }
            }
        }

        if (gPrediction->guiData.collision.firstHitBall && gPrediction->guiData.balls[0].onTable && isPotted) {
            // SCRATCH CHECK: Simulation directly tells us if cue ball was potted (onTable=false).
            // Trust the simulation result - no need for unreliable path-proximity heuristic.
            // The path-proximity check (81.0 threshold) was causing false positives AND false negatives.
            raw.power = testPower;
            evaluatedValid = true;
        }
        
        // Fallback to calculated power if testPower failed and we are in fast mode
        if (!evaluatedValid && automationSpeed == SPEED_FAST) {
            raw.idx = originalIdx;
            
            gPrediction->determineShotResult(true, angle, raw.power, sharedGameManager.getShotSpin(), raw);
            
            
            bool isPottedFallback = false;
            if (isNineBallGame) {
                for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                    if (gPrediction->guiData.balls[i].originalOnTable && !gPrediction->guiData.balls[i].onTable) {
                        isPottedFallback = true; break;
                    }
                }
            } else {
                if (onlyEightBallLeft) {
                    isPottedFallback = !gPrediction->guiData.balls[8].onTable && (nominatedPocket >= 6 || gPrediction->guiData.balls[8].pocketIndex == nominatedPocket);
                    if (isPottedFallback) {
                        raw.idx = 8;
                    }
                } else {
                    for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                        if (i == 8) continue;
                        bool match = (myclass == Ball::Classification::ANY) ? 
                                     (gPrediction->guiData.balls[i].classification != Ball::Classification::EIGHT_BALL) : 
                                     (gPrediction->guiData.balls[i].classification == myclass);
                        if (match && !gPrediction->guiData.balls[i].onTable) {
                            if (nominatedPocket >= 6 || gPrediction->guiData.balls[i].pocketIndex == nominatedPocket) {
                                isPottedFallback = true;
                                raw.idx = i;
                                break;
                            }
                        }
                    }
                }
            }

            if (gPrediction->guiData.collision.firstHitBall && gPrediction->guiData.balls[0].onTable && isPottedFallback) {
                // SCRATCH CHECK: Same as primary - trust simulation directly.
                evaluatedValid = true;
            }
        }

        if (!evaluatedValid) continue;

        // ==================== 9-BALL GAME ====================
        if (isNineBallGame) {
            auto firstHit = gPrediction->guiData.collision.firstHitBall;
            if (!firstHit) continue;
            if (firstHit->index != raw.idx) continue;

            int bestPottedIdx = -1;
            for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                auto& ball = gPrediction->guiData.balls[i];
                if (ball.originalOnTable && !ball.onTable) {
                    if (nominatedPocket < 6 && ball.pocketIndex != nominatedPocket) continue;
                    if (i == 9) { bestPottedIdx = 9; break; }
                    if (bestPottedIdx == -1 || i == raw.idx) bestPottedIdx = i;
                }
            }
            if (bestPottedIdx == -1) continue;
            int effectiveTargetIdx = bestPottedIdx;
            if (nominatedPocket < 6 && gPrediction->guiData.balls[effectiveTargetIdx].pocketIndex != nominatedPocket) continue;

            int potCount = 0;
            bool pots9 = false;
            for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                auto& ball = gPrediction->guiData.balls[i];
                if (ball.originalOnTable && !ball.onTable) {
                    potCount++;
                    if (i == 9) pots9 = true;
                }
            }
            
            Candidate cf = raw;
            cf.idx = effectiveTargetIdx;
            cf.pocketIndex = gPrediction->guiData.balls[effectiveTargetIdx].pocketIndex;
            fs.evals.push_back({cf, potCount, potCount, pots9});
            continue;
        }

        // ==================== STANDARD 8-BALL GAME ====================
        // Find which ball of our group was actually potted in the simulation
        int pottedBallIdx = -1;
        for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
            Prediction::Ball& ball = gPrediction->guiData.balls[i];
            if (ball.originalOnTable && !ball.onTable) {
                bool match = false;
                if (onlyEightBallLeft) {
                    if (i == 8) match = true;
                } else {
                    match = (myclass == Ball::Classification::ANY) ?
                            (ball.classification != Ball::Classification::CUE_BALL &&
                             ball.classification != Ball::Classification::EIGHT_BALL) :
                            (ball.classification == myclass);
                }
                if (match) {
                    pottedBallIdx = i;
                    break;
                }
            }
        }

        if (pottedBallIdx == -1) continue; // No legal ball potted

        // CRITICAL: Simulation must confirm ball went to the SAME pocket we geometrically aimed for.
        if (gPrediction->guiData.balls[pottedBallIdx].pocketIndex != raw.pocketIndex) continue;
        
        // If pocket already nominated, simulation must also match the nominated pocket
        if (nominatedPocket < 6 && gPrediction->guiData.balls[pottedBallIdx].pocketIndex != nominatedPocket) continue;

        int totalPotted = 0;
        int ownPotted = 0;
        bool hasLegalPot = false;
        bool eightBallPotted = false;
        
        for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
            Prediction::Ball& ball = gPrediction->guiData.balls[i];
            if (ball.originalOnTable && !ball.onTable) {
                totalPotted++;
                bool match = (myclass == Ball::Classification::ANY) ?
                             (ball.classification != Ball::Classification::CUE_BALL &&
                              ball.classification != Ball::Classification::EIGHT_BALL) :
                              (ball.classification == myclass);
                if (match) {
                    hasLegalPot = true;
                    ownPotted++;
                }
                if (i == 8) eightBallPotted = true;
            }
        }
        
        if (onlyEightBallLeft) {
            if (eightBallPotted) {
                hasLegalPot = true;
                ownPotted = 1;
                totalPotted = 1;
            } else {
                hasLegalPot = false;
            }
        }
        
        if (!hasLegalPot) continue;

        auto firstHit = gPrediction->guiData.collision.firstHitBall;
        if (firstHit) {
            if (myclass != Ball::Classification::ANY && firstHit->classification != myclass) {
                if (!onlyEightBallLeft || firstHit->index != 8) continue;
            }
            if (myclass == Ball::Classification::ANY && firstHit->classification == Ball::Classification::EIGHT_BALL) {
                if (!onlyEightBallLeft) continue;
            }
        }
        // SAFETY: Cue ball scratch - skip any shot that pockets the cue ball
        if (!gPrediction->guiData.balls[0].onTable) continue;
        // SAFETY: Accidental 8-ball pocket - skip if 8-ball potted AND it's NOT supposed to be
        // This catches combination shots where our ball deflects and accidentally pots the 8-ball
        if (eightBallPotted && !onlyEightBallLeft) continue;

        Candidate cf = raw;
        cf.idx = pottedBallIdx;
        cf.pocketIndex = gPrediction->guiData.balls[pottedBallIdx].pocketIndex;

        // GENIUS MODE: If we are in standard play and find a valid direct shot, shoot immediately!
        if (!isNineBallGame && (cleanTableMode == CLEAN_OFF || onlyEightBallLeft)) {
            fs.isInitiated = false;
            g_CurrentCandidate = cf;
            Shoot(cf.angle, cf.power);
            return;
        }

        fs.evals.push_back({cf, totalPotted, ownPotted, false});
    }

    gPrediction->guiData = savedGuiData;

    if (fs.evalIndex >= fs.raw.size()) {
        FastScanState::Eval* best = nullptr;
        
        if (isNineBallGame) {
            switch (nineBallStrategy) {
                case NINEBALL_BEST_SHOT:
                    for (auto& ev : fs.evals) {
                        if (!best || ev.tot > best->tot) best = &ev;
                    }
                    break;
                case NINEBALL_SNIPE_9:
                    for (auto& ev : fs.evals) {
                        if (!best) best = &ev;
                        else {
                            if (ev.p9 && !best->p9) best = &ev;
                            else if (ev.p9 == best->p9 && ev.tot > best->tot) best = &ev;
                        }
                    }
                    break;
                default:
                    if (!fs.evals.empty()) best = &fs.evals[0];
                    break;
            }
        } else {
            if (onlyEightBallLeft) {
                for (auto& ev : fs.evals) {
                    if (ev.c.idx == 8) { best = &ev; break; }
                }
                if (!best && !fs.evals.empty()) best = &fs.evals[0];
            } else {
                switch (cleanTableMode) {
                    case CLEAN_OFF:
                        if (!fs.evals.empty()) best = &fs.evals[0];
                        break;
                    case CLEAN_ALL_BALLS:
                        for (auto& ev : fs.evals) {
                            if (!best || ev.tot > best->tot) best = &ev;
                        }
                        break;
                    case CLEAN_YOUR_BALLS:
                        for (auto& ev : fs.evals) {
                            if (!best || ev.own > best->own) best = &ev;
                        }
                        break;
                }
            }
        }

        if (best) {
            fs.isInitiated = false;
            g_CurrentCandidate = best->c;
            Shoot(best->c.angle, best->c.power);
        } else {
            fs.isInitiated = false;
            // BUG FIX #1: jangan set lastFailedCuePos di sini.
            // Kalau di-set, IDLE block frame berikutnya akan cek distToFailed=0
            // → shouldScan=false → ScanSlow tidak pernah jalan → stuck di IDLE.
            // Set state=SCANNING eksplisit supaya ScanSlow langsung jalan frame berikutnya
            // tanpa harus lewat IDLE→shouldScan check dulu.
            scan = SLOW;
            state = SCANNING;
            g_autoPlayCalculating = true;
        }
    }

    // Render smooth rotating radar sweep line during fast scan
    if (fs.evalIndex < fs.raw.size()) {
        fastSweepAngle = normalizeAngle(fastSweepAngle + 0.15);
        if (!persistent_bool[O("bDisableFlicker")]) {
            
            gPrediction->determineShotResult(true, fastSweepAngle, 400.0f, sharedGameManager.getShotSpin());
            
        }
        sweepAngle = fastSweepAngle;
    }
}

void AutoPlay::Update() {
    frameCounter++;
    buttonClicker.Update();
    powerSlider.Update();

    g_lastFastShotTime = 0.0;
    g_shotCooldownEnd = 0.0;

    // Safety check
    if (!sharedGameManager || !gPrediction) {
        LOGI("HAHAHA : Update skipped: sharedGameManager or gPrediction null");
        return;
    }
    if (!sharedGameManager.mTable) {
        LOGI("HAHAHA :Update skipped: mTable null");
        return;
    }

    // Track cue ball movement/dragging (ball-in-hand)
    static Point2D lastFrameCuePos = {-1000.0, -1000.0};
    static int framesCueBallStill = 10;
    Point2D currentCuePos = {0.0, 0.0};
    bool hasCueBall = false;
    if (sharedGameManager) {
        Table table = sharedGameManager.mTable;
        if (table) {
            auto& balls = table.mBalls();
            if (balls && balls.Count > 0) {
                currentCuePos = balls[0].position();
                hasCueBall = true;
            }
        }
    }
    if (hasCueBall) {
        if (lastFrameCuePos.x == -1000.0) lastFrameCuePos = currentCuePos;
        double dx = currentCuePos.x - lastFrameCuePos.x;
        double dy = currentCuePos.y - lastFrameCuePos.y;
        if (dx*dx + dy*dy > 0.0001) framesCueBallStill = 0;
        else if (framesCueBallStill < 10) framesCueBallStill++;
        lastFrameCuePos = currentCuePos;
    } else {
        framesCueBallStill = 10;
        lastFrameCuePos = {-1000.0, -1000.0};
    }
    bCueBallIsMovingOrDragging = (framesCueBallStill < 5);

    if (g_postShotLock) {
        static bool wasPlayerTurnInLock = false;
        bool isPlayerTurnNow = sharedGameManager.mStateManager().isPlayerTurn();
        bool turnJustStartedInLock = !wasPlayerTurnInLock && isPlayerTurnNow;
        if (turnJustStartedInLock) {
            lastFailedCuePos = {-1000.0, -1000.0};
            bAimedThisTurn = false;
        }
        wasPlayerTurnInLock = isPlayerTurnNow;

        if (g_postShotFrames > 0 && sharedGameManager) {
            setAimAngle(g_postShotAngle);
            setPower(g_postShotPower);
            g_postShotFrames--;
        } else {
            // Release lock — tidak panggil ClearState() biar tidak loop
            g_postShotLock = false;
            state = IDLE;
        }
        g_autoPlayCalculating = false;
        return;
    }

    if (g_postAimLock) {
        if (g_postAimFrames > 0 && sharedGameManager) {
            setAimAngle(g_postAimAngle);
            setPower(g_postAimPower);
            g_postAimFrames--;
        } else {
            g_postAimLock = false;
            ClearState();
        }
        g_autoPlayCalculating = false;
        return;
    }

    bool humanRunning = (automationSpeed == SPEED_HUMAN && (humanState != HUM_IDLE || humanShotLocked));
    bool executingShot = anim_IsPulling || humanRunning;

    if (AreBallsMoving() && !executingShot) {
        if (state == SCANNING || state == NOMINATING) {
            ClearState();
            state = IDLE;
        }
        g_autoPlayCalculating = false;
        return;
    }

    // Periodic ruleset logging for debugging/validation
    if (sharedGameManager && frameCounter % 120 == 0) {
        auto rules = sharedGameManager._rules();
        if (rules) {
            LOGI("Ruleset State: 0x58=%d, 0x108=%d, 0x112=%d, 0x113=%d, 0x114=%d, 0x128=%d",
                 F(bool, rules + 0x58), F(bool, rules + 0x108), F(bool, rules + 0x112),
                 F(bool, rules + 0x113), F(bool, rules + 0x114), F(bool, rules + 0x128));
        }
    }

    // --- ANIMATION FIRST (FAST MODE VISUALS) ---
    if (anim_IsPulling) {
        float jX = Width * 0.83f; 
        float jY = Height * 0.82f; 
        float jR = 65.0f;

        double now_anim = nowSec();
        double elapsed = now_anim - stateStartTime;

        // Timing for each phase (total ~1.60s)
        const double t1_pullback = 0.20; // Phase 1: Fast pullback (opposite 30 deg)
        const double t2_sweep    = 0.75; // Phase 2: Smooth slide to overshoot (past target 20 deg)
        const double t3_correct  = 1.00; // Phase 3: Come back to nudge angle (1.5 deg short of target)
        const double t4_adjust   = 1.40; // Phase 4: Slow human adjustment/nudge to exact target
        const double t5_hold     = 1.60; // Phase 5: Hold touch at target for 0.20s

        // State 0: Rotation animation (joystick sweep)
        if (fastShotState == 0) {
            if (playStyle == STYLE_INSTANT) {
                // Instant Joystick Lock
                setAimAngle(anim_TargetAngle);
                NativeTouchesBegin(5, jX, jY);
                NativeTouchesMove(5, jX + (float)cos(anim_TargetAngle) * jR, 
                                     jY + (float)sin(anim_TargetAngle) * jR);
                anim_RotationDone = true;
                anim_TouchStarted = true;
                stateStartTime = nowSec();
                fastShotState = 1;
                return;
            }

            double normalizedStart = normalizeAngle(startAngle);
            double normalizedTarget = normalizeAngle(anim_TargetAngle);
            double delta = normalizedTarget - normalizedStart;
            if (delta > M_PI)  delta -= 2.0 * M_PI;
            if (delta < -M_PI) delta += 2.0 * M_PI;

            double dir = (delta > 0) ? 1.0 : -1.0;

            // Angles
            double oppositeAngle  = normalizedStart - dir * (30.0 * M_PI / 180.0);
            double overshootAngle = normalizedTarget + dir * (20.0 * M_PI / 180.0);
            double nudgeAngle     = normalizedTarget - dir * (1.5 * M_PI / 180.0); // 1.5 degrees nudge offset

            double curAngle = normalizedTarget;

            if (elapsed < t1_pullback) {
                // PHASE 1: Pullback to OPPOSITE side (30 degrees)
                double t = elapsed / t1_pullback;
                t = 1.0 - pow(1.0 - t, 3.0); // Ease-out
                curAngle = normalizedStart + (oppositeAngle - normalizedStart) * t;
                
                if (!anim_TouchStarted) {
                    anim_TouchStarted = true;
                    NativeTouchesBegin(5, jX, jY);
                }
            } else if (elapsed < t2_sweep) {
                // PHASE 2: Smoothly sweep from opposite past the target (20 deg overshoot)
                double t = (elapsed - t1_pullback) / (t2_sweep - t1_pullback);
                t = t * t * (3.0 - 2.0 * t); // Smoothstep for very smooth motion
                curAngle = oppositeAngle + (overshootAngle - oppositeAngle) * t;
            } else if (elapsed < t3_correct) {
                // PHASE 3: Correct back from overshoot to nudgeAngle (1.5 deg short of target)
                double t = (elapsed - t2_sweep) / (t3_correct - t2_sweep);
                t = t * t * (3.0 - 2.0 * t); // Smoothstep
                curAngle = overshootAngle + (nudgeAngle - overshootAngle) * t;
            } else if (elapsed < t4_adjust) {
                // PHASE 4: Slow human adjustment/nudge to exact target
                double t = (elapsed - t3_correct) / (t4_adjust - t3_correct);
                t = sin(t * M_PI_2); // Ease-out to slow down at the very end
                curAngle = nudgeAngle + (normalizedTarget - nudgeAngle) * t;
            } else if (elapsed < t5_hold) {
                // PHASE 5: Hold touch static at target angle
                curAngle = normalizedTarget;
                if (!anim_RotationDone) {
                    if (elapsed > t5_hold - 0.05) {
                        anim_RotationDone = true;
                        setAimAngle(anim_TargetAngle);
                    }
                }
            }

            if (elapsed < t5_hold) {
                setAimAngle(curAngle);
                NativeTouchesMove(5, jX + (float)cos(curAngle) * jR, 
                                     jY + (float)sin(curAngle) * jR);
                return; // Continue animation next frame
            }

            // Joystick sweep completed! Snap aim to exact target.
            // DO NOT release joystick yet - keep it held at target angle to prevent
            // game from resetting aim direction during power pull phase.
            setAimAngle(anim_TargetAngle);
            NativeTouchesMove(5, jX + (float)cos(anim_TargetAngle) * jR, 
                                 jY + (float)sin(anim_TargetAngle) * jR);
            stateStartTime = nowSec();
            fastShotState = 1; // Transition to STABILIZE phase (joystick still held!)
            return;
        }

        // Keep target angle locked in memory
        setAimAngle(anim_TargetAngle);

        double elapsed_shot = nowSec() - stateStartTime;

        // State 1: STABILIZE PHASE (0.15 seconds) - hold joystick at target, then start power pull
      /*  if (fastShotState == 1) {
    // Keep joystick held at EXACT target angle during stabilization.
    NativeTouchesMove(5, jX + (float)cos(anim_TargetAngle) * jR, 
                         jY + (float)sin(anim_TargetAngle) * jR);
    setAimAngle(anim_TargetAngle);

    bool shouldTriggerPower = false;
    if (playStyle == STYLE_INSTANT) {
        shouldTriggerPower = true;
    } else if (elapsed_shot >= 0.15) {
        shouldTriggerPower = true;
    }

    if (shouldTriggerPower) {
        // Release joystick RIGHT before power slider starts.
        NativeTouchesEnd(5, jX + (float)cos(anim_TargetAngle) * jR, 
                            jY + (float)sin(anim_TargetAngle) * jR);

        // 🔥 SET POWER LANGSUNG (TANPA SLIDER)
        setPower(anim_TargetPower);

        stateStartTime = nowSec();
        fastShotState = 2;
    }
    return;
        }*/
        
        if (fastShotState == 1) {
            // Keep joystick held at EXACT target angle during stabilization.
            // This prevents the game from resetting aim direction.
            NativeTouchesMove(5, jX + (float)cos(anim_TargetAngle) * jR, 
                                 jY + (float)sin(anim_TargetAngle) * jR);
            setAimAngle(anim_TargetAngle);

            bool shouldTriggerPower = false;
            if (playStyle == STYLE_INSTANT) {
                shouldTriggerPower = true;
            } else if (elapsed_shot >= 0.15) {
                shouldTriggerPower = true;
            }

            if (shouldTriggerPower) {
                // Release joystick RIGHT before power slider starts.
                // Minimal gap between joystick release and power pull to prevent aim reset.
                NativeTouchesEnd(5, jX + (float)cos(anim_TargetAngle) * jR, 
                                    jY + (float)sin(anim_TargetAngle) * jR);

                float sliderXPercent = persistent_float[O("fPowerBarXPercent")];
                float sliderX = Width * sliderXPercent;
                if (persistent_int[O("iPowerBarSide")] == 1) {
                    sliderX = Width * (1.0f - sliderXPercent); // Right Side
                }
                float sliderYStart = Height * persistent_float[O("fPowerBarYStartPercent")];
                float sliderYEnd = Height * persistent_float[O("fPowerBarYEndPercent")];
                ImVec4 sliderRect(sliderX - 20.0f, sliderYStart, 40.0f, sliderYEnd - sliderYStart);
                if (playStyle == STYLE_INSTANT) {
                    powerSlider.SimulateDrag(sliderRect, anim_TargetPower, 0.40f, 0.20f);
                } else {
                    powerSlider.SimulateDrag(sliderRect, anim_TargetPower, 0.85f, 0.40f);
                }

                stateStartTime = nowSec();
                fastShotState = 2; // Transition to wait-for-slider phase
            }
            return;
        }*/

     /*1   if (fastShotState == 2) {
    
    gPrediction->determineShotResult(true, anim_TargetAngle, anim_TargetPower,
                                     sharedGameManager.getShotSpin(), g_CurrentCandidate);
    

    // 🔥 TANPA CEK powerSlider.Active — langsung lanjut
    stateStartTime = nowSec();
    fastShotState = 3;
    return;
        }*/

        // State 2: Wait for power slider to complete (slider already started in state 1)
        if (fastShotState == 2) {
           // LOGI("[AUTOPLAY] fastShotState 2: powerSlider.Active=%d", powerSlider.Active);
            
            gPrediction->determineShotResult(true, anim_TargetAngle, anim_TargetPower,
                                             sharedGameManager.getShotSpin(), g_CurrentCandidate);
            

           if (powerSlider.Active) {
                return; // Wait for slider simulation to finish and release touch
            }

            stateStartTime = nowSec();
            fastShotState = 3;
            return;
        }

        // State 3: WAIT FOR BALLS TO STOP
        if (fastShotState == 3) {
            setAimAngle(anim_TargetAngle);

            static double s_ballsStoppedAt = -1.0;
            // Reset tracker kalau baru masuk state ini
            if (s_ballsStoppedAt < stateStartTime) {
                s_ballsStoppedAt = nowSec();
            }

            bool timedOut = (nowSec() - stateStartTime > 10.0);

            if (AreBallsMoving() && !timedOut) {
                s_ballsStoppedAt = nowSec(); // bola masih gerak, reset settled timer
                return;
            }

            // Tunggu 0.3s setelah bola berhenti sebelum lanjut
            double settledFor = nowSec() - s_ballsStoppedAt;
            if (settledFor < 0.3 && !timedOut) return;

            triggerShot();  // ← TAMBAHKAN INI!

            // Beres
            s_ballsStoppedAt = -1.0;
            anim_IsPulling = false;
            anim_RotationDone = false;
            anim_TouchStarted = false;
            fastShotState = 0;
            g_lastFastShotTime = nowSec();
            // Set cooldown singkat biar tidak langsung scan ulang
            g_shotCooldownEnd = nowSec() + 0.5;
            state = IDLE;
            g_CurrentCandidate.idx = -1;
            return;
        }
    }

    // SPIDERENGINE PREMIUM NOMINATED POCKET VISUAL
    if (persistent_bool.count(O("bPocketTargetVisual")) == 0 || persistent_bool[O("bPocketTargetVisual")]) {
        int nomPocket = sharedGameManager.getNominatedPocket();
        if (nomPocket >= 0 && nomPocket < 6) {
            ImVec2 pktPos = GetPocketScreenPos(nomPocket);
            ImDrawList* fg = ImGui::GetBackgroundDrawList(); // Draw behind UI but over game
            float pulse = (sin(ImGui::GetTime() * 8.0f) + 1.0f) * 0.5f; // Pulsing 0.0 to 1.0
            float r = 35.0f + (pulse * 8.0f);
            
            // Glowing Base
            fg->AddCircleFilled(pktPos, r, IM_COL32(255, 120, 0, 70));
            // Outer Bright Ring
            fg->AddCircle(pktPos, r, IM_COL32(255, 200, 0, 255), 0, 3.5f);
            
            // Spider Target Crosshair
            fg->AddLine(ImVec2(pktPos.x - 18, pktPos.y), ImVec2(pktPos.x + 18, pktPos.y), IM_COL32(255, 255, 255, 180), 2.5f);
            fg->AddLine(ImVec2(pktPos.x, pktPos.y - 18), ImVec2(pktPos.x, pktPos.y + 18), IM_COL32(255, 255, 255, 180), 2.5f);
        }
    }

    static bool wasPlayerTurn = false;
    bool isPlayerTurn = sharedGameManager.mStateManager().isPlayerTurn();
    if (isPlayerTurn && bAutoSpin) applyAutoSpin();
    
    bool turnJustStarted = !wasPlayerTurn && isPlayerTurn; // detect fresh turn beginning
    if (wasPlayerTurn && !isPlayerTurn) { g_autoPlayCalculating = false; ClearState(); bAimedThisTurn = false; }
    if (turnJustStarted) { bAimedThisTurn = false; lastFailedCuePos = {-1000.0, -1000.0}; }
    wasPlayerTurn = isPlayerTurn;

    static double turnStartTime = 0.0;
    if (turnJustStarted || (isPlayerTurn && turnStartTime == 0.0)) {
        turnStartTime = nowSec();
    }
    if (!isPlayerTurn) {
        turnStartTime = 0.0;
    }

    bool humanActive = (automationSpeed == SPEED_HUMAN && humanState != HUM_IDLE);
    
    // --- Break Shot Optimizer ---
    bool isBreakPosition = false;
    if (gPrediction->guiData.ballsCount >= 15) {
        int racked = 0;
        for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
            auto& b = gPrediction->guiData.balls[i];
            if (b.initialPosition.x < 70.0 || b.initialPosition.x > 120.0) racked++;
        }
        if (racked >= 13) isBreakPosition = true;
    }

    static int animationStuckCounter = 0;
    humanRunning = (automationSpeed == SPEED_HUMAN && (humanState != HUM_IDLE || humanShotLocked));
    if (IsAnimationActive() && !humanRunning && currentMode != MODE_AUTO_AIM && !isBreakPosition) {
        animationStuckCounter++;
        if (animationStuckCounter < 200) { 
            g_autoPlayCalculating = false; return;
        }
    } else {
        animationStuckCounter = 0;
    }

    // SHOT COOLDOWN: Prevent double-shot by blocking scan for 2.5s after firing
    if (nowSec() - g_lastFastShotTime < 2.5) {
        g_autoPlayCalculating = false;
        return;
    }

    // SHOT COOLDOWN: Don't scan/execute for 2s after firing to prevent stuck state
    if (AutoPlay::nowSec() < g_shotCooldownEnd) {
        g_autoPlayCalculating = false;
        return;
    }

    // STATE TIMEOUT SAFETY: If stuck in any state for > 10s, force reset
    static double lastStateChangeTime = 0;
    static State lastState = IDLE;
    if (state != lastState) {
        lastState = state;
        lastStateChangeTime = AutoPlay::nowSec();
    } else if (state != IDLE && (AutoPlay::nowSec() - lastStateChangeTime > 10.0)) {
        ClearState();
        return;
    }

    // Force re-scan when player's turn freshly begins
    if (turnJustStarted && bAutoPlaying) {
        state = IDLE;
        scan = FAST;
        currentScanAngle = 0.0;
    }

    // =====================================================================
    // HUMAN STATE MACHINE - Must run FIRST, before any animation checks!
    // =====================================================================
    if (automationSpeed == SPEED_HUMAN && humanState != HUM_IDLE) {
        if (state == NOMINATING_HUMAN) {
            nominationFrameCounter++;
            if (nominationFrameCounter == 15) buttonClicker.Click(GetPocketScreenPos(humanNominationPocket));
            if (nominationFrameCounter > 35 && !buttonClicker.Active) {
                humanState = HUM_THINKING; 
                stateStartTime = nowSec() + 0.35;
                state = EXECUTING; humanNeedsNomination = false;
            }
            return;
        }

        double now = nowSec();

        auto UpdateJoystickVisuals = [&](double angle) {
            float jX = Width * 0.83f;
            float jY = Height * 0.82f;
            float jR = 65.0f;
            float tX = jX + cos(angle) * jR;
            float tY = jY + sin(angle) * jR;
            NativeTouchesMove(5, tX, tY);
        };

        // 1. THINKING (0.5s pause)
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
            
            gPrediction->determineShotResult(true, targetAngle, pendingShotPower, sharedGameManager.getShotSpin(), g_CurrentCandidate);
            
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
            
            gPrediction->determineShotResult(true, targetAngle, pendingShotPower, sharedGameManager.getShotSpin(), g_CurrentCandidate);
            
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
            
            gPrediction->determineShotResult(true, targetAngle, pendingShotPower, sharedGameManager.getShotSpin(), g_CurrentCandidate);
            
            return;
        }

        // 4. STABILIZE & LOCK (0.4s) - joystick still held from HUM_HOLDING
        if (humanState == HUM_STABILIZING) {
            float jX = Width * 0.83f;
            float jY = Height * 0.82f;
            float jR = 65.0f;
            // Keep joystick actively held at target angle during stabilization.
            // This ensures game always has native touch direction = targetAngle.
            NativeTouchesMove(5, jX + (float)cos(targetAngle) * jR, 
                                 jY + (float)sin(targetAngle) * jR);
            setAimAngle(targetAngle);
            if (now - stateStartTime >= 0.4) {
                if (currentMode == MODE_AUTO_PLAY) {
                    // Release joystick RIGHT when transitioning to power pull.
                    // Minimal gap between release and power start prevents aim reset.
                    NativeTouchesEnd(5, jX + (float)cos(targetAngle) * jR, 
                                        jY + (float)sin(targetAngle) * jR);
                    stateStartTime = now;
                    startPower = getCurrentPower();
                    targetPower = pendingShotPower;
                    humanState = HUM_PULLING;
                } else {
                    NativeTouchesEnd(5, jX + (float)cos(targetAngle) * jR, 
                                        jY + (float)sin(targetAngle) * jR);
                    bAimedThisTurn = true;
                    lastCuePosWhenAimed = gPrediction->guiData.balls[0].initialPosition;
                    g_postAimLock = true;
                    g_postAimAngle = targetAngle;
                    g_postAimPower = pendingShotPower;
                    g_postAimFrames = 20; // Hold for 20 frames to stabilize and prevent reset
                    state = IDLE; humanState = HUM_IDLE;
                }
            }
            return;
        }

        // 5. POWER PULL (0.85s smooth) via simulated slider touch
        /*if (humanState == HUM_PULLING) {
            setAimAngle(targetAngle);
            if (!powerSlider.Active) {
                float sliderXPercent = persistent_float[O("fPowerBarXPercent")];
                float sliderX = Width * sliderXPercent;
                if (persistent_int[O("iPowerBarSide")] == 1) {
                    sliderX = Width * (1.0f - sliderXPercent); // Right Side
                }
                float sliderYStart = Height * persistent_float[O("fPowerBarYStartPercent")];
                float sliderYEnd = Height * persistent_float[O("fPowerBarYEndPercent")];
                ImVec4 sliderRect(sliderX - 20.0f, sliderYStart, 40.0f, sliderYEnd - sliderYStart);

                powerSlider.SimulateDrag(sliderRect, targetPower, 0.85f, 0.4f);
            }

            
            gPrediction->determineShotResult(true, targetAngle, targetPower,
                                             sharedGameManager.getShotSpin(), g_CurrentCandidate);
            

            if (powerSlider.Active) {
                return; // Wait for slider simulation to finish and release touch
            }

            stateStartTime = now;
            humanState = HUM_DELAY_BEFORE_SHOT;
            return;
        }*/

        if (humanState == HUM_PULLING) {
    setAimAngle(targetAngle);

    // 🔥 Mulai slider (kalau belum aktif)
    if (!powerSlider.Active) {
        float sliderXPercent = persistent_float[O("fPowerBarXPercent")];
        float sliderX = Width * sliderXPercent;
        if (persistent_int[O("iPowerBarSide")] == 1) {
            sliderX = Width * (1.0f - sliderXPercent);
        }
        float sliderYStart = Height * persistent_float[O("fPowerBarYStartPercent")];
        float sliderYEnd = Height * persistent_float[O("fPowerBarYEndPercent")];
        ImVec4 sliderRect(sliderX - 20.0f, sliderYStart, 40.0f, sliderYEnd - sliderYStart);

        powerSlider.SimulateDrag(sliderRect, targetPower, 0.85f, 0.4f);
    }

    // 🔥 Tunggu slider selesai DULU
    if (powerSlider.Active) {
        return; // slider masih jalan, tunggu
    }

    // 🔥 Simulasi prediksi SETELAH slider selesai
    
    gPrediction->determineShotResult(true, targetAngle, targetPower,
                                     sharedGameManager.getShotSpin(), g_CurrentCandidate);
    

    stateStartTime = now;
    humanState = HUM_DELAY_BEFORE_SHOT;
    return;
        }

        // 6. FINAL HUMAN PAUSE (0.4s) then FIRE!
        if (humanState == HUM_DELAY_BEFORE_SHOT) {
    setAimAngle(targetAngle);
    if (now - stateStartTime >= 0.4) {
        // 🔥 TAMBAHKAN INI!
        triggerShot();

        humanShotLocked = false;
        ClearState();
        state = IDLE;
        humanState = HUM_IDLE;
    }
    return;
        }
    }

    // ABORT HANDLER: If user turns off AutoPlay or turn ends
    if (!bAutoPlaying || !isPlayerTurn) {
        if (humanShotLocked || anim_IsPulling || state == SCANNING || state == NOMINATING) {
            if (humanState == HUM_OVERSHOOTING || humanState == HUM_CORRECTING || humanState == HUM_HOLDING || humanState == HUM_STABILIZING) {
                float jX = Width * 0.83f;
                float jY = Height * 0.82f;
                NativeTouchesEnd(5, jX, jY);
            }
            
            if (powerSlider.Active) {
                float sliderXPercent = persistent_float[O("fPowerBarXPercent")];
                float sliderX = Width * sliderXPercent;
                if (persistent_int[O("iPowerBarSide")] == 1) {
                    sliderX = Width * (1.0f - sliderXPercent);
                }
                float sliderYStart = Height * persistent_float[O("fPowerBarYStartPercent")];
                NativeTouchesEnd(powerSlider.TouchIndex, sliderX, sliderYStart);
                powerSlider.Active = false;
                powerSlider.state = PowerSlider::IDLE;
            }

            if (sharedGameManager) {
                double cur = sharedGameManager.mVisualCue().mVisualGuide().mAimAngle();
                sharedGameManager.mVisualCue().mVisualGuide().mAimAngle(cur);
            }

            
            humanShotLocked = false;
            anim_IsPulling = false;
            fastShotState = 0;
            humanState = HUM_IDLE;
            ClearState();
            state = IDLE;
            g_autoPlayCalculating = false;
        }
        g_autoPlayCalculating = false;
        return; 
    }

    // Reset aim status if cue ball is moved (e.g. ball-in-hand)
    if (currentMode == MODE_AUTO_AIM && bAimedThisTurn && sharedGameManager) {
        auto& cueBall = gPrediction->guiData.balls[0];
        double distSq = (cueBall.initialPosition - lastCuePosWhenAimed).square();
        if (distSq > 0.0025) {
            bAimedThisTurn = false;
            lastFailedCuePos = {-1000.0, -1000.0};
            state = IDLE;
        }
    }

    if (state == IDLE) {
        bool shouldScan = (currentMode != MODE_AUTO_AIM) || !bAimedThisTurn;
        if (shouldScan && sharedGameManager) {
            auto& cueBall = gPrediction->guiData.balls[0];
            double distToFailed = (cueBall.initialPosition - lastFailedCuePos).square();
            if (distToFailed <= 0.0025) {
                shouldScan = false;
            }
        }
        if (shouldScan) {
            state = SCANNING;
            scan = FAST;
            g_autoPlayCalculating = false;
        }
    }
    if (state == SCANNING) {
        if (scan == FAST) ScanFast();
        if (scan == SLOW) {
            g_autoPlayCalculating = true;
            float level = persistent_float.count(O("fScannerLevel")) ? persistent_float[O("fScannerLevel")] : 50.0f;
            // Map 0-100 to 0.005 - 0.040 radians (approx. 0.3 - 2.3 degrees) for maximum precision
            double step = 0.005 + (double(level) / 100.0) * 0.035;
            ScanSlow(step);
        }
    }

    if (state == NOMINATING) {
        setAimAngle(pendingShotAngle);
        nominationFrameCounter++;
        if (nominationFrameCounter == 10) {
            buttonClicker.Click(GetPocketScreenPos(g_CurrentCandidate.pocketIndex));
        }
        if (nominationFrameCounter > 20 && !buttonClicker.Active) {
            uint nominatedPocket = sharedGameManager.getNominatedPocket();
            if (nominatedPocket == g_CurrentCandidate.pocketIndex) {
                targetAngle = pendingShotAngle;
                g_PredictionLocked = true;

                // CRITICAL FIX: Re-validate pocketIndex by re-running simulation after nomination
                // This ensures g_CurrentCandidate.pocketIndex is fresh and not stale from
                // a previous scan frame, which is the root cause of wrong-angle-lock on black ball.
                {
                    
                    gPrediction->determineShotResult(true, pendingShotAngle, pendingShotPower,
                                                    sharedGameManager.getShotSpin(), g_CurrentCandidate);
                    
                    // Sync pocketIndex from fresh simulation result
                    if (g_CurrentCandidate.idx >= 0 && g_CurrentCandidate.idx < gPrediction->guiData.ballsCount) {
                        int freshPocket = gPrediction->guiData.balls[g_CurrentCandidate.idx].pocketIndex;
                        if (freshPocket >= 0 && freshPocket < 6) {
                            g_CurrentCandidate.pocketIndex = freshPocket;
                        }
                    }
                }

                if (currentMode == MODE_AUTO_AIM) {
                    applyAutoSpin();
                    bAimedThisTurn = true;
                    lastCuePosWhenAimed = gPrediction->guiData.balls[0].initialPosition;
                    g_postAimLock = true;
                    g_postAimAngle = pendingShotAngle;
                    g_postAimPower = pendingShotPower;
                    g_postAimFrames = 20;
                    ClearState();
                    state = IDLE;
                } else {
                    if (automationSpeed == SPEED_HUMAN && playStyle != STYLE_INSTANT) {
                        applyAutoSpin();
                        humanShotLocked = true;
                        humanState = HUM_THINKING;
                        stateStartTime = nowSec() + 0.3;
                        // CRITICAL FIX: Use pendingShotAngle as startAngle, NOT current visual cue angle.
                        // After nomination UI, game may have reset/changed the visual cue angle internally.
                        // Using pendingShotAngle ensures the human sweep animation starts from the correct
                        // reference angle and lands accurately on the target.
                        startAngle = pendingShotAngle;
                        state = EXECUTING;
                    } else {
                        // CRITICAL FIX: Manually set startAngle before takeShot so the joystick sweep
                        // in FAST mode always uses the correct reference angle post-nomination.
                        // takeShot() reads startAngle from visual cue which may be stale after nomination UI.
                        startAngle = pendingShotAngle;
                        takeShot(pendingShotAngle, pendingShotPower, true); // preserveStartAngle=true: don't overwrite with stale visual cue angle
                        state = EXECUTING;
                    }
                }
            } else {
                if (nominationFrameCounter > 40) {
                    nominationFrameCounter = 0;
                }
            }
        }
    }

    if (state == WAITING_FOR_USER_POCKET) {
        setAimAngle(pendingShotAngle);
        setPower(pendingShotPower);
        
        int currentNom = sharedGameManager.getNominatedPocket();
        if (currentNom == g_CurrentCandidate.pocketIndex && currentNom < 6) {
            takeShot(pendingShotAngle, pendingShotPower); 
            ClearState(); 
            state = IDLE;
        }
    }

    // --- REAL-TIME MANUAL TRACKING ---
    if (bShowAutoPlayLines && isPlayerTurn && state != EXECUTING && state != NOMINATING && state != WAITING_FOR_USER_POCKET && state != SCANNING && !g_autoPlayCalculating && g_CurrentCandidate.idx == -1) {
        double curAngle = sharedGameManager.mVisualCue().mVisualGuide().mAimAngle();
        double curPower = getCurrentPower();
        if (curPower < 100.0) curPower = 800.0;

        
        gPrediction->determineShotResult(true, curAngle, curPower, sharedGameManager.getShotSpin());
        
    }
}

bool AutoPlay::AreBallsMoving() {
    if (!sharedGameManager) return false;
    Table table = sharedGameManager.mTable;
    if (!table) return false;
    auto& balls = table.mBalls();
    if (!balls) return false;
    for (int i = 0; i < balls.Count; i++) {
        Ball ball = balls[i];
        if (ball && ball.isOnTable()) {
            auto vel = ball.velocity();
            if (vel.x * vel.x + vel.y * vel.y > 0.000001) {
                return true;
            }
            auto spin = ball.spin();
            if (spin.x * spin.x + spin.y * spin.y + spin.z * spin.z > 0.000001) {
                return true;
            }
        }
    }
    return false;
}

bool isTouchLockedByBot() {
    return (AutoPlay::g_PredictionLocked && AutoPlay::g_CurrentCandidate.idx != -1) || (AutoPlay::state == AutoPlay::NOMINATING);
}

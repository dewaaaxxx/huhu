#include "AutoPlay.h"
#include "game/GameManager.h"
#include "Prediction.h"
#include "game/inc/GameMaster.h"  // also pulls in HumanScan.h — needed for
                                   // GameMaster::ResetPlan() / HumanScan::ResetTurn()
                                   // / HumanScan::RunIfReady() calls below
#include "mod/ButtonClicker.h"
extern ButtonClicker buttonClicker;
#include "mod/PowerSlider.h"
extern PowerSlider powerSlider;
#include <math.h>
#include <random>

// --- Static Helpers ---
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

static constexpr double maxAngle = 2.0 * M_PI;

static double normalizeAngle(double angle) {
    constexpr double maxAngleVal = 2.0 * M_PI;
    double newAngle = angle;
    if (newAngle >= maxAngleVal) newAngle = fmod(newAngle, maxAngleVal);
    else if (newAngle < 0) newAngle = maxAngleVal - fmod(-newAngle, maxAngleVal);
    return newAngle;
}

static double CalculateRequiredPower(double cueDist, double distP, double cutDot = 1.0) {
    // AIMX Physics Sync with Cut Angle Energy Loss (v_object = v_cue * cutDot * e)
    // To ensure the target ball drops into the pocket cleanly without dying at the lip,
    // we add an extra distance margin (extraDist = 30.0) and scale cue velocity by 1 / (cutDot * e).
    double effectiveCut = std::max(fabs(cutDot) * 0.95, 0.15);
    double extraDist = 30.0;
    double v_obj_sq = 2.0 * 196.0 * (distP + extraDist);
    double v_impact_sq = v_obj_sq / (effectiveCut * effectiveCut);
    double v_cue_sq = v_impact_sq + 2.0 * 196.0 * cueDist;
    double p = sqrt(v_cue_sq);
    if (p < 0.0) p = 0.0;
    if (p > 666.0) p = 666.0;
    return p;
}

static double CalculateRequiredPower(double totalDist) {
    return CalculateRequiredPower(totalDist * 0.5, totalDist * 0.5, 1.0);
}

static Point2D GetEffectivePocketTarget(int pocketIdx, const Point2D& rawPocket, const Point2D& ballPos) {
    Point2D aperture = rawPocket;
    if (pocketIdx == 1) { // Bottom Side Pocket
        aperture.y = -63.8;
    } else if (pocketIdx == 4) { // Top Side Pocket
        aperture.y = 63.8;
    } else { // Corner Pockets (0, 2, 3, 5)
        double sx = (rawPocket.x > 0) ? 1.0 : -1.0;
        double sy = (rawPocket.y > 0) ? 1.0 : -1.0;
        aperture.x = rawPocket.x - sx * 3.5;
        aperture.y = rawPocket.y - sy * 3.2;
    }

    double dist = sqrt((rawPocket - ballPos).square());
    if (dist < 60.0) {
        double t = std::clamp(dist / 60.0, 0.0, 1.0);
        return aperture * (1.0 - t * 0.4) + rawPocket * (t * 0.4);
    }
    return rawPocket;
}

// ---------------------------------------------------------------------------
// Shape / reliability scoring (WILD "Beast" AI).
// The PRIMARY rule is untouched: pot the maximum number of our own balls.
// `leave` is ONLY a tie-break between candidates with the same pot count, so
// the aggressive potting style is preserved exactly.
// ---------------------------------------------------------------------------

// Penalty when an object ball rests too close to a cushion (unreliable pot).
static double RailProximityPenalty(const Point2D& pos) {
    double dx = std::min(TABLE_BOUND_RIGHT - pos.x, pos.x - TABLE_BOUND_LEFT);
    double dy = std::min(TABLE_BOUND_BOTTOM - pos.y, pos.y - TABLE_BOUND_TOP);
    double m = std::min(dx, dy);
    if (m >= 3.0 * BALL_RADIUS) return 0.0;
    return (3.0 * BALL_RADIUS - m) * 14.0;
}

// Lower = better. Measures how playable the cue ball rest position is for the
// NEXT shot after this candidate pots ball `pottedIdx`. Uses the post-shot
// predicted positions, so it also rewards shots that open up the table.
static double ComputeLeaveScore(int pottedIdx, Ball::Classification myclass, bool onlyEightBallLeft, int nominatedPocket) {
    const Prediction::SceneData& sim = gPrediction->guiData;
    if (!sim.balls[0].onTable) return 1e9; // scratch, already filtered out earlier
    const Point2D& cue = sim.balls[0].predictedPosition;
    bool isNineBall = (myclass == Ball::Classification::NINE_BALL_RULE);

    // Determine the next target ball(s).
    std::vector<int> nextTargets;
    if (isNineBall) {
        int lowest = -1;
        for (int i = 1; i < sim.ballsCount; i++) {
            if (!sim.balls[i].originalOnTable || !sim.balls[i].onTable) continue;
            if (lowest == -1 || i < lowest) lowest = i;
        }
        if (lowest != -1) nextTargets.push_back(lowest);
    } else {
        for (int i = 1; i < sim.ballsCount; i++) {
            const auto& b = sim.balls[i];
            if (!b.originalOnTable || !b.onTable) continue;
            if (i == pottedIdx) continue;
            bool mine = onlyEightBallLeft ? (i == 8)
                        : (myclass == Ball::Classification::ANY)
                            ? (b.classification != Ball::Classification::EIGHT_BALL &&
                               b.classification != Ball::Classification::CUE_BALL)
                            : (b.classification == myclass);
            if (mine) nextTargets.push_back(i);
        }
    }
    if (nextTargets.empty()) return 0.0; // nothing left to set up

    const auto& pockets = ::getPockets();
    double bestNext = 1e18;
    for (int ti : nextTargets) {
        const auto& b = sim.balls[ti];
        for (int pi = 0; pi < (int)pockets.size(); pi++) {
            if (nominatedPocket >= 0 && nominatedPocket < 6 && pi != nominatedPocket) continue;
            Point2D pocket = GetEffectivePocketTarget(pi, pockets[pi], b.predictedPosition);
            Point2D toPocket = pocket - b.predictedPosition;
            double distToPocket = sqrt(toPocket.square());
            if (distToPocket < 0.1) continue;
            Point2D dirP = toPocket * (1.0 / distToPocket);
            Point2D ghost = b.predictedPosition - dirP * (2.0 * BALL_RADIUS);
            Point2D toGhost = ghost - cue;
            double distToGhost = sqrt(toGhost.square());
            if (distToGhost < 0.1) continue;
            Point2D cueToBall = b.predictedPosition - cue;
            double dCueBall = sqrt(cueToBall.square());
            double cutDot = (cueToBall.x * dirP.x + cueToBall.y * dirP.y) / (dCueBall + 1e-9);
            double diff = distToGhost + distToPocket + (1.0 - cutDot) * 120.0;
            if (diff < bestNext) bestNext = diff;
        }
    }
    return bestNext;
}

// Reliability of the CURRENT shot that potted `pottedIdx` (lower = more reliable).
static double ComputeShotReliability(int pottedIdx, int pocketIdx) {
    const Prediction::SceneData& sim = gPrediction->guiData;
    if (pocketIdx < 0 || pocketIdx >= 6) return 0.0;
    const Point2D& cueInit = sim.balls[0].initialPosition;
    const Point2D& ballInit = sim.balls[pottedIdx].initialPosition;
    double reliab = (double)sim.collision.railCollisions * 6.0;
    reliab += RailProximityPenalty(ballInit);

    Point2D cueToBall = ballInit - cueInit;
    // ::getPockets() hands back a reference to a cached array; the AutoPlay
    // wrapper copies it into a fresh std::vector on every call. This runs once
    // per accepted candidate, so the wrapper was allocating twice per shot
    // evaluated - once here just to read a single element.
    Point2D ballToPocket = ::getPockets()[pocketIdx] - ballInit;
    double d1 = sqrt(cueToBall.square());
    double d2 = sqrt(ballToPocket.square());
    if (d1 > 1e-6 && d2 > 1e-6) {
        double cutDot = (cueToBall.x * ballToPocket.x + cueToBall.y * ballToPocket.y) / (d1 * d2);
        reliab += (1.0 - cutDot) * 120.0; // thin cut = unreliable
    }

    // Cue ball left near a pocket mouth is a scratch/easy-pot risk next turn.
    const Point2D& cue = sim.balls[0].predictedPosition;
    double nearPocketSq = 1e18;
    for (const auto& p : ::getPockets()) {
        double dsq = (cue - p).square();
        if (dsq < nearPocketSq) nearPocketSq = dsq;
    }
    if (nearPocketSq < (2.2 * POCKET_RADIUS) * (2.2 * POCKET_RADIUS)) reliab += 40.0;
    return reliab;
}

std::vector<double> AutoPlay::BuildPowerGrid(double idealPower) {
    const double minPower = std::max(0.0, (double)powerMin);
    const double maxPower = std::max(minPower, (double)powerMax);
    const double ideal = std::clamp(idealPower, minPower, maxPower);
    const double samples[] = {
        0.0,
        ideal * 0.35,
        ideal * 0.55,
        ideal * 0.70,
        ideal * 0.85,
        ideal,
        ideal * 1.10,
        ideal * 1.25,
        ideal * 1.45,
        300.0,
        420.0,
        540.0,
        666.0
    };

    std::vector<double> result;
    for (double sample : samples) {
        double value = std::clamp(sample, minPower, maxPower);
        bool duplicate = false;
        for (double existing : result) {
            if (fabs(existing - value) < 0.5) { duplicate = true; break; }
        }
        if (!duplicate) result.push_back(value);
    }
    std::sort(result.begin(), result.end());
    return result;
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
    if (forcedShotSpinActive) {
        auto ec = sharedGameManager.mVisualEnglishControl();
        if (ec) ec.mEnglish(forcedShotSpin);
        return;
    }
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
    auto ec = sharedGameManager.mVisualEnglishControl();
    if (ec) ec.mEnglish(spin);
}

std::vector<Point2D> AutoPlay::getPockets() {
    const auto& pts = ::getPockets();
    return std::vector<Point2D>(pts.begin(), pts.end());
}

static inline double g_lastSyncAngle = -999.0;
static inline double g_shotCooldownEnd = 0.0; // Prevents re-scan during shot animation
static int fastShotState = 0;                 // Fast shot sequence state (0: aiming, 1: stabilize, 2: delay before fire)

// Persistent wild scanning state (shared by Update() and ScanWild()).
static AutoPlay::WildScanState g_wild;

// Animation State for Smooth Power Pull
static double anim_CurrentPower = 0.0;
static double anim_TargetPower = 0.0;
static double anim_TargetAngle = 0.0;
static bool anim_IsPulling = false;
static long long anim_StartTime = 0;
static bool anim_RotationDone = false;     // Prevents Phase 2 running every frame
static bool anim_TouchStarted = false;     // Ensures joystick touch always starts reliably
static double g_lastFastShotTime = 0.0;   // Cooldown to prevent double-shot

// Captured ONCE when a STYLE_HUMAN shot is decided (in ExecuteHumanAI / the
// MODE_AUTO_AIM branch of Shoot()), instead of re-reading AutoPlay::currentMode
// live at the HUM_STABILIZING->HUM_PULLING fork several seconds later. Before
// this, that fork re-checked currentMode directly; currentMode is reasserted
// every frame from the menu (see menu.h DrawESP) AND separately written by the
// scratch-avoidance code in DrawToasts() (via the long-dead bAutoPlaySwitch /
// bAutoAimSwitch flags) and by the debug mode-picker UI - three independent
// writers racing across a ~2.5s human aiming sequence. If currentMode ever
// reads back as anything other than MODE_AUTO_PLAY on that one exact frame,
// the fork silently takes the "aim-only" branch: it releases the aim touch
// and stops, leaving the cue visibly aimed but never pulling the power bar -
// exactly the reported symptom ("aims by itself, but I have to pull the
// stick myself"). Deciding once at shot-start and reading that decision back
// removes the mid-sequence race entirely.
static bool humanShouldAutoFire = false;

static bool g_postShotLock = false;
static double g_postShotAngle = 0.0;
static double g_postShotPower = 0.0;
static int g_postShotFrames = 0;

static bool g_postAimLock = false;
static double g_postAimAngle = 0.0;
static double g_postAimPower = 0.0;
static int g_postAimFrames = 0;

void AutoPlay::ClearState() {
    const bool hadTouch = anim_TouchStarted;
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
    humanShouldAutoFire = false;
    forcedShotSpinActive = false;
    forcedShotSpin = Vec2d(0.0, 0.0);
    bShowAutoPlayLines = false;
    state = IDLE; // CRITICAL: Reset state machine
    fastShotState = 0;
    anim_IsPulling = false;
    anim_RotationDone = false;
    anim_TouchStarted = false;
    g_wild.isInitiated = false;

    if (!g_postShotLock) {
        setPower(0.0);
    }

    if (hadTouch) {
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

    // Cooldown: 2.0s mandatory wait after any shot to let animations finish
    g_shotCooldownEnd = AutoPlay::nowSec() + 2.0;
    HumanScan::ResetTurn();
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
    g_postShotAngle = (playStyle == STYLE_HUMAN) ? targetAngle : anim_TargetAngle;
    g_postShotPower = (playStyle == STYLE_HUMAN) ? pendingShotPower : anim_TargetPower;
    g_postShotFrames = 15;
    M(void, libmain + 0x2dc0c58, void*)(F(void*, sharedGameManager + 0x3b0));
}

bool AutoPlay::IsAnimationActive() {
    auto visualCue = sharedGameManager.mVisualCue();
    if (!visualCue) return false;
    auto _powerBarView = F(ptr, visualCue + 0x510);
    if (!_powerBarView) return false;
    return (M(ptr, libmain + 0x2de6f30, ptr)(_powerBarView) != 0);
}

void AutoPlay::ExecuteHumanAI(double angle, double power) {
    applyAutoSpin();
    startAngle = sharedGameManager.mVisualCue().mVisualGuide().mAimAngle();
    targetAngle = angle;
    pendingShotPower = power;
    
    g_PredictionLocked = true;
    bShowAutoPlayLines = false;

    gPrediction->forceFullSimulation = true;
    gPrediction->determineShotResult(true, angle, power, sharedGameManager.getShotSpin(), g_CurrentCandidate);
    gPrediction->forceFullSimulation = false;

    humanShotLocked = true;
    state = EXECUTING;
    humanState = HUM_THINKING;
    stateStartTime = nowSec() + 0.4;
    // Reached only via Shoot()'s "AUTO PLAY MODE" fallthrough (after the
    // MODE_AUTO_AIM branch above has already returned), i.e. this call IS
    // the full-autoplay delegation - decide to fire once, here, rather than
    // re-checking currentMode live mid-sequence.
    humanShouldAutoFire = true;
}

void AutoPlay::ExecuteBeastAI(double angle, double power) {
    applyAutoSpin();
    startAngle = sharedGameManager.mVisualCue().mVisualGuide().mAimAngle();
    targetAngle = angle;
    pendingShotPower = power;
    
    g_PredictionLocked = true;
    bShowAutoPlayLines = false;

    gPrediction->forceFullSimulation = true;
    gPrediction->determineShotResult(true, angle, power, sharedGameManager.getShotSpin(), g_CurrentCandidate);
    gPrediction->forceFullSimulation = false;

    takeShot(angle, power);
    state = EXECUTING;
}

// Defined further down, next to the rest of the table-state helpers, but needed
// here: the nomination gate below has to ask "are we shooting at the 8?" exactly
// the way the scanner asks it.
static bool ComputeOnlyEightBallLeft(Ball::Classification myclass);

void AutoPlay::Shoot(double angle, double power) {
    applyAutoSpin(); // AIMX SYNC: Apply spin BEFORE simulation so visuals match

    angle = NumberUtils::normalizeDoublePrecision(angle);
    // Snap the power to a value the cue can actually hold. setPower() stores
    // ShotPowerToPower(power) rounded to 4 decimals and the cue reads it back
    // through the inverse, and that round trip carries a sqrt - so near the top
    // of the bar, where this style lives, the fired speed drifted about half a
    // unit from the simulated one. Rounding `power` itself did nothing: it is in
    // the hundreds, so a 1e-4 snap is a no-op. The quantum is in the stored
    // value, so quantise through it and simulate the number that comes back.
    power = QuantizeShotPower(power);

    gPrediction->forceFullSimulation = true;
    gPrediction->determineShotResult(true, angle, power, sharedGameManager.getShotSpin());
    gPrediction->forceFullSimulation = false;

    bool nominating = false;
    int nominationMode = sharedGameManager.getPocketNominationMode();
    auto myclass = sharedGameManager.getPlayerClassification();
    
    // Use the EXACT angle and power that were simulated
    pendingShotPower = power;
    pendingShotAngle = angle;
    
    // "Are we shooting at the 8?" has to be answered by COUNTING what is left on
    // the table, not by reading the player's classification. The game does not
    // reliably flip that field to EIGHT_BALL when a group is cleared - it often
    // stays SOLID or STRIPE - which is the entire reason
    // ComputeOnlyEightBallLeft exists and why every other site in the scanner
    // calls it instead of testing the enum. This gate was the one place still
    // testing the enum directly, so on any table where the classification stayed
    // put, a call-pocket rule was silently skipped: the scanner correctly picked
    // a shot on the 8, `nominating` came out false, and the shot was fired with
    // no pocket called. Reported on a 4M table where the 8 needed both a called
    // pocket and a bank - the bank was played straight, uncalled. The enum test
    // is kept inside the helper (its first line), so tables where the game DOES
    // flip the classification behave exactly as before.
    const bool shootingAtTheEight = ComputeOnlyEightBallLeft(myclass);

    if ((nominationMode == 1 && shootingAtTheEight) || (nominationMode == 2 && myclass != Ball::Classification::ANY)) {
        if (g_CurrentCandidate.idx != -1 && g_CurrentCandidate.pocketIndex >= 0 &&
            sharedGameManager.getNominatedPocket() != g_CurrentCandidate.pocketIndex) {
            nominating = true;
        }
        // The rule says call a pocket and we are about to fire without calling
        // one. Either the shot carries no pocket to call (a blind sweep ray that
        // was never solved for one) or the pocket is already nominated. The
        // first is a lost frame; say so in the log rather than let it pass
        // silently, because on the 8 it is the difference between winning and
        // handing over the game.
        else if (g_CurrentCandidate.pocketIndex < 0) {
            LOGI("[WildDbg] NOMINATE SKIPPED - no pocket on candidate | mode=%d atEight=%d myclass=%d idx=%d nom=%u",
                 nominationMode, (int)shootingAtTheEight, (int)myclass,
                 g_CurrentCandidate.idx, sharedGameManager.getNominatedPocket());
        }
    }

    if (nominating) {
        LOGI("[WildDbg] NOMINATE pocket=%d | mode=%d atEight=%d myclass=%d idx=%d mustBank=%d",
             g_CurrentCandidate.pocketIndex, nominationMode, (int)shootingAtTheEight,
             (int)myclass, g_CurrentCandidate.idx,
             (int)sharedGameManager.is8BallCushionShotRequired());
        pendingShotPower = power;
        pendingShotAngle = angle;
        state = NOMINATING;
        nominationFrameCounter = 0;
        
        // Record if we need to return to human aiming after nomination
        humanNeedsNomination = (playStyle == STYLE_HUMAN);
        return; 
    }

    // --- AUTO AIM MODE ---
    if (currentMode == MODE_AUTO_AIM) {
        applyAutoSpin();
        if (playStyle == STYLE_WILD) {
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
        } else if (playStyle == STYLE_HUMAN) {
            humanShotLocked = true;
            state = EXECUTING;
            humanState = HUM_THINKING;
            stateStartTime = nowSec() + 0.4;
            startAngle = sharedGameManager.mVisualCue().mVisualGuide().mAimAngle();
            targetAngle = angle;
            pendingShotPower = power;
            // MODE_AUTO_AIM by definition only aims and holds - the player
            // pulls and releases manually. Pin that decision now so it can't
            // drift later even if currentMode changes mid-sequence.
            humanShouldAutoFire = false;
        }
        return;
    }

    // --- AUTO PLAY MODE (DELEGATED PIPELINES) ---
    if (playStyle == STYLE_WILD) {
        ExecuteBeastAI(angle, power);
    } else {
        ExecuteHumanAI(angle, power);
    }
    return;
}

// =============================================================================
// WILD SCANNER
//
// Replaces the old Beast pair ScanFast/ScanSlow plus TryFindSafetyShot (~930
// lines, deleted here). Two things were wrong with them:
//
//   * ScanFast could not return a pot AT ALL. Its ten-level power loop never
//     broke on the wild path - only STYLE_HUMAN did - so by the time the
//     acceptance gates read gPrediction->guiData it held the LAST iteration,
//     which was always testPower = powerMin = 0.0. A zero-power shot pots
//     nothing, so every candidate hit `continue` and fs.evals stayed empty.
//     Control therefore always fell through to `scan = SLOW`.
//   * ScanSlow, the only path that actually ran, swept the cue ball's full 360
//     degrees blind at ONE ray per frame (maxSteps = 1): 1257 rays at scanner
//     level 0, ~4.7 s at level 50. The 10 s state timeout would fire mid-sweep
//     and call ClearState(), which armed a 2.0 s cooldown and sent the state
//     machine back to SCANNING. That loop is the freeze.
//
// The replacement rests on two changes:
//   1. Candidates come from ghost-ball geometry solved per (ball, pocket)
//      pair, so a full table is a few dozen real candidates instead of a
//      thousand blind rays.
//   2. Per-frame work is bounded by WALL-CLOCK time, not by a fixed candidate
//      count. A fast phone burns many candidates per frame and a slow one
//      burns few; neither drops a frame. A count budget cannot do this, since
//      the cost of one candidate varies by more than an order of magnitude
//      with how long the balls keep rolling.
//
// The old per-frame radar sweep is gone with them: it spent one extra FULL
// simulation every frame purely to animate a rotating line, which is the
// single most expensive piece of decoration in the mod.
//
// This is the scan lifecycle only. Candidate generation (phase 3), the
// budgeted evaluation loop (phase 4) and selection (phase 5) fill the marked
// holes.
// "Every ball of our group is gone, so the 8 is now the legal target." Both the
// candidate generator and the acceptance gates branch on this, and they must
// agree exactly - if one of them thinks the 8 is still illegal while the other
// aims at it, the bot fouls the game away. One definition, two callers.
static bool ComputeOnlyEightBallLeft(Ball::Classification myclass) {
    const Prediction::SceneData& sim = gPrediction->guiData;
    if (myclass == Ball::Classification::EIGHT_BALL) return true;
    if (myclass == Ball::Classification::SOLID) {
        for (int k = 1; k < 8; k++)
            if (sim.balls[k].originalOnTable) return false;
        return true;
    }
    if (myclass == Ball::Classification::STRIPE) {
        for (int k = 9; k <= 15; k++)
            if (sim.balls[k].originalOnTable) return false;
        return true;
    }
    if (myclass == Ball::Classification::ANY) {
        for (int k = 1; k <= 15; k++) {
            if (k == 8) continue;
            if (sim.balls[k].originalOnTable) return false;
        }
        return true;
    }
    return false;
}

// How many balls we are actually allowed to pot right now. The refinement pass
// keeps hunting for a bigger scatter until it reaches its target, and without
// this that target would be a flat 5 - so with two balls left on the table it
// would grind through every refinement round it is allowed, every single turn,
// chasing three balls that do not exist. The ceiling is what the table can give.
static int CountLegalTargets(Ball::Classification myclass, bool isNineBallGame,
                             bool onlyEightBallLeft) {
    const Prediction::SceneData& sim = gPrediction->guiData;
    if (onlyEightBallLeft) return 1;
    int n = 0;
    for (int i = 1; i < sim.ballsCount; i++) {
        const auto& b = sim.balls[i];
        if (!b.originalOnTable) continue;
        if (isNineBallGame) { n++; continue; }
        if (b.classification == Ball::Classification::EIGHT_BALL) continue;
        if (myclass == Ball::Classification::ANY || b.classification == myclass) n++;
    }
    return n;
}

// --- Geometric pre-filters -------------------------------------------------
// The old generator emitted every ball x pocket pair unconditionally and paid a
// full 10-power simulation sweep to discover that a path was blocked or the cut
// was impossible. Each of these tests is a few dozen flops and kills the
// candidate before it costs a single sim, which is where most of the speed-up
// comes from: on a crowded table they reject roughly two thirds of the pairs.

// A ball rolling from `from` to `to` passes cleanly only if every other ball
// centre clears the centreline by more than 2R. Returns the number of blockers
// and, via `allOurs`, whether every one of them is a ball we may legally pot -
// that distinction is what separates a real combination from a dead end.
static int PathBlockers(const Point2D& from, const Point2D& to,
                        int ignoreA, int ignoreB,
                        double clearance, bool* allOurs,
                        Ball::Classification myclass, bool onlyEightBallLeft,
                        bool isNineBallGame) {
    const Prediction::SceneData& sim = gPrediction->guiData;
    const double clearSq = clearance * clearance;
    int blockers = 0;
    bool ours = true;
    for (int i = 1; i < sim.ballsCount; i++) {
        if (i == ignoreA || i == ignoreB) continue;
        const auto& b = sim.balls[i];
        if (!b.originalOnTable) continue;
        if (DistToSegmentSq(b.initialPosition, from, to) >= clearSq) continue;
        blockers++;
        bool mine = isNineBallGame ? true
                    : onlyEightBallLeft ? (i == 8)
                    : (myclass == Ball::Classification::ANY)
                        ? (b.classification != Ball::Classification::EIGHT_BALL)
                        : (b.classification == myclass);
        if (!mine) ours = false;
    }
    if (allOurs) *allOurs = ours;
    return blockers;
}

// The cue ball centre can never leave the cushion box, so a ghost position
// outside it is unreachable and the shot does not exist. The margin keeps
// legitimate pocket-mouth shots alive, where the real table shape opens past
// the nominal rail line.
static bool GhostReachable(const Point2D& ghost) {
    constexpr double m = BALL_RADIUS;
    return ghost.x > TABLE_BOUND_LEFT  - m && ghost.x < TABLE_BOUND_RIGHT  + m &&
           ghost.y > TABLE_BOUND_TOP   - m && ghost.y < TABLE_BOUND_BOTTOM + m;
}

// One-cushion bank: aim at the mirror image of the ghost across a rail, so the
// cue ball reflects off that rail and arrives at the ghost from the far side.
// Two things need this. The 8-ball-must-bank rule makes it the ONLY legal way
// to finish those racks, and on a crowded table it reaches ghosts whose direct
// lane is blocked - which is most of the positions where the scanner used to
// come up empty and hand over the turn.
//
// `railAxis` 0 = vertical rail at `railPos` (mirror x), 1 = horizontal (mirror y).
// Returns false when the reflection point falls off the end of that rail, when
// either leg is blocked, or when cue and ghost are not both on the live side.
static bool SolveBank(const Point2D& cuePos, const Point2D& ghost,
                      int railAxis, double railPos, int ignoreA,
                      Ball::Classification myclass, bool onlyEightBallLeft,
                      bool isNineBallGame,
                      Point2D* outAimPoint, double* outPathLen) {
    Point2D mirror = ghost;
    double cueSide, ghostSide;
    if (railAxis == 0) { mirror.x = 2.0 * railPos - ghost.x; cueSide = cuePos.x; ghostSide = ghost.x; }
    else               { mirror.y = 2.0 * railPos - ghost.y; cueSide = cuePos.y; ghostSide = ghost.y; }

    // Both must sit on the same side of the rail, otherwise the "reflection"
    // is behind the cushion and the shot is fiction.
    double dC = cueSide - railPos, dG = ghostSide - railPos;
    if (dC * dG <= 0.0) return false;
    if (fabs(dC) < 0.5 || fabs(dG) < 0.5) return false;

    Point2D seg = mirror - cuePos;
    double denom = (railAxis == 0) ? seg.x : seg.y;
    if (fabs(denom) < 1e-6) return false;
    double t = (railPos - ((railAxis == 0) ? cuePos.x : cuePos.y)) / denom;
    if (t <= 0.02 || t >= 0.98) return false;

    Point2D hit = cuePos + seg * t;
    // The reflection must land on the rail's playable span, not past a pocket
    // jaw where the cushion simply is not there.
    constexpr double endMargin = 3.0 * BALL_RADIUS;
    if (railAxis == 0) {
        if (hit.y < TABLE_BOUND_TOP + endMargin || hit.y > TABLE_BOUND_BOTTOM - endMargin) return false;
    } else {
        if (hit.x < TABLE_BOUND_LEFT + endMargin || hit.x > TABLE_BOUND_RIGHT - endMargin) return false;
        // A ball crossing the middle of a long rail passes the side pocket
        // mouth, where there is no cushion to bounce off at all.
        if (fabs(hit.x) < 2.5 * BALL_RADIUS) return false;
    }

    if (PathBlockers(cuePos, hit, ignoreA, -1, 1.85 * BALL_RADIUS, nullptr,
                     myclass, onlyEightBallLeft, isNineBallGame) != 0) return false;
    if (PathBlockers(hit, ghost, ignoreA, -1, 1.85 * BALL_RADIUS, nullptr,
                     myclass, onlyEightBallLeft, isNineBallGame) != 0) return false;

    *outAimPoint = hit;
    *outPathLen  = sqrt((hit - cuePos).square()) + sqrt((ghost - hit).square());
    return true;
}


// Two candidates whose angle and power agree to finer than the cue can express
// are the same shot, and the second one costs a full simulation to learn
// nothing. The targeted fans produce a lot of these - two of our balls roughly
// in line with the cue share most of their contact window. Keeping the FIRST
// occurrence keeps the better-ranked derivation, because the list is assembled
// in priority order before this runs.
//
// The angle quantum is a parameter because the refinement grids are not all the
// same scale. A razor-fine grid stepping 0.002 rad would be almost entirely
// erased by the 0.006 default - every sample inside a bucket after the first is
// discarded - so a grid built to find a tenth-of-a-degree cut has to dedup at
// its own resolution or it never gets simulated at all.
static void DedupeWildRaw(double angleQuantum = 0.006) {
    std::vector<AutoPlay::WildRaw> uniq;
    std::vector<uint64_t> keys;
    uniq.reserve(g_wild.raw.size());
    keys.reserve(g_wild.raw.size());
    for (const auto& r : g_wild.raw) {
        uint64_t ka = (uint64_t)(int64_t)llround(r.c.angle / angleQuantum);
        uint64_t kp = (uint64_t)(int64_t)llround(r.c.power / 10.0);
        uint64_t key = (ka << 20) | (kp & 0xFFFFF);
        if (std::find(keys.begin(), keys.end(), key) != keys.end()) continue;
        keys.push_back(key);
        uniq.push_back(r);
    }
    g_wild.raw.swap(uniq);
}

// Fills g_wild.raw with every shot worth simulating from the current cue position.
// Runs ONCE per scan, costs no simulations at all, and is the reason the scan
// is fast: it replaces the old blind 360-degree ray sweep (1257 rays at scanner
// level 0, one per frame) with solved ghost-ball geometry, then throws away
// everything the table geometry already proves impossible.
static void BuildWildCandidates() {
    using WildRaw = AutoPlay::WildRaw;
    Prediction::SceneData& sim = gPrediction->guiData;
    const auto& cueBall = sim.balls[0];
    const Point2D cuePos = cueBall.initialPosition;

    Ball::Classification myclass = sharedGameManager.getPlayerClassification();
    uint nominatedPocket = sharedGameManager.getNominatedPocket();
    bool isNineBallGame = (myclass == Ball::Classification::NINE_BALL_RULE);
    const auto& pockets = ::getPockets();

    // "Only the 8 left" decides both what counts as a legal target and what
    // counts as a legal blocker, so it has to be resolved before anything else.
    bool onlyEightBallLeft = ComputeOnlyEightBallLeft(myclass);

    std::vector<WildRaw> direct;
    std::vector<WildRaw> special;   // combos + kisses, ranked and capped
    std::vector<WildRaw> banks;     // one-cushion solves, ranked and capped
    std::vector<WildRaw> scatter;   // targeted contact-window fans: the multi-ball engine
    std::vector<WildRaw> sweep;     // blind fallback, lowest family

    // THE BREAK. Every ball is racked, so no ghost-ball solve means anything:
    // the pot count is decided by where the cluster is struck and how hard.
    // This is also the single best multi-ball opportunity in the game, so it
    // gets a dense ray fan at full power rather than the usual sparse sweep.
    //
    // The old test was `x < 70.0 || x > 120.0` counted over the balls, but the
    // table spans -127..127, so that window covers about 78% of its width and
    // the test passed for most ordinary positions - the "break optimizer" was
    // running mid-game, replacing real geometry with 79 blind rays that were
    // additionally all tagged pocket 0. Measuring the cluster instead says what
    // was actually meant: a full rack still packed into a rack-sized box.
    bool isBreakPosition = false;
    if (sim.ballsCount >= 15) {
        int n = 0;
        double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
        for (int k = 1; k < sim.ballsCount; k++) {
            const auto& b = sim.balls[k];
            if (!b.originalOnTable) continue;
            n++;
            minX = std::min(minX, b.initialPosition.x);
            maxX = std::max(maxX, b.initialPosition.x);
            minY = std::min(minY, b.initialPosition.y);
            maxY = std::max(maxY, b.initialPosition.y);
        }
        // A racked triangle measures about 26 x 31 units; 12R = 45.6 leaves
        // margin for the 9-ball diamond without admitting a broken spread.
        if (n >= 15 && (maxX - minX) < 12.0 * BALL_RADIUS
                    && (maxY - minY) < 12.0 * BALL_RADIUS) {
            isBreakPosition = true;
        }
    }

    if (isBreakPosition) {
        for (double a = 0.0; a < 2.0 * M_PI; a += 0.08) {
            Candidate c = {1, a, 10.0, -1, (double)AutoPlay::powerMax, 10.0};
            sweep.push_back({c, AutoPlay::FAM_SWEEP});
        }
    } else {
        // Two fans, and neither is the old uniform 360-degree spray.
        //
        // The first is TARGETED. The set of cue directions that can touch a ball
        // at all is exactly asin(2R/d) either side of the straight line to it,
        // so a fan across THAT window and nothing outside it makes contact on
        // every single ray, at every cut angle from full-thin-left through
        // full-thin-right. The uniform fan spent most of its rays on empty felt
        // and still sampled a distant ball more coarsely than this does: at
        // d = 100 the whole window is 0.152 rad wide, which the old 0.105 step
        // covered with one or two rays, and this covers with nine.
        //
        // The second is a COARSE net. It exists only for the snookered case,
        // where every direct line to our own balls is blocked and the only legal
        // contact left comes off a cushion - which no ghost-ball solve produces
        // and no targeted ray can reach.
        //
        // Both the ray count and the power count were raised once it was clear
        // the style was potting one ball at a time. How many balls a scatter
        // drops is decided by two things and the old fan only varied one of
        // them: WHERE on the object ball the cue lands (the angle) and HOW FAR
        // the cue keeps travelling afterwards (the power). Every ray used to
        // fire at maximum, so the entire family of shots where the cue ball is
        // meant to die inside a cluster rather than blast through it was
        // unrepresented. Two powers per ray covers both behaviours.
        const double fanPowers[2] = { (double)AutoPlay::powerMax,
                                      (double)AutoPlay::powerMin
                                      + ((double)AutoPlay::powerMax
                                         - (double)AutoPlay::powerMin) * 0.74 };
        int fanBalls = 0;
        for (int i = 1; i < sim.ballsCount && fanBalls < 10; i++) {
            const auto& b = sim.balls[i];
            if (!b.originalOnTable) continue;
            if (!isNineBallGame) {
                bool isTarget = onlyEightBallLeft ? (i == 8)
                                : (myclass == Ball::Classification::ANY)
                                    ? (b.classification != Ball::Classification::EIGHT_BALL)
                                    : (b.classification == myclass);
                if (!isTarget) continue;
            }
            Point2D d = b.initialPosition - cuePos;
            double dist = sqrt(d.square());
            if (dist < 2.0 * BALL_RADIUS) continue;
            fanBalls++;

            double centre = atan2(d.y, d.x);
            double half   = asin(std::min(1.0, (2.0 * BALL_RADIUS) / dist));
            for (int k = 0; k < 19; k++) {
                // 0.97 keeps the two end rays just inside the tangent instead of
                // exactly on it, where the contact grazes and may not resolve.
                // Those end rays are the thinnest legal hits on either side, and
                // they are what sends the cue ball the length of the table into
                // a second cluster - which is where multi-ball scatters come from.
                double a = centre + half * (-1.0 + 2.0 * (double)k / 18.0) * 0.97;
                if (a < 0) a += 2 * M_PI;
                if (a >= 2 * M_PI) a -= 2 * M_PI;
                for (int p = 0; p < 2; p++) {
                    Candidate c = {i, a, 30.0, -1, fanPowers[p], 30.0};
                    scatter.push_back({c, AutoPlay::FAM_SWEEP, true});
                }
            }
        }
        for (double a = 0.0; a < 2.0 * M_PI; a += 0.26) {
            Candidate c = {1, a, 40.0, -1, (double)AutoPlay::powerMax, 40.0};
            sweep.push_back({c, AutoPlay::FAM_SWEEP});
        }
    }

    // The 8 has to reach a cushion before it drops on these tables, so a clean
    // straight pot on the 8 is a foul no matter how good the geometry is.
    // Knowing this up front lets the generator spend its budget on banks
    // instead of on direct solves that will be rejected after the fact.
    const bool eightMustBank = onlyEightBallLeft && sharedGameManager.is8BallCushionShotRequired();

    for (int i = 1; i < sim.ballsCount; i++) {
        const auto& ball = sim.balls[i];
        if (!ball.originalOnTable) continue;

        if (!isNineBallGame) {
            bool isTarget = onlyEightBallLeft ? (i == 8)
                            : (myclass == Ball::Classification::ANY)
                                ? (ball.classification != Ball::Classification::EIGHT_BALL)
                                : (ball.classification == myclass);
            if (!isTarget) continue;
        }

        for (int pi = 0; pi < (int)pockets.size(); pi++) {
            if (nominatedPocket < 6 && pi != (int)nominatedPocket) continue;

            Point2D rawPocket = pockets[pi];
            Point2D pocket = GetEffectivePocketTarget(pi, rawPocket, ball.initialPosition);
            Point2D toPocket = pocket - ball.initialPosition;
            double distToPocket = sqrt(toPocket.square());
            if (distToPocket < 0.1) continue;
            Point2D dirP = toPocket * (1.0 / distToPocket);
            Point2D ghost = ball.initialPosition - dirP * (2.0 * BALL_RADIUS);

            // ---- DIRECT POT ----
            bool directViable = false;
            if (GhostReachable(ghost)) {
                Point2D shotLine = ghost - cuePos;
                double distCueToGhost = sqrt(shotLine.square());
                if (distCueToGhost >= 0.001) {
                    Point2D cueToBall = ball.initialPosition - cuePos;
                    double dCueBall = sqrt(cueToBall.square());
                    if (dCueBall >= 0.001) {
                        Point2D dirCue = cueToBall * (1.0 / dCueBall);
                        double dotCut = dirCue.x * dirP.x + dirCue.y * dirP.y;

                        // A cut past ~86 degrees transfers almost nothing along
                        // the pocket line. This was 0.12 (~83 degrees), which
                        // also discarded the thin cuts that are often the only
                        // shot on the table - and a discarded thin cut is how
                        // the scanner ended a turn with nothing to fire.
                        if (dotCut > 0.06) {
                            bool objOurs = true, cueOurs = true;
                            int cueBlock = PathBlockers(cuePos, ghost, i, -1, 1.85 * BALL_RADIUS,
                                                        &cueOurs, myclass, onlyEightBallLeft, isNineBallGame);
                            int objBlock = PathBlockers(ball.initialPosition, pocket, i, 0, 1.95 * BALL_RADIUS,
                                                        &objOurs, myclass, onlyEightBallLeft, isNineBallGame);

                            // A clear lane both ways is a true direct pot. If
                            // either lane is fouled only by balls we may legally
                            // pot, the shot is really a combination - keep it,
                            // but rank it as one so it cannot outrank a clean pot
                            // at equal count. A cue lane blocked by one of OUR
                            // balls used to kill the candidate outright; the sim
                            // is perfectly capable of telling us whether that
                            // ball redirects into something useful, and cheap
                            // multi-ball pots hide in exactly that case.
                            bool cueClear = (cueBlock == 0);
                            int fam = -1;
                            if (cueClear && objBlock == 0)          fam = AutoPlay::FAM_DIRECT;
                            else if (cueClear && objOurs)           fam = AutoPlay::FAM_COMBO;
                            else if (cueOurs && cueBlock <= 1 && objBlock == 0) fam = AutoPlay::FAM_COMBO;

                            // Potting the 8 without a cushion is a loss, so on
                            // those tables the direct solve is not a candidate
                            // at all - only the bank below is.
                            if (eightMustBank && i == 8) fam = -1;

                            if (fam >= 0) {
                                double angle = atan2(shotLine.y, shotLine.x);
                                if (angle < 0) angle += 2 * M_PI;
                                double score = distToPocket * 1.5 + distCueToGhost + (1.0 - dotCut) * 120.0;
                                if (pi == 1 || pi == 4) {
                                    // Side pockets have no jaw to funnel a shallow
                                    // approach; a rimming angle rattles out.
                                    if (fabs(dirP.y) < 0.4) score += 80.0;
                                }
                                double power = CalculateRequiredPower(distCueToGhost, distToPocket, dotCut);
                                Candidate c = {i, angle, score, pi, power, score};
                                direct.push_back({c, fam});
                                if (fam == AutoPlay::FAM_DIRECT) directViable = true;
                            }
                        }
                    }
                }
            }

            // ---- ONE-CUSHION BANK ----
            // Generated only where it earns its cost: when the 8 must bank (the
            // bank is then the sole legal shot) or when no clean direct pot to
            // this pocket exists. That keeps the candidate list from doubling on
            // open tables while still producing shots on the crowded ones where
            // the scanner used to find nothing.
            if ((eightMustBank && i == 8) || !directViable) {
                if (GhostReachable(ghost)) {
                    const double rails[4][2] = {
                        {0.0, TABLE_BOUND_LEFT},  {0.0, TABLE_BOUND_RIGHT},
                        {1.0, TABLE_BOUND_TOP},   {1.0, TABLE_BOUND_BOTTOM}
                    };
                    for (int r = 0; r < 4; r++) {
                        Point2D aimPoint; double pathLen = 0.0;
                        if (!SolveBank(cuePos, ghost, (int)rails[r][0], rails[r][1], i,
                                       myclass, onlyEightBallLeft, isNineBallGame,
                                       &aimPoint, &pathLen)) continue;

                        Point2D shotLine = aimPoint - cuePos;
                        double angle = atan2(shotLine.y, shotLine.x);
                        if (angle < 0) angle += 2 * M_PI;
                        // Rank behind every direct family, and behind shorter
                        // banks. A cushion eats speed, so the power carries a
                        // surcharge the flat distance formula does not know about.
                        double score = pathLen + distToPocket * 1.5 + 200.0;
                        double power = std::clamp(
                            CalculateRequiredPower(pathLen + distToPocket) * 1.45,
                            (double)AutoPlay::powerMin, (double)AutoPlay::powerMax);
                        Candidate c = {i, angle, score, pi, power, score};
                        banks.push_back({c, AutoPlay::FAM_BANK});
                    }
                }
            }

            // ---- COMBINATION: cue -> A -> B -> pocket ----
            // Only worth generating when B can actually reach this pocket, so
            // the pocket loop already prunes most of the pairs.
            for (int j = 1; j < sim.ballsCount; j++) {
                if (j == i) continue;
                const auto& ballB = sim.balls[j];
                if (!ballB.originalOnTable) continue;
                bool bValid = isNineBallGame ? true
                              : onlyEightBallLeft ? false
                              : (myclass == Ball::Classification::ANY)
                                  ? (ballB.classification != Ball::Classification::EIGHT_BALL)
                                  : (ballB.classification == myclass);
                if (!bValid) continue;

                Point2D toPocketB = pocket - ballB.initialPosition;
                double distBToPocket = sqrt(toPocketB.square());
                if (distBToPocket < 0.1) continue;
                Point2D dirB = toPocketB * (1.0 / distBToPocket);
                Point2D ghostB = ballB.initialPosition - dirB * (2.0 * BALL_RADIUS);

                Point2D toGhostB = ghostB - ball.initialPosition;
                double distAToGhostB = sqrt(toGhostB.square());
                if (distAToGhostB < 0.1) continue;
                Point2D dirA = toGhostB * (1.0 / distAToGhostB);
                Point2D ghostA = ball.initialPosition - dirA * (2.0 * BALL_RADIUS);
                if (!GhostReachable(ghostA)) continue;

                Point2D shotLine = ghostA - cuePos;
                double distCueToA = sqrt(shotLine.square());
                if (distCueToA < 0.001) continue;

                if (PathBlockers(cuePos, ghostA, i, -1, 1.85 * BALL_RADIUS,
                                 nullptr, myclass, onlyEightBallLeft, isNineBallGame) != 0) continue;
                if (PathBlockers(ball.initialPosition, ghostB, i, j, 1.9 * BALL_RADIUS,
                                 nullptr, myclass, onlyEightBallLeft, isNineBallGame) != 0) continue;

                double angle = atan2(shotLine.y, shotLine.x);
                if (angle < 0) angle += 2 * M_PI;
                double score = distCueToA + distAToGhostB + distBToPocket + 80.0;
                double power = std::clamp(CalculateRequiredPower(distCueToA + distAToGhostB + distBToPocket) * 1.1,
                                          (double)AutoPlay::powerMin, (double)AutoPlay::powerMax);
                Candidate c = {i, angle, score, pi, power, score};
                special.push_back({c, AutoPlay::FAM_COMBO});
            }

            // ---- KISS / CAROM: cue -> A, A deflects B into the pocket ----
            for (int j = 1; j < sim.ballsCount; j++) {
                if (j == i) continue;
                const auto& ballB = sim.balls[j];
                if (!ballB.originalOnTable) continue;
                bool bValid = isNineBallGame ? true
                              : onlyEightBallLeft ? false
                              : (myclass == Ball::Classification::ANY)
                                  ? (ballB.classification != Ball::Classification::EIGHT_BALL)
                                  : (ballB.classification == myclass);
                if (!bValid) continue;

                Point2D toPocketB = pocket - ballB.initialPosition;
                double distBToPocket = sqrt(toPocketB.square());
                if (distBToPocket < 0.1) continue;
                Point2D dirB = toPocketB * (1.0 / distBToPocket);
                Point2D ghostB = ballB.initialPosition - dirB * (2.0 * BALL_RADIUS);

                Point2D d = ghostB - ball.initialPosition;
                double distD = sqrt(d.square());
                if (distD < 2.0 * BALL_RADIUS) continue;

                double ratio = std::min((2.0 * BALL_RADIUS) / distD, 1.0);
                double theta = acos(ratio);
                double angleD = atan2(d.y, d.x);

                for (int sign : {-1, 1}) {
                    double angleU = angleD + sign * theta;
                    Point2D u = {cos(angleU), sin(angleU)};
                    Point2D ghostA = ball.initialPosition + u * (2.0 * BALL_RADIUS);
                    if (!GhostReachable(ghostA)) continue;

                    Point2D shotLine = ghostA - cuePos;
                    double distCueToA = sqrt(shotLine.square());
                    if (distCueToA < 0.001) continue;

                    if (PathBlockers(cuePos, ghostA, i, -1, 1.85 * BALL_RADIUS,
                                     nullptr, myclass, onlyEightBallLeft, isNineBallGame) != 0) continue;

                    double angle = atan2(shotLine.y, shotLine.x);
                    if (angle < 0) angle += 2 * M_PI;
                    double score = distCueToA + distD + distBToPocket + 120.0;
                    double power = std::clamp(CalculateRequiredPower(distCueToA + distD + distBToPocket) * 1.2,
                                              (double)AutoPlay::powerMin, (double)AutoPlay::powerMax);
                    Candidate c = {i, angle, score, pi, power, score};
                    special.push_back({c, AutoPlay::FAM_KISS});
                }
            }
        }
    }

    // Assemble. Direct pots first: they are the trustworthy shots and the
    // guaranteed one-ball floor, so they have to be in hand before anything can
    // go wrong. The SCATTER fan comes straight after them, and that placement is
    // the whole reason this style pots more than one ball at a time. It used to
    // sit at the very end, behind ~108 combos, kisses and banks costing 2-4
    // simulations each - about 300 simulations, which is more than the entire
    // time budget for a scan that already holds a one-ball pot. The three-ball
    // shot was in the list and the search stopped just short of ever simulating
    // it. Specials and banks are still worth having, but they are worth having
    // AFTER the thing we actually came for.
    //
    // The one exception is a table where the 8 must bank - there the bank IS the
    // shot, so it goes to the front or it never gets simulated at all.
    g_wild.raw.clear();
    g_wild.raw.reserve(direct.size() + 40 + banks.size() + scatter.size() + sweep.size());

    std::sort(direct.begin(), direct.end(), [](const WildRaw& a, const WildRaw& b) {
        if (a.family != b.family) return a.family > b.family;
        return a.c.score < b.c.score;
    });
    // Beyond the best 30, extra direct solves are near-duplicates of ones
    // already in the list and each one delays the scatter fan by 2-4 sims.
    if (direct.size() > 30) direct.resize(30);

    if (!special.empty()) {
        std::sort(special.begin(), special.end(), [](const WildRaw& a, const WildRaw& b) {
            return a.c.score < b.c.score;
        });
        if (special.size() > 40) special.resize(40);
    }
    if (!banks.empty()) {
        std::sort(banks.begin(), banks.end(), [](const WildRaw& a, const WildRaw& b) {
            return a.c.score < b.c.score;
        });
        if (banks.size() > 28) banks.resize(28);
    }

    if (eightMustBank) {
        g_wild.raw.insert(g_wild.raw.end(), banks.begin(), banks.end());
        g_wild.raw.insert(g_wild.raw.end(), direct.begin(), direct.end());
        g_wild.raw.insert(g_wild.raw.end(), scatter.begin(), scatter.end());
        g_wild.raw.insert(g_wild.raw.end(), special.begin(), special.end());
    } else {
        g_wild.raw.insert(g_wild.raw.end(), direct.begin(), direct.end());
        g_wild.raw.insert(g_wild.raw.end(), scatter.begin(), scatter.end());
        g_wild.raw.insert(g_wild.raw.end(), special.begin(), special.end());
        g_wild.raw.insert(g_wild.raw.end(), banks.begin(), banks.end());
    }

    g_wild.raw.insert(g_wild.raw.end(), sweep.begin(), sweep.end());

    DedupeWildRaw();

    // Where the scatter fan ends AFTER dedup, since dedup shifts every index.
    // A one-ball pot is not committed to until the scan has passed this mark.
    g_wild.scatterEnd = 0;
    for (size_t k = 0; k < g_wild.raw.size(); k++) {
        if (g_wild.raw[k].scatter) g_wild.scatterEnd = k + 1;
    }
}

// The deep fan. Built ONLY when the whole normal list came back without a single
// pot, and it is the reason the bot no longer settles for a legal miss while a
// pot is still on the table. The normal targeted fan samples each ball's contact
// window with 13 rays, which is about 0.6 units of ball face per ray - enough for
// a pocket a short distance away, marginal for a long one, and a pot that needs
// finer aim than the sampling falls straight through the net. This re-samples the
// same windows at 27 rays (0.28 units) and at three powers, so a long thin pot
// that the first pass could not resolve gets found on the second.
//
// It is expensive by design and it is only ever reached on a table where the
// alternative was giving the turn away, which costs the whole game.
static void BuildWildDeepFan() {
    using WildRaw = AutoPlay::WildRaw;
    Prediction::SceneData& sim = gPrediction->guiData;
    const Point2D cuePos = sim.balls[0].initialPosition;

    Ball::Classification myclass = sharedGameManager.getPlayerClassification();
    bool isNineBallGame = (myclass == Ball::Classification::NINE_BALL_RULE);
    bool onlyEightBallLeft = ComputeOnlyEightBallLeft(myclass);

    const double pMin = (double)AutoPlay::powerMin, pMax = (double)AutoPlay::powerMax;
    const double powers[3] = { pMax,
                               pMin + (pMax - pMin) * 0.70,
                               pMin + (pMax - pMin) * 0.45 };

    g_wild.raw.clear();

    for (int i = 1; i < sim.ballsCount; i++) {
        const auto& b = sim.balls[i];
        if (!b.originalOnTable) continue;
        if (!isNineBallGame) {
            bool isTarget = onlyEightBallLeft ? (i == 8)
                            : (myclass == Ball::Classification::ANY)
                                ? (b.classification != Ball::Classification::EIGHT_BALL)
                                : (b.classification == myclass);
            if (!isTarget) continue;
        }
        Point2D d = b.initialPosition - cuePos;
        double dist = sqrt(d.square());
        if (dist < 2.0 * BALL_RADIUS) continue;

        double centre = atan2(d.y, d.x);
        double half   = asin(std::min(1.0, (2.0 * BALL_RADIUS) / dist));
        for (int k = 0; k < 27; k++) {
            double a = centre + half * (-1.0 + 2.0 * (double)k / 26.0) * 0.98;
            if (a < 0) a += 2 * M_PI;
            if (a >= 2 * M_PI) a -= 2 * M_PI;
            for (int p = 0; p < 3; p++) {
                Candidate c = {i, a, 30.0, -1, powers[p], 30.0};
                g_wild.raw.push_back({c, AutoPlay::FAM_SWEEP});
            }
        }
    }

    // Fine full-circle net for the snookered case, where no direct line to any
    // of our balls exists and the only contact left comes off a cushion.
    for (double a = 0.0; a < 2.0 * M_PI; a += 0.055) {
        Candidate c = {1, a, 40.0, -1, pMax, 40.0};
        g_wild.raw.push_back({c, AutoPlay::FAM_SWEEP});
    }

    DedupeWildRaw();

    // The whole deep list IS the fan, so there is no span to hold out for -
    // by the time we are here the normal pass already found nothing, and the
    // job is to find ANY pot rather than to hold out for a bigger one.
    g_wild.scatterEnd = 0;
}

// -----------------------------------------------------------------------------
// The refinement grid: a dense local re-sample around the best shots found so
// far. This is the answer to "it pots them one at a time", and it is a separate
// pass rather than more probes inside the main loop because it must only be paid
// for on the handful of lines that already proved they do something.
//
// A scatter is a chain: cue hits ball A, A and the cue both go somewhere, one of
// them reaches ball B, and so on. Every link multiplies the sensitivity of the
// last, so the difference between one ball dropping and four is routinely a
// fraction of a degree at the cue - far finer than any fan that has to cover the
// whole table can afford to sample. The main list finds the neighbourhood; this
// finds the shot inside it.
//
// Two axes, because a scatter needs both and the coarse fan can only vary one:
//   angle  - decides which SIDE of the next ball the cue passes, and therefore
//            whether the chain continues at all
//   power  - decides how far the cue keeps going after each contact, and
//            therefore how many links the chain has before it runs out
//
// `evals` is deliberately not cleared before this runs, so the shot already in
// hand stays in the running. A refinement round can only ever improve the
// result; it cannot lose the pot we came in with.
static void BuildWildRefineFan(int round) {
    std::vector<AutoPlay::WildEval> seeds = g_wild.evals;
    std::sort(seeds.begin(), seeds.end(),
              [](const AutoPlay::WildEval& a, const AutoPlay::WildEval& b) {
                  if (a.own != b.own) return a.own > b.own;
                  if (a.tot != b.tot) return a.tot > b.tot;
                  return a.leave < b.leave;
              });
    if (seeds.size() > 6) seeds.resize(6);

    // Each round searches a DIFFERENT SHAPE of neighbourhood. The previous
    // version rebuilt one fixed grid every round from a re-sorted top-6 that
    // was usually the same top-6, so round 2 re-walked round 1's ground and
    // came back empty by construction. Measured over two full games: refinement
    // improved the shot in 3 turns out of 18, and 12 of the other 15 fired on
    // NO_PROGRESS after exactly one barren round.
    //
    //   1 LOCAL COARSE - the original grid. Finds the pot next door.
    //   2 LOCAL FINE   - a quarter of the step over a fifth of the span. A
    //                    tenth of a degree on the cut is the difference between
    //                    the object ball missing its neighbour and clipping it,
    //                    and round 1 steps straight over that.
    //   3 WIDE         - +/- 11 degrees. Far enough to reach a different first
    //                    contact entirely: another object ball, or the cut that
    //                    opens a combo.
    //   4 POWER SWEEP  - eight power levels down to a sixth of full over a
    //                    narrow angle span. Power is the axis nothing has been
    //                    searching: 14 of the 18 shots in the last two games
    //                    fired at exactly powerMax, because every grid so far
    //                    offered four levels all in the top half of the range,
    //                    and scatters live at the speed where the object ball
    //                    stops short against its neighbour instead of driving
    //                    clear of it.
    //   5 LOCAL FINE   - polish. Only ever reached if 3 or 4 found something,
    //                    so its seeds are new even though its shape is not.
    struct RefineShape {
        int    halfSteps;   // angle samples each side of the seed
        double angStep;     // radians between samples
        double dedupQ;      // angle dedup quantum, matched to angStep
        int    nPower;
        double powFrac[8];  // fraction of the power span, high to low
    };
    static const RefineShape kShapes[5] = {
        { 6, 0.0080, 0.0040, 4, {1.00, 0.82, 0.62, 0.42} },
        { 7, 0.0020, 0.0010, 4, {1.00, 0.82, 0.62, 0.42} },
        { 8, 0.0250, 0.0060, 4, {1.00, 0.80, 0.58, 0.36} },
        { 3, 0.0080, 0.0040, 8, {1.00, 0.88, 0.76, 0.64, 0.52, 0.40, 0.28, 0.16} },
        { 7, 0.0020, 0.0010, 4, {1.00, 0.82, 0.62, 0.42} },
    };
    const RefineShape& sh = kShapes[std::max(0, std::min(4, round - 1))];

    const double pMin = (double)AutoPlay::powerMin, pMax = (double)AutoPlay::powerMax;
    const double span = pMax - pMin;

    g_wild.raw.clear();
    g_wild.raw.reserve(seeds.size() * sh.halfSteps * 2 * sh.nPower);

    // Offset-major, then seed, then power - NOT seed-major. The pass almost
    // never finishes: at roughly 200 sims a second against a budget of 0.65 to
    // 1.10 seconds, a 300-candidate grid gets through about half. Seed-major
    // ordering spent that half entirely on seeds 1 and 2 and never simulated a
    // single sample around seeds 3 to 6. Ordering by distance from the seed
    // instead means a truncated pass has checked the closest offsets of EVERY
    // seed, which is where the improvements actually are.
    for (int step = 1; step <= sh.halfSteps; step++) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            for (const auto& s : seeds) {
                double a = s.c.angle + sh.angStep * (double)(sgn * step);
                if (a < 0) a += 2 * M_PI;
                if (a >= 2 * M_PI) a -= 2 * M_PI;
                for (int p = 0; p < sh.nPower; p++) {
                    Candidate c = s.c;
                    c.angle = a;
                    c.power = pMin + span * sh.powFrac[p];
                    // FAM_SWEEP on purpose. These are no longer the geometric solve
                    // they grew out of - the aim has been moved off it deliberately -
                    // so the judge must not hold them to the original ball-and-pocket
                    // contract, and the single-probe path is the right one because
                    // this grid already varies power itself.
                    g_wild.raw.push_back({c, AutoPlay::FAM_SWEEP, false});
                }
            }
        }
    }

    DedupeWildRaw(sh.dedupQ);
    // The WHOLE refinement grid is scatter-hunting territory, so mark all of it
    // as such. This used to be 0, which the time gate reads as "the fan is
    // already behind us" - so the grid built specifically to turn two balls into
    // four was handed the SHORT branch of every budget (0.10 s at three balls
    // instead of 0.65 s) and got about 5% of the way through itself before the
    // clock cut it off. Measured over a full game: refinement rounds examined
    // 11 of 204 candidates, twice, and then fired the two-ball shot it started
    // with. The long branch is what the table asks for while a fan is pending,
    // and a freshly-built grid is the definition of pending.
    g_wild.scatterEnd = g_wild.raw.size();
}

// -----------------------------------------------------------------------------
// Judges the scene left behind by ONE determineShotResult call: is this shot
// legal, and what did it sink? Split out of the loop deliberately. The old code
// ran ten sims back to back and then judged whatever the tenth left in guiData,
// which on the wild path was always the powerMin = 0 probe - a shot that pots
// nothing, so every candidate was rejected and the wild scanner could not
// return a pot at all. Judging each sim the moment it finishes makes that whole
// class of bug unrepresentable.
//
// Returns false ONLY when the shot is an outright foul. A legal shot that pots
// nothing still comes back, tagged TIER_LEGAL - that is the change that stops
// the bot handing over the turn. It used to return false whenever `own == 0`,
// so on any table with no pot available the scanner finished with an empty
// result set, dropped to IDLE and let the shot clock run out.
static bool EvaluateWildSim(const AutoPlay::WildRaw& raw,
                            Ball::Classification myclass, uint nominatedPocket,
                            bool isNineBallGame, bool onlyEightBallLeft,
                            bool eightMustBank,
                            AutoPlay::WildEval* out) {
    const Prediction::SceneData& g = gPrediction->guiData;

    // A computed candidate carries real geometry, so the sim has to confirm the
    // exact ball and pocket it was solved for. A blind sweep ray aimed at
    // nothing, so only the game's own legality rules apply to it - and it ranks
    // last, so it can only ever win by potting strictly more.
    const bool aimed = (raw.family != AutoPlay::FAM_SWEEP);

    if (!g.balls[0].onTable) return false;      // scratch
    auto firstHit = g.collision.firstHitBall;
    if (!firstHit) return false;                // no contact at all

    // The cue must strike a ball we are allowed to strike.
    if (!isNineBallGame) {
        if (onlyEightBallLeft) {
            if (firstHit->index != 8) return false;
        } else if (myclass == Ball::Classification::ANY) {
            if (firstHit->classification == Ball::Classification::EIGHT_BALL) return false;
        } else if (firstHit->classification != myclass) {
            return false;
        }
    }

    // Past this line the shot is LEGAL. Everything below decides how good it is,
    // and a demotion is never a rejection.
    int family = raw.family;

    // Hitting a different ball first than the one we solved for means the
    // geometry did not play out - but the shot is still legal, and it may still
    // have potted something. Demote it to luck rather than discard it.
    if (aimed && firstHit->index != raw.c.idx) family = AutoPlay::FAM_SWEEP;

    int tot = 0, own = 0, oppPotted = 0;
    bool eightPotted = false, ninePotted = false;
    int ownInAimedPocket = -1, ownAny = -1;

    for (int i = 1; i < g.ballsCount; i++) {
        const Prediction::Ball& b = g.balls[i];
        if (!b.originalOnTable || b.onTable) continue;
        tot++;
        if (i == 8) eightPotted = true;
        if (i == 9) ninePotted = true;

        bool mine = isNineBallGame ? true
                    : onlyEightBallLeft ? (i == 8)
                    : (myclass == Ball::Classification::ANY)
                        ? (b.classification != Ball::Classification::CUE_BALL &&
                           b.classification != Ball::Classification::EIGHT_BALL)
                        : (b.classification == myclass);
        if (!mine) { if (i != 8) oppPotted++; continue; }
        // A nominated pocket binds every ball we claim credit for.
        if (nominatedPocket < 6 && b.pocketIndex != (int)nominatedPocket) continue;
        own++;
        if (ownAny < 0) ownAny = i;
        if (aimed && b.pocketIndex == raw.c.pocketIndex && ownInAimedPocket < 0)
            ownInAimedPocket = i;
    }

    // Sinking the 8 before the group is clear loses the game outright. This is
    // the one "pot" that is worse than any miss, so it stays a hard reject.
    if (!isNineBallGame && eightPotted && !onlyEightBallLeft) return false;

    // The 8 must reach a cushion first on these tables. The sim exposes only a
    // whole-shot rail counter, not the ORDER of events, so this is a necessary
    // condition rather than a sufficient one: a shot that banks before touching
    // the 8 would pass. A FAM_BANK candidate is exempt from the doubt - its
    // geometry puts the cushion before the contact by construction.
    if (eightPotted && eightMustBank &&
        raw.family != AutoPlay::FAM_BANK && g.collision.railCollisions == 0) {
        return false;
    }

    out->c      = raw.c;
    out->family = family;
    out->tot    = tot;
    out->own    = own;
    out->win    = isNineBallGame ? ninePotted : (onlyEightBallLeft && eightPotted);

    if (own == 0) {
        // Legal, but nothing of ours went down. Keep it as the guaranteed move
        // of last resort and score it as a safety: leave the cue far from the
        // opponent's balls, and never gift them a pot.
        out->tier = AutoPlay::TIER_LEGAL;
        out->win  = false;
        const Point2D& cueRest = g.balls[0].predictedPosition;
        double nearestOpp = 1e9;
        for (int i = 1; i < g.ballsCount; i++) {
            const Prediction::Ball& b = g.balls[i];
            if (!b.onTable) continue;
            bool mine = isNineBallGame ? true
                        : onlyEightBallLeft ? (i == 8)
                        : (myclass == Ball::Classification::ANY)
                            ? (b.classification != Ball::Classification::EIGHT_BALL)
                            : (b.classification == myclass);
            if (mine) continue;
            nearestOpp = std::min(nearestOpp, sqrt((b.predictedPosition - cueRest).square()));
        }
        if (nearestOpp > 1e8) nearestOpp = 0.0;
        out->leave = -nearestOpp + 400.0 * (double)oppPotted;
        return true;
    }

    // Take the ball that reached the pocket we solved for - NOT the lowest
    // index we happened to pot. Reading the first index instead is what made
    // the old gate discard multi-ball shots: when a shot sank two of ours and
    // the smaller index rolled into a different pocket, the aimed-pocket test
    // saw that ball, failed, and threw away the whole shot. Multi-ball scatters
    // are the entire point of this style, so they cannot be collateral damage.
    int target = ownInAimedPocket;
    if (target < 0) {
        // It potted one of ours, just not into the pocket we aimed at. Still a
        // pot, still worth firing - but it was luck, so rank it as luck.
        target = ownAny;
        family = AutoPlay::FAM_SWEEP;
        out->family = family;
    }

    out->tier        = AutoPlay::TIER_POT;
    out->c.idx       = target;
    out->c.pocketIndex = g.balls[target].pocketIndex;
    out->leave       = ComputeLeaveScore(target, myclass, onlyEightBallLeft, (int)nominatedPocket)
                     + ComputeShotReliability(target, out->c.pocketIndex)
                     + 300.0 * (double)oppPotted;
    return true;
}

// =============================================================================
void AutoPlay::ScanWild() {
    if (g_CurrentCandidate.idx != -1) return;
    if (!sharedGameManager) return;

    auto& cueBall = gPrediction->guiData.balls[0];

    // Start a scan when none is live, or restart when the cue ball has moved
    // (ball-in-hand drag) - every candidate is solved from the cue position, so
    // a move invalidates all of them. Otherwise fall through and RESUME where
    // the previous frame ran out of its time budget.
    double distSq = (cueBall.initialPosition - g_wild.scanCuePos).square();
    bool firstFrameOfScan = false;
    if (!g_wild.isInitiated || distSq > 0.0025) {
        g_wild.raw.clear();
        g_wild.evals.clear();
        g_wild.evalIndex = 0;
        g_wild.pass = 0;
        g_wild.refineRounds = 0;
        g_wild.refineBaseOwn = 0;
        g_wild.refineBarren = 0;
        g_wild.haveFallback = false;
        g_wild.fallback = AutoPlay::WildEval{};
        g_wild.scanCuePos = cueBall.initialPosition;
        g_wild.scanStart = nowSec();
        g_wild.passStart = g_wild.scanStart;
        g_wild.isInitiated = true;
        firstFrameOfScan = true;
        g_wild.simCount = 0;
        g_wild.frames = 0;

        if (currentMode == MODE_AUTO_AIM && bAimedThisTurn) return;

        // Push the spin into the game FIRST, then read back what the cue will
        // actually carry and hold it for the whole scan. Every rung, the final
        // validation and the fired shot all read this one value. The old order
        // simulated with whatever spin happened to be set, then let Shoot()
        // apply the preset at fire time - so the proven shot and the fired shot
        // could disagree on spin, which is a different shot.
        applyAutoSpin();
        g_wild.spin = sharedGameManager.getShotSpin();

        BuildWildCandidates();
        LOGI("[WildDbg] SCAN START raw=%d scatterEnd=%d",
             (int)g_wild.raw.size(), (int)g_wild.scatterEnd);
    }

    g_wild.frames++;

    // Live rules for this scan. ComputeOnlyEightBallLeft is shared with the
    // generator, so the judge and the geometry can never disagree about whether
    // the 8 is a legal target.
    Ball::Classification myclass = sharedGameManager.getPlayerClassification();
    uint nominatedPocket = sharedGameManager.getNominatedPocket();
    bool isNineBallGame = (myclass == Ball::Classification::NINE_BALL_RULE);
    bool onlyEightBallLeft = ComputeOnlyEightBallLeft(myclass);
    bool eightMustBank = onlyEightBallLeft && sharedGameManager.is8BallCushionShotRequired();

    // Wall-clock budget, not a candidate count. A count is the wrong unit: 25
    // sims is idle time on a fast phone and a dropped frame on a slow one.
    // Metering a fixed SLICE of each frame lets a fast device burn the whole
    // list in one frame and a slow device spread it over several, and neither
    // misses its vsync. This is the answer to "how do they play that fast
    // without the frames dropping" - the work is bounded by the clock.
    //
    // The first frame of a scan gets double the slice. The turn has only just
    // started, nothing is animating yet, and one long frame there is invisible -
    // whereas it is the frame that decides whether the shot leaves immediately
    // or three frames later.
    const double kFrameSlice = firstFrameOfScan ? 0.011 : 0.0075;
    // A LAST-RESORT deadline, not a working one. It exists so a pathological
    // table can never let the shot clock run out, and for no other reason.
    // It used to be 1.10 s and it used to be able to end a scan that had found
    // nothing - which meant the bot fired the non-potting fallback while real
    // pots were still sitting unexamined further down the list. That is one of
    // the two ways it ended up gifting the turn away. Committing early is only
    // ever right when a pot is already in hand; with no pot there is nothing to
    // commit to, so the scan runs the list out instead. The shot clock is tens
    // of seconds, and a few of them spent finding a four-ball shot instead of a
    // one-ball shot is the trade this style exists to make.
    constexpr double kSafetyDeadline = 9.0;
    const double frameT0 = nowSec();
    const double elapsed = frameT0 - g_wild.scanStart;

    // How long to keep looking once a pot is in hand depends on how good that pot
    // is AND on whether the scatter fan has actually been simulated yet.
    //
    // That second condition is the fix for "it pots them one at a time". A time
    // budget alone cannot express what we want, because the value of stopping
    // depends on WHICH candidates have been looked at, not on how many
    // milliseconds have passed. A one-ball pot found in the direct solves used to
    // buy 0.24 s, which is roughly the cost of the specials and banks that sat in
    // front of the fan - so the clock ran out at almost exactly the index where
    // the multi-ball shots begin, and the bot fired the single every time. Every
    // shot that scatters three balls lives inside that fan span, so a one-ball pot
    // is now simply not allowed to end the scan until the span is behind us. It
    // costs the fan's simulations and nothing else: after scatterEnd, one ball
    // commits faster than it ever did.
    //
    // Every cap is measured from passStart, not scanStart. Each pass - the normal
    // list, the deep fan, each refinement round - is a separate budget, so a
    // refinement round that begins late in the scan still gets its full look.
    // The absolute safety deadline from scanStart is the only ceiling that
    // carries across passes.
    int bestOwnSoFar = 0;
    for (const auto& ev : g_wild.evals) bestOwnSoFar = std::max(bestOwnSoFar, ev.own);
    const bool scatterCovered = g_wild.evalIndex >= g_wild.scatterEnd;

    const double passElapsed = frameT0 - g_wild.passStart;
    // Forcing the first sweep to cover the whole scatter fan before it could
    // stop was tried and measured, and it made the bot play WORSE - 1.2 balls a
    // shot against 2.6. The fan is cheap blind rays with a low hit rate; the
    // computed ghost-ball solves that actually pot all sit in the first ~100
    // candidates. Covering 288 candidates yielded 28 surviving shots where
    // covering 110 yielded 39, because the extra two seconds went on rays that
    // pot nothing AND starved the refinement rounds, which are what were
    // producing the upgrades. Coverage of the fan is not the bottleneck.
    bool outOfTime =
           elapsed > kSafetyDeadline
        || (bestOwnSoFar >= 5 && passElapsed > 0.03)
        || (bestOwnSoFar >= 3 && passElapsed > (scatterCovered ? 0.10 : 0.65))
        || (bestOwnSoFar == 2 && passElapsed > (scatterCovered ? 0.35 : 1.10))
        || (bestOwnSoFar == 1 && scatterCovered && passElapsed > 0.55);
    bool goodEnough = false;

    // guiData is the single shared overlay buffer and every sim overwrites it.
    Prediction::SceneData savedGuiData = gPrediction->guiData;

    const double pMin = (double)powerMin, pMax = (double)powerMax;

    while (!outOfTime && !goodEnough && g_wild.evalIndex < g_wild.raw.size()) {
        const AutoPlay::WildRaw raw = g_wild.raw[g_wild.evalIndex++];
        const bool aimed = (raw.family != AutoPlay::FAM_SWEEP);

        AutoPlay::WildEval best{};
        bool haveBest = false;

        // One simulation, folded into the running best for this candidate.
        // Returns the number of OUR balls it sank, 0 for a legal miss, -1 for an
        // outright foul. Every probe is quantised to a power the cue can
        // actually store, so the shot that gets fired is bit-for-bit the shot
        // that was proven.
        auto probeShot = [&](double dAngle, double power) -> int {
            const double a = NumberUtils::normalizeDoublePrecision(
                                 normalizeAngle(raw.c.angle + dAngle));
            const double p = QuantizeShotPower(std::clamp(power, pMin, pMax));

            g_wild.simCount++;

            AutoPlay::WildRaw probe = raw;
            probe.c.angle = a;
            probe.c.power = p;

            gPrediction->forceFullSimulation = true;
            gPrediction->determineShotResult(true, a, p, g_wild.spin, probe.c);
            gPrediction->forceFullSimulation = false;

            AutoPlay::WildEval ev{};
            if (!EvaluateWildSim(probe, myclass, nominatedPocket, isNineBallGame,
                                 onlyEightBallLeft, eightMustBank, &ev)) return -1;
            ev.c.angle = a;
            ev.c.power = p;

            bool better = !haveBest
                       || ev.tier > best.tier
                       || (ev.tier == best.tier && ev.own > best.own)
                       || (ev.tier == best.tier && ev.own == best.own && ev.tot > best.tot)
                       || (ev.tier == best.tier && ev.own == best.own &&
                           ev.tot == best.tot && ev.leave < best.leave);
            if (better) { best = ev; haveBest = true; }
            return ev.own;
        };

        // ---- PROBE PLAN ----
        // Adaptive, not a fixed ladder. The old version ran a 3-power ladder at
        // up to 3 angles and leaned on break conditions to stop early, which
        // still cost 4 simulations on a candidate that pots nothing - and most
        // candidates on a crowded table pot nothing. Here each probe is only
        // paid for once the previous ones prove it worth paying for.
        if (!aimed) {
            // A blind ray at the power it was generated with - the deep fan
            // varies that per ray, so it cannot be hard-coded to pMax here.
            // What a blind ray pots is decided by WHERE it lands on the ball far
            // more than by how hard, so one probe is enough. The exception is a
            // foul at full pace - usually the cue ball flying in off - where the
            // same line at three-quarter pace can be perfectly legal.
            const double pw = std::clamp(raw.c.power, pMin, pMax);
            const int r = probeShot(0.0, pw);
            if (r < 0 && pw > pMin + (pMax - pMin) * 0.8) {
                probeShot(0.0, pMin + (pMax - pMin) * 0.72);
            } else if (r > 0 && r < 5 && bestOwnSoFar < 5) {
                // This ray already pots. That is the expensive part - it means
                // the cue ball is going somewhere useful through traffic - and
                // the rays either side of it are almost free to check by
                // comparison, because the cost of finding a live line has
                // already been paid. A sixth of a degree changes which side of
                // the next ball the cue passes, and that is what separates
                // clipping a cluster from splitting it. Only paid on rays that
                // already scored.
                if (probeShot(+0.006, pw) < 5) probeShot(-0.006, pw);
            }
        } else {
            const double geom = std::clamp(raw.c.power, pMin, pMax);
            const int r0 = probeShot(0.0, geom);

            if (r0 == 0) {
                // Legal contact on the right ball, and nothing dropped. TWO
                // completely different failures look identical from here, and
                // both have to be probed. The ball can be SHORT of the pocket,
                // which is a power failure that full power fixes outright; or it
                // can be off the pocket line, which is an aiming failure that no
                // power fixes. Probing only the aim - on the reasoning that
                // "power cannot fix an angular miss", which is true but answers
                // the wrong half of the question - is exactly what made the bot
                // fire shots that potted nothing: every long pot whose computed
                // power fell a little short was written off as a miss, and on a
                // table where the only pots were long ones the whole list came
                // back barren and the turn went to the opponent.
                if (probeShot(0.0, pMax) <= 0) {
                    if (probeShot(+0.011, geom) <= 0) probeShot(-0.011, geom);
                }
            } else if (r0 > 0) {
                // It pots at the power its own geometry asked for. Now find out
                // whether it pots MORE: the same line at full power keeps the
                // object ball on its way to the pocket but throws the cue ball
                // across the table with enough left to break something open.
                if (best.own < 5) probeShot(0.0, pMax);

                // Still short of a big shot, and nothing better exists anywhere
                // in this scan yet. A hair of angle at full power moves the
                // cue's post-contact path by a whole ball's width by the time it
                // reaches the far rail, which is the difference between passing
                // a cluster and splitting it - while the object ball, only a few
                // inches from its pocket, still drops. This is the single cheapest
                // way to turn one-ball shots into multi-ball ones. The gate used
                // to stop at two, which meant a two-ball shot was never nudged
                // toward three even though the nudge is exactly what produces the
                // third; the target is as many as the table will give.
                if (best.own < 5 && bestOwnSoFar < 5) {
                    if (probeShot(+0.009, pMax) < 5) probeShot(-0.009, pMax);
                }
            } else {
                // Missed, or fouled. More power does not fix an angular miss, so
                // the useful question is whether a hair either side of the exact
                // line lands - contact throw, cut-angle spin and the pocket's
                // real mouth shape all move the true aim off the textbook ghost
                // line, and a shot that misses by a hair reads as no shot at all.
                if (probeShot(+0.011, geom) <= 0) probeShot(-0.011, geom);
            }
        }

        if (haveBest) {
            if (best.tier == AutoPlay::TIER_POT) {
                g_wild.evals.push_back(best);
                // Keep this current WITHIN the frame, not just between frames.
                // It gates the scatter probes below, and a stale zero here means
                // every remaining candidate in the frame keeps paying for them
                // long after a multi-ball shot is already in hand.
                bestOwnSoFar = std::max(bestOwnSoFar, best.own);
                // Five balls off one shot is as much as any table realistically
                // gives. Stop there rather than grinding the rest of the list.
                if (best.own >= 5 || best.win) goodEnough = true;
            } else if (!g_wild.haveFallback ||
                       best.family > g_wild.fallback.family ||
                       (best.family == g_wild.fallback.family &&
                        best.leave < g_wild.fallback.leave)) {
                // Not a pot, but a legal shot - the guaranteed move if the whole
                // list turns out to be barren. Ranked by how it was derived and
                // then by how safe the leave is.
                g_wild.fallback = best;
                g_wild.haveFallback = true;
            }
        }

        // A sim cannot be interrupted half way, so the slice is enforced between
        // candidates.
        if (nowSec() - frameT0 >= kFrameSlice) break;
    }

    gPrediction->guiData = savedGuiData;

    const bool listDone = g_wild.evalIndex >= g_wild.raw.size();

    if (listDone || outOfTime || goodEnough) {
        // ---- PHASE 5: SELECT ----
        // Wild ranks on balls potted, full stop. `family` enters only to break a
        // tie at an equal count, so a computed ghost-ball solve beats a lucky
        // sweep ray that pots the same number - while a sweep ray that pots
        // strictly MORE of our balls still wins, which is where the big
        // scatters come from. Ordering those two the other way round is exactly
        // what made the previous attempt play worse than the code it replaced:
        // acceptance was widened before the selector learned to prefer computed
        // geometry, so zero-margin luck outranked real shots.
        const bool winFirst = !isNineBallGame || (nineBallStrategy != NINEBALL_BEST_SHOT);

        const AutoPlay::WildEval* best = nullptr;
        for (const auto& ev : g_wild.evals) {
            if (!best) { best = &ev; continue; }
            if (winFirst && ev.win != best->win) { if (ev.win) best = &ev; continue; }
            if (ev.own != best->own)             { if (ev.own > best->own) best = &ev; continue; }
            if (ev.family != best->family)       { if (ev.family > best->family) best = &ev; continue; }
            if (ev.tot != best->tot)             { if (ev.tot > best->tot) best = &ev; continue; }
            if (ev.leave < best->leave) best = &ev;
        }

        if (best) {
            // ---- REFINE: TURN TWO INTO FOUR ----
            // A pot is in hand, but not the scatter this style exists for. The
            // instruction is explicit - a slower shot that drops four beats a
            // fast one that drops one - so before firing, re-sample a dense
            // local grid of angle x power around the best few lines found. A
            // scatter is a chain of contacts and every link multiplies the
            // sensitivity of the last, so the gap between one ball and four is
            // routinely finer than any table-wide fan can afford to sample. The
            // fan finds the neighbourhood; this finds the shot inside it.
            //
            // Bounded three ways so it can never stall: refinement must keep
            // EARNING its next round, the absolute safety deadline, and the fact
            // that no round can lose the pot already held - `evals` is never
            // cleared, so the worst case is that the grid finds nothing and the
            // original shot is fired a few hundred milliseconds later.
            //
            // The brake is progress, not a round counter. Measured over two full
            // games, the old cap of 2 rounds fired on ROUNDS_EXHAUSTED in 10 of
            // 13 shots while rounds were still visibly climbing (1 -> 2 -> 3),
            // and stopped at 3.3 s against a 9 s allowance and a 30 s shot
            // clock - it was cutting off work that was still paying.
            //
            // One barren round is no longer the place to stop, though. It was,
            // while every round rebuilt the same shape of grid around the same
            // seeds: that round really had exhausted its neighbourhood and its
            // successor would have re-walked it. The rounds now differ in shape
            // - coarse-local, razor-fine, wide, power-sweep - so a fine grid
            // coming back empty says nothing about the wide one behind it.
            // Two consecutive barren rounds is the honest signal, and the hard
            // cap behind it is only a backstop against a pathological table.
            const int legalLeft = CountLegalTargets(myclass, isNineBallGame,
                                                    onlyEightBallLeft);
            const int target = std::min(5, legalLeft);
            const bool roundImproved =
                g_wild.refineRounds == 0 || best->own > g_wild.refineBaseOwn;
            const int barrenAfter = roundImproved ? 0 : g_wild.refineBarren + 1;
            const bool refineProgressed = barrenAfter < 2;
            LOGI("[WildDbg] SELECT own=%d tot=%d win=%d fam=%d | target=%d legalLeft=%d refineRounds=%d baseOwn=%d improved=%d barren=%d | pass=%d evalIdx=%d/%d scatterEnd=%d fanDone=%d | why=%s%s%s elapsed=%.3f passEl=%.3f sims=%d frames=%d evals=%d",
                 best->own, best->tot, (int)best->win, best->family,
                 target, legalLeft, g_wild.refineRounds, g_wild.refineBaseOwn,
                 (int)roundImproved, barrenAfter,
                 g_wild.pass, (int)g_wild.evalIndex, (int)g_wild.raw.size(),
                 (int)g_wild.scatterEnd, (int)scatterCovered,
                 listDone ? "LIST_DONE " : "", outOfTime ? "OUT_OF_TIME " : "",
                 goodEnough ? "GOOD_ENOUGH" : "",
                 elapsed, passElapsed, g_wild.simCount, g_wild.frames,
                 (int)g_wild.evals.size());
            if (!best->win && best->own < target &&
                refineProgressed &&
                g_wild.refineRounds < 5 &&
                elapsed < kSafetyDeadline - 1.5) {
                g_wild.refineBarren = barrenAfter;
                g_wild.refineRounds++;
                g_wild.refineBaseOwn = best->own;
                g_wild.evalIndex = 0;
                g_wild.passStart = nowSec();
                BuildWildRefineFan(g_wild.refineRounds);
                LOGI("[WildDbg] REFINE round=%d raw=%d baseOwn=%d barren=%d",
                     g_wild.refineRounds, (int)g_wild.raw.size(),
                     g_wild.refineBaseOwn, g_wild.refineBarren);
                // isInitiated stays true so the next frame resumes into the grid
                // instead of rebuilding the normal list and looping forever.
                return;
            }

            // ---- PHASE 6: FIRE ----
            // Nothing is deferred to a later frame: the shot leaves as soon as a
            // winner exists, which is usually the first frame of the turn.
            LOGI("[WildDbg] FIRE own=%d tot=%d fam=%d angle=%.4f power=%.1f | noRefineBecause=%s%s%s%s | rounds=%d totalSims=%d totalFrames=%d totalTime=%.3f",
                 best->own, best->tot, best->family, best->c.angle, best->c.power,
                 best->win ? "WIN " : "",
                 (best->own >= target) ? "TARGET_MET " : "",
                 !refineProgressed ? "TWO_BARREN_ROUNDS " : "",
                 (g_wild.refineRounds >= 5) ? "ROUNDS_EXHAUSTED " : "",
                 g_wild.refineRounds,
                 g_wild.simCount, g_wild.frames, elapsed);
            g_wild.isInitiated = false;
            g_CurrentCandidate = best->c;
            Shoot(best->c.angle, best->c.power);
            return;
        }

        // ---- SECOND CHANCE ----
        // The whole list came back without one pot. Before settling for a legal
        // miss - which gives the turn away, and giving the turn away is how games
        // are lost - re-sample every ball's contact window far more finely and at
        // three powers. A pot that needs finer aim than the first pass sampled is
        // invisible to it and obvious to this. isInitiated deliberately stays
        // true so the next frame RESUMES into the deep list instead of rebuilding
        // the normal one and looping between the two forever.
        if (listDone && !outOfTime && g_wild.pass == 0) {
            g_wild.pass = 1;
            g_wild.evalIndex = 0;
            g_wild.passStart = nowSec();
            BuildWildDeepFan();
            LOGI("[WildDbg] NO POT -> DEEP FAN raw=%d sims=%d elapsed=%.3f",
                 (int)g_wild.raw.size(), g_wild.simCount, elapsed);
            return;
        }

        g_wild.isInitiated = false;
        LOGI("[WildDbg] NO POT AT ALL | why=%s%s%s pass=%d evalIdx=%d/%d sims=%d frames=%d elapsed=%.3f haveFallback=%d",
             listDone ? "LIST_DONE " : "", outOfTime ? "OUT_OF_TIME " : "",
             goodEnough ? "GOOD_ENOUGH" : "",
             g_wild.pass, (int)g_wild.evalIndex, (int)g_wild.raw.size(),
             g_wild.simCount, g_wild.frames, elapsed, (int)g_wild.haveFallback);

        // ---- GUARANTEED MOVE ----
        // No pot anywhere on the table. This used to record the cue position as
        // barren and drop to IDLE, and Update() then refused to re-enter
        // SCANNING at that position - so the bot sat there for the rest of the
        // turn and let the clock hand it to the opponent. Passing the turn is
        // never the right answer; a legal miss is strictly better than not
        // playing, and a foul is still better than a timeout.
        if (g_wild.haveFallback) {
            g_CurrentCandidate = g_wild.fallback.c;
            Shoot(g_wild.fallback.c.angle, g_wild.fallback.c.power);
            return;
        }

        // Nothing even made a legal contact - every simulated shot scratched or
        // hit the wrong ball first. Aim straight down the line at the nearest
        // ball we are allowed to hit and roll it. This is the "at minimum, play
        // a completely direct shot" floor: it does not need the sim to agree,
        // because there is no alternative left except standing still.
        const Prediction::SceneData& sim = gPrediction->guiData;
        int nearest = -1;
        double nearestDist = 1e9;
        for (int i = 1; i < sim.ballsCount; i++) {
            const auto& b = sim.balls[i];
            if (!b.originalOnTable) continue;
            bool legal = isNineBallGame ? true
                         : onlyEightBallLeft ? (i == 8)
                         : (myclass == Ball::Classification::ANY)
                             ? (b.classification != Ball::Classification::EIGHT_BALL)
                             : (b.classification == myclass);
            if (!legal) continue;
            double d = sqrt((b.initialPosition - cueBall.initialPosition).square());
            if (d < nearestDist) { nearestDist = d; nearest = i; }
        }

        if (nearest >= 0 && cueBall.originalOnTable) {
            Point2D line = sim.balls[nearest].initialPosition - cueBall.initialPosition;
            double ang = atan2(line.y, line.x);
            if (ang < 0) ang += 2 * M_PI;
            double pw = QuantizeShotPower(std::clamp(
                CalculateRequiredPower(nearestDist * 1.6), pMin, pMax));
            g_CurrentCandidate = {nearest, ang, 0.0, -1, pw, nearestDist};
            Shoot(ang, pw);
            return;
        }

        // Genuinely nothing to shoot at - no cue ball on the table, or no legal
        // target exists. Back off briefly and retry rather than blocking this
        // cue position permanently, which is what the old lastFailedCuePos gate
        // did. A short cooldown keeps the retry off the hot path without ever
        // making the halt permanent.
        g_wild.retryAfter = nowSec() + 0.35;
        g_autoPlayCalculating = false;
        state = IDLE;
    }
}

void AutoPlay::Update() {
    frameCounter++;
    buttonClicker.Update();

    // Restate the aim angle BEFORE the slider runs, not after.
    //
    // The shot physically LEAVES inside powerSlider.Update() below: its End()
    // releases the slider touch and the engine reads mAimAngle at that instant.
    // Our own re-assert used to sit ~240 lines further down this function, so on
    // the one frame that matters - the firing frame - it ran AFTER the ball had
    // already been struck. What actually fired was the PREVIOUS frame's angle,
    // minus whatever the game's aim controller had drifted it by in between
    // (we run in the eglSwapBuffers hook, so a full round of the game's own
    // input processing happens between our writes). That is the hair of movement
    // felt right before the shot, and a hair is the whole margin: the refinement
    // grid resolves angles down to 0.008 rad, so a drift far too small to see is
    // still enough to turn a proven pot into a miss and hand over the turn.
    //
    // This was reverted once, on the theory that it was costing balls. It was
    // not: the ball count was being held down by a scatterEnd bug in the
    // refinement fan, which was present in both builds. Restored.
    if (gPrediction && anim_IsPulling && (fastShotState == 1 || fastShotState == 2)) {
        setAimAngle(anim_TargetAngle);
    }

    powerSlider.Update();

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
        if (lastFrameCuePos.x == -1000.0) {
            lastFrameCuePos = currentCuePos;
        }
        double dx = currentCuePos.x - lastFrameCuePos.x;
        double dy = currentCuePos.y - lastFrameCuePos.y;
        double distSq = dx * dx + dy * dy;
        if (distSq > 0.0001) {
            framesCueBallStill = 0;
        } else {
            if (framesCueBallStill < 10) {
                framesCueBallStill++;
            }
        }
        lastFrameCuePos = currentCuePos;
    } else {
        framesCueBallStill = 10;
        lastFrameCuePos = {-1000.0, -1000.0};
    }
    bCueBallIsMovingOrDragging = (framesCueBallStill < 5);

    // Shot-fire diagnostic (edge detector): the first frame the cue ball
    // exceeds the motion threshold, log its actual direction/speed vs the
    // angle/power the simulation planned. Large dA = the fired shot diverges
    // from the sim (aim convention, y-flip, or stale aim at fire time).
    if (sharedGameManager) {
        Table _t = sharedGameManager.mTable;
        if (_t) {
            auto& _balls = _t.mBalls();
            if (_balls && _balls.Count > 0) {
                Ball _cue = _balls[0];
                double sp = 0.0;
                if (_cue) {
                    static double prevCueSpeed = 0.0;
                    auto vel = _cue.velocity();
                    sp = sqrt(vel.x * vel.x + vel.y * vel.y);
                    if (sp > 20.0 && prevCueSpeed <= 20.0) {
                        double fired = atan2(vel.y, vel.x);
                        if (fired < 0.0) fired += 2.0 * M_PI;
                        LOGI("[FireDbg] MOTION angle=%.4f speed=%.1f | planned angle=%.4f power=%.1f | dA=%.4f rad",
                             fired, sp, pendingShotAngle, pendingShotPower,
                             normalizeAngle(fired) - normalizeAngle(pendingShotAngle));
                    }
                    prevCueSpeed = sp;
                }
                // Object-ball transfer measurement: log the first non-cue ball
                // that starts moving, plus the cue's speed on the PREVIOUS
                // frame (closing speed) and the current frame (post collision).
                // obj/closing = (1+e)/2 lets us calibrate the restitution.
                static double prevObjSpeed = 0.0;
                static double cueSpeedPrevFrame = 0.0;
                double objSpeed = 0.0;
                int objIdx = -1;
                for (int bi = 1; bi < (int)_balls.Count; bi++) {
                    Ball _ob = _balls[bi];
                    if (!_ob) continue;
                    auto ov = _ob.velocity();
                    double os = sqrt(ov.x * ov.x + ov.y * ov.y);
                    if (os > objSpeed) { objSpeed = os; objIdx = bi; }
                }
                if (objSpeed > 20.0 && prevObjSpeed <= 20.0 && objIdx > 0) {
                    auto cv = _cue.velocity();
                    double cueNow = sqrt(cv.x * cv.x + cv.y * cv.y);
                    LOGI("[FireDbg] OBJHIT idx=%d obj=%.1f closing~%.1f cueNow=%.1f xfer=%.3f",
                         objIdx, objSpeed, cueSpeedPrevFrame, cueNow,
                         objSpeed / (cueSpeedPrevFrame + 1e-9));
                }
                prevObjSpeed = objSpeed;
                cueSpeedPrevFrame = sp;
            }
        }
    }

    if (g_postShotLock) {
        if (g_postShotFrames > 0 && sharedGameManager) {
            setAimAngle(g_postShotAngle);
            setPower(g_postShotPower);
            g_postShotFrames--;
        } else {
            g_postShotLock = false;
            ClearState();
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

    bool humanRunning = (playStyle == STYLE_HUMAN && (humanState != HUM_IDLE || humanShotLocked));
    bool executingShot = anim_IsPulling || humanRunning;

    if (AreBallsMoving() && !executingShot) {
        if (state == SCANNING || state == NOMINATING) {
            ClearState();
            state = IDLE;
        }
        g_autoPlayCalculating = false;
        return;
    }

    // Periodic ruleset logging for debugging/validation (disabled: it was
    // flooding logcat and pushing out the shot-decision logs we need to read).
    if (false && sharedGameManager && frameCounter % 120 == 0) {
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
            if (playStyle == STYLE_WILD) {
                // حركة عصا بصرية فقط قبل الإطلاق: دورانان ونصف سريعان،
                // ثم تثبيت دقيق على نفس الزاوية والقوة المحسوبتين مسبقًا.
                // لا نعدّل anim_TargetAngle أو anim_TargetPower هنا.
                constexpr double wildSpinDuration = 0.42;
                constexpr double wildSpinTurns = 2.5;
                double spinT = std::clamp(elapsed / wildSpinDuration, 0.0, 1.0);
                double spinAngle = anim_TargetAngle + spinT * (2.0 * M_PI * wildSpinTurns);

                setAimAngle(anim_TargetAngle);
                if (!anim_TouchStarted) {
                    NativeTouchesBegin(5, jX, jY);
                    anim_TouchStarted = true;
                }

               /* if (spinT < 1.0) {
                    NativeTouchesMove(5, jX + (float)cos(spinAngle) * jR,
                                         jY + (float)sin(spinAngle) * jR);
                    return;
                }*/

                // نهاية الدوران: تثبيت العصا على الهدف ثم متابعة الإطلاق الأصلي.
                NativeTouchesMove(5, jX + (float)cos(anim_TargetAngle) * jR,
                                     jY + (float)sin(anim_TargetAngle) * jR);
                setAimAngle(anim_TargetAngle);
                anim_RotationDone = true;
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
        // The aim angle is restated at the TOP of Update() now, ahead of
        // powerSlider.Update() where the shot actually leaves. It used to be
        // restated here, which was both too late to affect the firing frame and
        // too broad - this line runs for state 3 as well, so it went on
        // overwriting the angle while the balls were still rolling.

        double elapsed_shot = nowSec() - stateStartTime;

        // State 1: STABILIZE PHASE (0.15 seconds) - hold joystick at target, then start power pull
        if (fastShotState == 1) {
            // Keep joystick held at EXACT target angle during stabilization.
            // This prevents the game from resetting aim direction.
            NativeTouchesMove(5, jX + (float)cos(anim_TargetAngle) * jR, 
                                 jY + (float)sin(anim_TargetAngle) * jR);
            setAimAngle(anim_TargetAngle);

            bool shouldTriggerPower = false;
            if (playStyle == STYLE_WILD) {
                shouldTriggerPower = true;
            } else if (elapsed_shot >= 0.15) {
                shouldTriggerPower = true;
            }

            if (shouldTriggerPower) {
                // Apply spin AFTER aiming has stabilized and BEFORE pulling power bar!
                applyAutoSpin();

                // Release joystick RIGHT before power slider starts.
                // Minimal gap between joystick release and power pull to prevent aim reset.
                NativeTouchesEnd(5, jX + (float)cos(anim_TargetAngle) * jR,
                                    jY + (float)sin(anim_TargetAngle) * jR);
                // Touch 5 is now gone. Without clearing this, ClearState() at the
                // end of the shot saw anim_TouchStarted still set and issued a
                // SECOND NativeTouchesEnd for a touch that no longer existed -
                // an unmatched release landing on the joystick centre, i.e. one
                // last "aim at nothing" delivered after the shot.
                anim_TouchStarted = false;

                float sliderXPercent = persistent_float[O("fPowerBarXPercent")];
                float sliderX = Width * sliderXPercent;
                if (persistent_int[O("iPowerBarSide")] == 1) {
                    sliderX = Width * (1.0f - sliderXPercent); // Right Side
                }
                float sliderYStart = Height * persistent_float[O("fPowerBarYStartPercent")];
                float sliderYEnd = Height * persistent_float[O("fPowerBarYEndPercent")];
                ImVec4 sliderRect(sliderX - 20.0f, sliderYStart, 40.0f, sliderYEnd - sliderYStart);
                if (playStyle == STYLE_WILD) {
                    powerSlider.SimulateDrag(sliderRect, anim_TargetPower, 0.40f, 0.20f);
                } else {
                    powerSlider.SimulateDrag(sliderRect, anim_TargetPower, 0.85f, 0.40f);
                }

                stateStartTime = nowSec();
                fastShotState = 2; // Transition to wait-for-slider phase
            }
            return;
        }

        // State 2: Wait for power slider to complete (slider already started in state 1)
        if (fastShotState == 2) {
            gPrediction->forceFullSimulation = true;
            gPrediction->determineShotResult(true, anim_TargetAngle, anim_TargetPower,
                                             sharedGameManager.getShotSpin(), g_CurrentCandidate);
            gPrediction->forceFullSimulation = false;

            if (powerSlider.Active) {
                return; // Wait for slider simulation to finish and release touch
            }

            stateStartTime = nowSec();
            fastShotState = 3;
            return;
        }

        // State 3: WAIT FOR BALLS TO STOP
        if (fastShotState == 3) {
            // Deliberately NOT re-asserting the aim angle here.
            //
            // The ball has already been struck; nothing about this angle can
            // change the shot any more. What it DID do was overwrite the aim
            // angle every frame while the balls rolled, so the guide lines
            // stayed pinned to the angle we had chosen while the game's own cue
            // settled wherever it settles after a shot - the lines pointing one
            // way and the cue another, right up until a real screen touch made
            // the game recompute and the two snapped back together. Leaving the
            // angle alone once the shot is gone lets them agree on their own.
            static double s_ballsStoppedAt = -1.0;
            if (s_ballsStoppedAt < stateStartTime) {
                s_ballsStoppedAt = stateStartTime;
            }

            bool timedOut = (nowSec() - stateStartTime > 12.0);

            if (AreBallsMoving() && !timedOut) {
                s_ballsStoppedAt = nowSec();
                return;
            }

            double settledFor = nowSec() - s_ballsStoppedAt;
            if (settledFor < 0.5 && !timedOut) {
                return;
            }

            s_ballsStoppedAt = -1.0;
            anim_IsPulling = false;
            anim_RotationDone = false;
            anim_TouchStarted = false;
            fastShotState = 0;
            ClearState();
            state = IDLE;
            g_lastFastShotTime = nowSec();
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
    if (wasPlayerTurn && !isPlayerTurn) {
        // The opponent may have received the turn after a miss, foul, or
        // game-state transition. Never carry a half-executed plan into the
        // next visit to the table.
        g_autoPlayCalculating = false;
        GameMaster::ResetPlan();
        HumanScan::ResetTurn();
        ClearState();
        bAimedThisTurn = false;
    }
    if (turnJustStarted) {
        bAimedThisTurn = false;
        lastFailedCuePos = {-1000.0, -1000.0};
        // A retry cooldown left over from the previous turn would push the first
        // scan of THIS turn up to 0.35 s later for no reason - the table it was
        // set on no longer exists. A fresh turn always scans on its first frame.
        g_wild.retryAfter = 0.0;
        g_wild.isInitiated = false;
        HumanScan::ResetTurn();
    }
    wasPlayerTurn = isPlayerTurn;

    static double turnStartTime = 0.0;
    if (turnJustStarted || (isPlayerTurn && turnStartTime == 0.0)) {
        turnStartTime = nowSec();
    }
    if (!isPlayerTurn) {
        turnStartTime = 0.0;
    }

    bool humanActive = (playStyle == STYLE_HUMAN && humanState != HUM_IDLE);
    
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
    humanRunning = (playStyle == STYLE_HUMAN && (humanState != HUM_IDLE || humanShotLocked));
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

    // Force re-scan when player's turn freshly begins or autoplay toggles
    static bool lastPlayerTurnState = false;
    static bool lastAutoPlayingState = false;
    if ((isPlayerTurn && !lastPlayerTurnState) || (bAutoPlaying != lastAutoPlayingState)) {
        HumanScan::ResetTurn();
        state = IDLE;
        g_wild.isInitiated = false;
    }
    lastPlayerTurnState = isPlayerTurn;
    lastAutoPlayingState = bAutoPlaying;

    // =====================================================================
    // HUMAN AI DECISION (HumanScan) - replaces Beast scanning for STYLE_HUMAN.
    // Runs only while idle with nothing in flight; takes one decision and
    // hands off to the human execution machine below via ExecuteHumanAI.
    // =====================================================================
    if (playStyle == STYLE_HUMAN && currentMode == MODE_AUTO_PLAY && bAutoPlaying && isPlayerTurn &&
        humanState == HUM_IDLE && state == IDLE) {
        if (HumanScan::RunIfReady()) {
            return;
        }
    }

    // Gate diagnostics: why the HumanScan branch isn't taking the frame.
    if (playStyle == STYLE_HUMAN && currentMode == MODE_AUTO_PLAY) {
        static double lastGateDbg = 0.0;
        double _n = AutoPlay::nowSec();
        if (_n - lastGateDbg > 2.0) {
            lastGateDbg = _n;
            LOGI("[HumanScan] gate auto=%d turn=%d hs=%d st=%d bm=%d anim=%d",
                 (int)bAutoPlaying, (int)isPlayerTurn, (int)humanState, (int)state,
                 (int)AutoPlay::AreBallsMoving(), (int)IsAnimationActive());
        }
    }

    // =====================================================================
    // HUMAN STATE MACHINE - Must run FIRST, before any animation checks!
    // =====================================================================
    if (playStyle == STYLE_HUMAN && humanState != HUM_IDLE) {
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
            gPrediction->forceFullSimulation = true;
            gPrediction->determineShotResult(true, targetAngle, pendingShotPower, sharedGameManager.getShotSpin(), g_CurrentCandidate);
            gPrediction->forceFullSimulation = false;
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
            gPrediction->forceFullSimulation = true;
            gPrediction->determineShotResult(true, targetAngle, pendingShotPower, sharedGameManager.getShotSpin(), g_CurrentCandidate);
            gPrediction->forceFullSimulation = false;
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
                NativeTouchesMove(5, jX + (float)cos(targetAngle) * jR, 
                                     jY + (float)sin(targetAngle) * jR);
                stateStartTime = now;
                humanState = HUM_STABILIZING;
            } else {
                double ease = sin(t * M_PI_2);
                double normalizedStart = normalizeAngle(nudgeAngle);
                double normalizedTarget = normalizeAngle(targetAngle);
                double delta = normalizedTarget - normalizedStart;
                if (delta > M_PI) delta -= 2.0 * M_PI; if (delta < -M_PI) delta += 2.0 * M_PI;
                double curAngle = normalizedStart + delta * ease;
                setAimAngle(curAngle);
                UpdateJoystickVisuals(curAngle);
            }
            gPrediction->forceFullSimulation = true;
            gPrediction->determineShotResult(true, targetAngle, pendingShotPower, sharedGameManager.getShotSpin(), g_CurrentCandidate);
            gPrediction->forceFullSimulation = false;
            return;
        }

        // 4. STABILIZE & LOCK (0.4s)
        if (humanState == HUM_STABILIZING) {
            float jX = Width * 0.83f;
            float jY = Height * 0.82f;
            float jR = 65.0f;
            NativeTouchesMove(5, jX + (float)cos(targetAngle) * jR, 
                                 jY + (float)sin(targetAngle) * jR);
            setAimAngle(targetAngle);
            if (now - stateStartTime >= 0.4) {
                if (humanShouldAutoFire) {
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
                    g_postAimFrames = 20;
                    state = IDLE; humanState = HUM_IDLE;
                }
            }
            return;
        }

        // 5. POWER PULL (0.85s smooth)
        if (humanState == HUM_PULLING) {
            setAimAngle(targetAngle);
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

            gPrediction->forceFullSimulation = true;
            gPrediction->determineShotResult(true, targetAngle, targetPower,
                                             sharedGameManager.getShotSpin(), g_CurrentCandidate);
            gPrediction->forceFullSimulation = false;

            if (powerSlider.Active) {
                return;
            }

            stateStartTime = now;
            humanState = HUM_DELAY_BEFORE_SHOT;
            return;
        }

        // 6. FINAL HUMAN PAUSE (0.4s) then FIRE!
        if (humanState == HUM_DELAY_BEFORE_SHOT) {
            setAimAngle(targetAngle);
            if (now - stateStartTime >= 0.4) {
                humanShotLocked = false;
                ClearState();
                state = IDLE; humanState = HUM_IDLE;
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

            gPrediction->forceFullSimulation = false;
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

    if (playStyle != STYLE_HUMAN) {
        if (state == IDLE) {
            bool shouldScan = (currentMode != MODE_AUTO_AIM) || !bAimedThisTurn;
            // A short time cooldown, not the old position-keyed block. That gate
            // tested lastFailedCuePos and refused to re-scan within 0.05 units
            // of it, so a single barren scan silenced the bot for the whole turn
            // even though the table had not changed in a way it could detect.
            // ScanWild now always finds something to fire, and reaches its retry
            // cooldown only when there is literally no legal target on the table.
            if (shouldScan && nowSec() < g_wild.retryAfter) shouldScan = false;
            if (shouldScan) {
                state = SCANNING;
                g_wild.isInitiated = false;
                g_autoPlayCalculating = false;
            }
        }
        if (state == SCANNING) {
            // One scanner now, no FAST/SLOW split. The split existed only
            // because FAST was broken and SLOW was the fallback; there is no
            // second mode to fall back to, and nothing to configure per frame -
            // ScanWild sizes its own per-frame work from the clock.
            ScanWild();
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
                    gPrediction->forceFullSimulation = true;
                    gPrediction->determineShotResult(true, pendingShotAngle, pendingShotPower,
                                                    sharedGameManager.getShotSpin(), g_CurrentCandidate);
                    gPrediction->forceFullSimulation = false;
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
                    if (playStyle == STYLE_HUMAN) {
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

        gPrediction->forceFullSimulation = true;
        gPrediction->determineShotResult(true, curAngle, curPower, sharedGameManager.getShotSpin());
        gPrediction->forceFullSimulation = false;
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

static bool ComputeCuePlacement(const std::vector<Point2D>& balls, Point2D& out) {
    const double inset = 2.0 * BALL_RADIUS;
    const double xMin = -TABLE_HALF_WIDTH + inset;
    const double xMax = TABLE_HALF_WIDTH - inset;
    const double yMin = -TABLE_HALF_HEIGHT + inset;
    const double yMax = TABLE_HALF_HEIGHT - inset;
    const double step = 8.0;
    const double minSepSq = (2.0 * BALL_RADIUS) * (2.0 * BALL_RADIUS);
    const auto& pockets = getPockets();

    // Head spot: 1/4 of the table length from the head rail (the rack/break
    // sits at the +x foot rail, so the head rail is at -TABLE_HALF_WIDTH).
    const Point2D headSpot(-TABLE_HALF_WIDTH + TABLE_WIDTH * 0.25, 0.0);

    Point2D best = headSpot;
    double bestScore = -1e18;
    bool found = false;

    for (double x = xMin; x <= xMax; x += step) {
        for (double y = yMin; y <= yMax; y += step) {
            const Point2D cand(x, y);

            // Reject candidates that would overlap an on-table ball.
            bool overlap = false;
            for (const Point2D& b : balls) {
                if ((cand - b).square() < minSepSq) { overlap = true; break; }
            }
            if (overlap) continue;

            // Count object balls with a clean line to ANY pocket from here.
            int openLines = 0;
            double nearestBallSq = 1e18;
            for (size_t i = 0; i < balls.size(); i++) {
                const Point2D& b = balls[i];
                double dSq = (cand - b).square();
                if (dSq < nearestBallSq) nearestBallSq = dSq;
                for (const Point2D& p : pockets) {
                    // Cue -> ball leg: any other ball in the corridor blocks.
                    bool blocked = false;
                    for (size_t j = 0; j < balls.size(); j++) {
                        if (j == i) continue;
                        if (DistToSegmentSq(balls[j], cand, b) < minSepSq) { blocked = true; break; }
                    }
                    if (blocked) continue;
                    // Ball -> pocket leg: other balls AND the cue at `cand`
                    // (a real ball once placed) must stay out of the corridor.
                    for (size_t j = 0; j < balls.size(); j++) {
                        if (j == i) continue;
                        if (DistToSegmentSq(balls[j], b, p) < minSepSq) { blocked = true; break; }
                    }
                    if (!blocked && DistToSegmentSq(cand, b, p) < minSepSq) blocked = true;
                    if (!blocked) { openLines++; break; } // one clean pocket is enough per ball
                }
            }

            if (openLines > 0) {
                double score = openLines * 1000.0 - sqrt(nearestBallSq) * 5.0 - cand.square() * 0.001;
                if (score > bestScore) { bestScore = score; best = cand; found = true; }
            }
        }
    }

    if (found) { out = best; return true; }

    // No open pot lines anywhere: park at the head spot, nudged along the
    // head string if a ball is already sitting on it.
    Point2D fallback = headSpot;
    for (int nudge = 0; nudge < 8; nudge++) {
        bool blocked = false;
        for (const Point2D& b : balls) {
            if ((fallback - b).square() < minSepSq) { blocked = true; break; }
        }
        if (!blocked) break;
        double sign = (nudge % 2 == 0) ? 1.0 : -1.0;
        fallback = Point2D(headSpot.x + sign * (nudge / 2 + 1) * 4.0, 0.0);
    }

    out = fallback;
    return true;
}

void AutoPlay::PlaceCueBall() {
    // Ball-in-hand: drag the cue to the best open-pot spot. Self-throttled ?
    // each frame advances a begin -> move -> release touch sequence, so the
    // caller (HumanScan::RunIfReady) can invoke this every frame until the
    // cue is actually placed. No-op when the cue is already on the table.
    if (gPrediction->guiData.balls[0].onTable) return;

    static int placePhase = 0;          // 0 = idle, 1 = dragging
    static double dragStartTime = 0.0;
    static ImVec2 startPos, targetPos;

    if (placePhase == 0) {
        std::vector<Point2D> onTable;
        for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
            if (gPrediction->guiData.balls[i].originalOnTable) {
                onTable.push_back(gPrediction->guiData.balls[i].initialPosition);
            }
        }

        Point2D targetWorld;
        ComputeCuePlacement(onTable, targetWorld);
        targetPos = WorldToScreen(targetWorld);

        // Start the drag on the cue's current screen position when it is
        // visible; otherwise touch down straight on the target spot.
        Point2D cueWorld = gPrediction->guiData.balls[0].initialPosition;
        ImVec2 cueScr = WorldToScreen(cueWorld);
        bool cueOnScreen = cueScr.x > TABLE_LEFT - 50.0 && cueScr.x < TABLE_RIGHT + 50.0 &&
                           cueScr.y > TABLE_TOP - 50.0 && cueScr.y < TABLE_BOTTOM + 50.0;
        startPos = cueOnScreen ? cueScr : targetPos;

        NativeTouchesBegin(5, startPos.x, startPos.y);
        placePhase = 1;
        dragStartTime = nowSec();
        return;
    }

    // Phase 1: move in steps toward the target (~0.5s), then release.
    double t = std::clamp((nowSec() - dragStartTime) / 0.5, 0.0, 1.0);
    double ease = t * t * (3.0 - 2.0 * t); // smoothstep
    ImVec2 cur(startPos.x + (targetPos.x - startPos.x) * ease,
               startPos.y + (targetPos.y - startPos.y) * ease);
    NativeTouchesMove(5, cur.x, cur.y);

    if (t >= 1.0) {
        NativeTouchesEnd(5, targetPos.x, targetPos.y);
        placePhase = 0;
    }
}

#pragma once

#include <vector>
#include <algorithm>
#include <map>
#include <string>
#include <chrono>
#include <random>
#include "include/logger.h"
#include "Vector/Vectors.h"
#include "imgui/imgui.h"
#include "GameConstants.h"
#include "game/Ball.h"

// Forward Declarations
struct GameManager;
struct Prediction;
struct ImDrawList;
struct PowerSlider;
struct ButtonClicker;

// --- Global External References ---
extern GameManager sharedGameManager;
extern Prediction* gPrediction;
extern float g_toggleRotAngle;
extern std::map<std::string, bool> persistent_bool;
extern std::map<std::string, float> persistent_float;
extern std::map<std::string, int> persistent_int;
extern uintptr_t libmain;
extern int Width, Height; // Weight was likely Width in previous context
extern ImVec2 GetPocketScreenPos(int pocketIndex);

// Power conversion helper
extern double ShotPowerToPower(double power);

class AutoPlay {
public:
    // ==================== ENUMS ====================
    enum AutoMode { MODE_OFF = 0, MODE_AUTO_PLAY, MODE_AUTO_AIM };
    enum NineBallStrategy { NINEBALL_NORMAL = 0, NINEBALL_BEST_SHOT, NINEBALL_SNIPE_9 };
    enum CleanTableMode { CLEAN_OFF = 0, CLEAN_ALL_BALLS, CLEAN_YOUR_BALLS };
    enum SpinPreset { SPIN_TOP = 0, SPIN_BOTTOM, SPIN_LEFT, SPIN_RIGHT, SPIN_CENTER };
    enum State { IDLE, SCANNING, NOMINATING, NOMINATING_HUMAN, WAITING_FOR_USER_POCKET, EXECUTING };
    enum HumanState { 
        HUM_IDLE, 
        HUM_THINKING, 
        HUM_OVERSHOOTING, 
        HUM_CORRECTING, 
        HUM_HOLDING,
        HUM_STABILIZING, 
        HUM_PULLING, 
        HUM_DELAY_BEFORE_SHOT 
    };

    enum PlayStyle { STYLE_HUMAN = 0, STYLE_WILD = 1 };
    static inline PlayStyle playStyle = STYLE_HUMAN;
    static inline AutoMode currentMode = MODE_OFF;
    static inline NineBallStrategy nineBallStrategy = NINEBALL_SNIPE_9;
    static inline CleanTableMode cleanTableMode = CLEAN_ALL_BALLS;
    static inline SpinPreset spinPreset = SPIN_CENTER;
    static inline float powerMin = 0.0f;
    static inline float powerMax = 666.0f;
    static inline bool bAutoSpin = false;
    static inline bool forcedShotSpinActive = false;
    static inline Vec2d forcedShotSpin = {0.0, 0.0};
    static inline bool bShowAutoPlayLines = false;
    static inline bool bAutoPocket = true;

    // ==================== INTERNAL STATE ====================
    static inline State state = IDLE;
    static inline bool bAutoPlaying = false;
    static inline bool g_autoPlayCalculating = false;
    static inline double sweepAngle = 0.0; // Exported for Radar effect
    static inline bool bAutoPlaySwitch = false; // Menu Switch
    static inline bool bAutoAimSwitch = false;  // Menu Switch
    static inline bool bCueBallIsMovingOrDragging = false;

    // ==================== WILD SCANNER STATE ====================
    // The old Beast scanner swept 360 degrees blind: ~1257 rays at scanner level
    // 0, one ray per frame in its SLOW path, which is what made a turn take tens
    // of seconds. The replacement solves the ghost-ball geometry per (ball,
    // pocket) pair instead, so `raw` holds a few dozen real candidates rather
    // than a thousand rays, and the eval loop is bounded by wall-clock time per
    // frame rather than by a fixed candidate count.
    // How a candidate was derived. Selection ranks on this BEFORE falling back
    // to luck, so a blind sweep ray can never outrank a computed ghost-ball
    // solve that pots the SAME number of balls - while a sweep ray that pots
    // strictly more balls still wins, which is where the multi-ball scatters
    // come from. Getting this backwards is exactly what made the previous
    // attempt play worse: acceptance was widened before the selector learned to
    // prefer computed geometry.
    enum WildFamily { FAM_SWEEP = 0, FAM_BANK = 1, FAM_KISS = 2, FAM_COMBO = 3, FAM_DIRECT = 4 };

    // How good the outcome was, coarsely. The scanner used to throw away every
    // sim that potted nothing of ours, so a turn with no pot on the table left
    // it with an empty result set and no move to make - it went IDLE and handed
    // the turn over. Keeping the merely-LEGAL sims means there is always
    // something to fire, which is the whole point: passing the turn is never an
    // acceptable outcome, a legal miss is.
    enum WildTier { TIER_NONE = 0, TIER_LEGAL = 1, TIER_POT = 2 };

    struct WildRaw {
        Candidate c;
        int family = FAM_SWEEP;
        // Part of the targeted contact-window fan - the rays that actually
        // produce multi-ball scatters. Tracked so the scan can refuse to settle
        // for a one-ball pot before it has simulated them.
        bool scatter = false;
    };
    struct WildEval {
        Candidate c;
        int  tot    = 0;    // balls potted by this shot, all colours
        int  own    = 0;    // of those, the ones that are legally ours
        bool win    = false;// this shot legally finishes the rack
        int  family = FAM_SWEEP;
        int  tier   = TIER_NONE;
        double leave = 0.0; // cue-ball rest quality, lower is better
    };
    struct WildScanState {
        std::vector<WildRaw>  raw;    // generated once per cue-ball position
        std::vector<WildEval> evals;  // survivors, accumulated across frames
        size_t  evalIndex = 0;         // how far through `raw` we have got
        // One past the last scatter-fan candidate. A one-ball pot is not
        // committed to until evalIndex reaches this, because everything capable
        // of potting three sits inside that span.
        size_t  scatterEnd = 0;
        // Which sweep of the table we are on. 0 is the normal list, 1 is the
        // deep fan built when the normal list produced no pot at all.
        int     pass = 0;
        // How many local refinement grids have been run around the best shots
        // found so far. Refinement is what turns a two-ball shot into a
        // four-ball one, and it is bounded so a crowded table cannot loop.
        int     refineRounds = 0;
        // Best `own` held when the CURRENT refinement round started. A round
        // that ends without beating it has found nothing the previous grid
        // missed, and another grid over the same neighbourhood will not either -
        // so that is where refinement stops. This replaces a fixed round cap as
        // the real brake: the cap used to fire while rounds were still climbing
        // 1 -> 2 -> 3, and lifting it alone would have burned the clock on
        // tables that had already given everything they had.
        int     refineBaseOwn = 0;
        // How many refinement rounds IN A ROW have failed to beat their base.
        // One barren round no longer means the well is dry, because each round
        // now searches a different shape of neighbourhood (coarse-local, then
        // razor-fine, then wide, then a power sweep). A fine grid finding
        // nothing says nothing about whether a wide one will. Measured over two
        // games on the single-shape grid, round 1 improved the shot in only 3 of
        // 18 turns and the other 15 stopped on NO_PROGRESS after re-searching
        // ground the first pass had already covered.
        int     refineBarren = 0;
        Point2D scanCuePos = {-1000, -1000};
        bool    isInitiated = false;
        double  scanStart = 0.0;       // nowSec() when this scan began
        // nowSec() when the CURRENT pass began. The per-quality time caps are
        // measured from here, not from scanStart, so a refinement pass starts
        // with a full budget instead of inheriting an already-expired one and
        // being skipped without simulating a single candidate.
        double  passStart = 0.0;
        // Best shot found so far that is LEGAL but pots nothing of ours. Held
        // apart from `evals` so it can never win while a real pot exists, and
        // used as the guaranteed move when no pot does.
        WildEval fallback;
        bool     haveFallback = false;
        // Spin frozen for the whole scan. Every rung, the final validation and
        // the fired shot all read this one value, so what was proven is what
        // leaves the cue.
        Vec2d    spin = {0.0, 0.0};
        // Set when a scan genuinely could not fire (no cue ball / empty table).
        // A short time cooldown, NOT the old position-keyed block, which
        // permanently barred re-scanning at that cue position for the rest of
        // the turn.
        double   retryAfter = 0.0;
        // --- TEMPORARY DIAGNOSTICS ---
        // Answering "why is it potting one ball instead of four?" needs numbers,
        // not guesses: whether the scan is stopping on the clock or genuinely
        // running out of candidates, and how many simulations it actually got
        // through on THIS phone. Costs one increment per sim and a handful of
        // log lines per turn. Remove once the tuning question is settled.
        int      simCount = 0;   // probeShot calls in this scan
        int      frames   = 0;   // frames this scan has spanned
    };

    // Corrected initializer to match global Candidate struct (6 fields)
    static inline Candidate g_CurrentCandidate = {-1, 0, 0, -1, 0, 0};
    static inline Point2D lastFailedCuePos = {-1000, -1000};
    static inline Point2D lastSetCuePos = {-1000, -1000};

    // Minimal validity gate for PowerSlider: a candidate index of -1 is the
    // convention this file already uses everywhere else to mean "no active
    // shot" (see End()/Cancel() callers). This was called from
    // mod/PowerSlider.h but never defined anywhere — undefined symbol at
    // link time. If you want richer validation (e.g. re-check the ball is
    // still on the table / pocket still reachable) before releasing the
    // slider, extend this.
    static inline bool IsShotValid() {
        return g_CurrentCandidate.idx != -1;
    }

    
    static inline double pendingShotAngle = 0, pendingShotPower = 0;
    static inline int nominationFrameCounter = 0;
    static inline int frameCounter = 0;

    // Human mode specific
    static inline HumanState humanState = HUM_IDLE;
    static inline double stateStartTime = 0;
    static inline double targetAngle = 0, startAngle = 0, currentOvershootTarget = 0;
    static inline double overshootOffset = 0;
    static inline double aimDuration = 0.8, pullDuration = 0.6;
    static inline double stabilizeDuration = 0.3;
    static inline double startPower = 0, targetPower = 0;
    static inline bool humanShotLocked = false;
    static inline bool g_PredictionLocked = false; // Locks lines on final shot angle
    static inline bool humanNeedsNomination = false;
    static inline int humanNominationPocket = -1;

    // ==================== CORE FUNCTIONS ====================
    static void applyAutoSpin();
    static void ClearState();
    static std::vector<Point2D> getPockets();
    static void setAimAngle(double angle);
    static void setPower(double power);
    static std::vector<double> BuildPowerGrid(double idealPower);
    static void takeShot(double angle, double power, bool preserveStartAngle = false);
    static void triggerShot();
    static void Shoot(double angle, double power = 0.f);
    static void ExecuteHumanAI(double angle, double power);
    static void ExecuteBeastAI(double angle, double power);
    static void ScanWild();
    static bool IsAnimationActive();
    static void Update();
    static void PlaceCueBall(); // Ball-in-hand: drags the cue to a good spot (self-throttling; call every frame)

    // Helpers
    static double nowSec() {
        auto now = std::chrono::steady_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration<double>(duration).count();
    }
    static double getCurrentPower();
    static bool AreBallsMoving();
};

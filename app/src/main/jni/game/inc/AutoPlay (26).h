#pragma once

#include <vector>
#include <algorithm>
#include <map>
#include <string>
#include <chrono>
#include <random>

// ==================== تعريف Candidate من GameConstants.h ====================
// ملاحظة: Candidate معرف في GameConstants.h، لا نعيد تعريفه هنا!
// ============================================================================

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
extern int Width, Height;
extern ImVec2 GetPocketScreenPos(int pocketIndex);

// Power conversion helper
extern double ShotPowerToPower(double power);

class AutoPlay {
public:
    // ==================== ENUMS ====================
    enum AutoMode { MODE_OFF = 0, MODE_AUTO_PLAY, MODE_AUTO_AIM };
    enum NineBallStrategy { NINEBALL_NORMAL = 0, NINEBALL_BEST_SHOT, NINEBALL_SNIPE_9 };
    enum AutomationSpeed { SPEED_FAST = 0, SPEED_HUMAN };
    enum CleanTableMode { CLEAN_OFF = 0, CLEAN_ALL_BALLS, CLEAN_YOUR_BALLS };
    enum SpinPreset { SPIN_TOP = 0, SPIN_BOTTOM, SPIN_LEFT, SPIN_RIGHT, SPIN_CENTER };
    enum State { IDLE, SCANNING, NOMINATING, NOMINATING_HUMAN, WAITING_FOR_USER_POCKET, EXECUTING };
    enum ScanMode { FAST, SLOW };
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

    enum PlayStyle { STYLE_NATURAL = 0, STYLE_INSTANT };
    static inline PlayStyle playStyle = STYLE_NATURAL;
    static inline AutoMode currentMode = MODE_OFF;
    static inline NineBallStrategy nineBallStrategy = NINEBALL_SNIPE_9;
    static inline AutomationSpeed automationSpeed = SPEED_FAST;
    static inline CleanTableMode cleanTableMode = CLEAN_OFF;
    static inline SpinPreset spinPreset = SPIN_CENTER;
    static inline float powerMin = 100.0f;
    static inline float powerMax = 666.0f;
    static inline bool bAutoSpin = false;
    static inline bool bShowAutoPlayLines = false;
    static inline bool bAutoPocket = true;
    static inline bool bCushionShot = true;

    // ==================== INTERNAL STATE ====================
    static inline State state = IDLE;
    static inline ScanMode scan = FAST;
    static inline bool bAutoPlaying = false;
    static inline bool g_autoPlayCalculating = false;
    static inline double sweepAngle = 0.0;
    static inline bool bAutoPlaySwitch = false;
    static inline bool bAutoAimSwitch = false;
    static inline bool bCueBallIsMovingOrDragging = false;

    struct FastScanState {
        std::vector<Candidate> raw;
        struct Eval { Candidate c; int tot, own; bool p9; };
        std::vector<Eval> evals;
        int evalIndex = 0;
        int prepPhase = 0;
        Point2D scanCuePos = {-1000, -1000};
        bool isInitiated = false;
    };

    // ✅ التهيئة الصحيحة حسب تعريف Candidate في GameConstants.h (5 عناصر)
    static inline Candidate g_CurrentCandidate = {-1, 0.0, 0.0, -1, 0.0};
    static inline Point2D lastFailedCuePos = {-1000, -1000};
    static inline Point2D lastSetCuePos = {-1000, -1000};
    
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
    static inline bool g_PredictionLocked = false;
    static inline bool humanNeedsNomination = false;
    static inline int humanNominationPocket = -1;

    // ==================== CORE FUNCTIONS ====================
    static void applyAutoSpin();
    static void ClearState();
    static std::vector<Point2D> getPockets();
    static void setAimAngle(double angle);
    static void setPower(double power);
    static void takeShot(double angle, double power, bool preserveStartAngle = false);
    static void triggerShot();
    static void Shoot(double angle, double power = 0.f);
    static void ScanSlow(double angleStep = 0.02f);
    static void ScanFast(double angleStep = 0.1f);
    static bool IsAnimationActive();
    static void Update();

    // Helpers
    static double nowSec() {
        auto now = std::chrono::steady_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration<double>(duration).count();
    }
    static double getCurrentPower();
    static bool AreBallsMoving();
};

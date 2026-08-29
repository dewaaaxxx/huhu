#pragma once

#include <vector>

#include "GameConstants.h"
#include "Vector/Vectors.h"
#include "game/Ball.h"

// =====================================================================
// HumanScan — Human-style Auto Play AI (8-Ball, Direct Shots Only)
//
// Completely separate from the wild scanner (AutoPlay::ScanWild). Pipeline:
//   AnalyzeTable -> ChooseGroup -> CreateCandidates -> EasyFilter
//   -> RankCandidates -> SimulateCandidate (Phase A: pot pool,
//   Phase R: multi-shot run-out planner to the 8-ball, Phase B:
//   spin/power refinement for position play)
//   -> CommitShot -> ExecuteShot
// (BreakPhase handles the opening break separately.)
//
// Performance contract (v1.8):
//   * Normal turn: bounded candidate validation plus a bounded run/position
//     search. The entry point is called once per settled turn, not per frame.
//   * Break:       up to 492 bounded simulations (41 angles x 3 spins x 4
//     power levels), with scratch rejection before committing the shot.
// =====================================================================

namespace HumanScan {

    struct BallEntry {
        int index;
        Point2D pos;
        Ball::Classification cls;
    };

    // Live snapshot of the table, read once per decision cycle.
    struct TableInfo {
        bool isBreak = false;
        bool cueOnTable = false;
        int  solidsLeft = 0;
        int  stripesLeft = 0;
        int  solidsPotted = 0;
        int  stripesPotted = 0;
        bool only8BallLeft = false;
        bool onLastGroupBall = false;
        Ball::Classification gameGroup = Ball::Classification::ANY;
        Ball::Classification myGroup = Ball::Classification::ANY;
        int  nominatedPocket = 6; // < 6 => a pocket is nominated
        int  nominationMode = 0;   // 0=none, 1=8ball, 2=all
        bool eightBallMustBank = false;
        Point2D cuePos;
        Point2D pockets[TABLE_POCKETS_COUNT];
        std::vector<BallEntry> allBalls; // cached once per decision cycle
    };

    // A single geometric candidate (no simulation performed yet).
    struct HumanCandidate {
        int   idx = -1;
        int   pocket = -1;
        double angle = 0.0;
        double score = 1e18;      // geometric rank score (lower = easier)
        double dist = 0.0;        // target ball -> pocket distance
        double power = 0.0;
        double cutDot = 1.0;      // 1.0 = perfectly straight
        double cueDist = 0.0;     // cue -> ghost ball distance
        Point2D ballPos;
        Point2D ghost;
        Point2D predictedCueRest;
        Vec2d  spin;              // raw english (mEnglish) chosen for this shot
        int    spinPreset = -1;   // AutoPlay::SpinPreset (or -1 = don't touch)
        const char* spinTag = "-";// short label for logs
        double shapeScore = 1e18; // ease of the NEXT shot from the cue resting spot (lower = better; 0 = none needed)
        int    followTarget = -1; // ball planned as the next shot (index)
        bool   simulated = false;
        bool   valid = false;
        bool   potted = false;
        bool   hasFollowUp = false;
        double finalScore = 1e18;
    };

    struct CommittedShot {
        bool   locked = false;
        int    idx = -1;
        int    pocket = -1;
        double angle = 0.0;
        double power = 0.0;
        Vec2d  spin;
        int    spinPreset = -1;
        const char* spinTag = "-";
        int    followTarget = -1;
        double shapeScore = 1e18;
        bool   hasFollowUp = false;
    };

    enum class SimStatus { INVALID, SAFE, POTTED };

    // ═══════════════════════════════════════════════════════════════════════════
    // 🐯 ENHANCED AI PARAMETERS (LEGENDARY IMPROVEMENTS)
    // Better decision making, more realistic play
    // ═══════════════════════════════════════════════════════════════════════════
    namespace ImprovedParams {
        // ════════════════════════════════════════════════════════════════════
        // 🐯 SUPER LEGENDARY AI - UNBEATABLE HUMAN PLAYER
        // Enhanced cut angles, aggressive play, expert positioning
        // ════════════════════════════════════════════════════════════════════
        
        // Super-aggressive cut angle tolerance
        constexpr double CUT_MIN = 0.28;            // Very forgiving cuts
        constexpr double CUT_OPTIMAL = 0.95;        // Prefer straight shots
        constexpr double MAX_CUE_DIST = 320.0;      // Maximum reach (+12%)
        constexpr double MAX_POCKET_DIST = 340.0;   // More pockets reachable (+13%)
        
        // Extreme candidate evaluation
        constexpr int MAX_CANDIDATES = 35;          // +75% more options!
        constexpr int SIM_BUDGET = 950;             // +58% simulation power
        constexpr int MAX_BANK_CANDIDATES = 22;     // Enhanced bank shots
        constexpr int MAX_COMBO_CANDIDATES = 18;    // Better combos
        
        // Master-level position play
        constexpr double SHAPE_WEIGHT = 1.65;       // Aggressive positioning (+38%)
        constexpr double SHAPE_WEIGHT_LAST = 2.8;   // Perfect 8-ball placement (+40%)
        constexpr double LOOKAHEAD_MAX_SCORE = 280.0; // Better lookahead
        
        // Ultimate 8-Ball handling
        constexpr double EIGHTBALL_MIN_POWER = 260.0;  // Softer, precise shots
        constexpr double EIGHTBALL_VALID_FLOOR = 200.0; // Wide power range
        constexpr double POST_BREAK_MIN_POWER = 150.0;  // Aggressive break
        constexpr double EIGHTBALL_BANK_MIN_POWER = 240.0;
        
        // Expert run-out planning
        constexpr int RUN_BEAM = 4;                 // +100% branch selection!
        constexpr int RUN_BALLS = 4;                // +100% ball consideration!
        constexpr int RUN_BUDGET = 250;             // +79% planning power!
        constexpr double RUN_MAX_SHOT_SCORE = 380.0; // More sophisticated
        
        // Instant re-analysis and adaptation
        constexpr double RETRY_WINDOW = 1.2;        // Ultra-fast decisions (-40%)
        constexpr double CUE_MOVE_EPS_SQ = 0.0008;  // Hyper-sensitive cue tracking
        constexpr double CLUSTER_ESCAPE_BONUS = 1.8; // Better escape shots
        
        // Supreme spin variants for total control
        constexpr int SPIN_VARIANTS_COUNT = 32;     // +33% spin options!
        constexpr double SPIN_PRECISION = 1.2;      // Tighter control
        
        // Advanced AI decision system
        constexpr int TACTICAL_DEPTH = 3;           // 3-shot lookahead
        constexpr double RISK_ASSESSMENT = 0.85;    // Calculated aggression
        constexpr double SAFETY_MARGIN = 1.1;       // Expert safety plays
        constexpr int MAX_REFINE = 9;               // Maximum optimization
    }

    struct Context {
        TableInfo table;
        std::vector<HumanCandidate> candidates;
        HumanCandidate best;
        HumanCandidate safeShot;
        CommittedShot shot;
        bool   turnAnalyzed = false;
        bool   awaitingSettlement = false;
        bool   breakDone = false;
        bool   breakShotActive = false;
        bool   postBreakShot = false;
        std::vector<BallEntry> breakBeforeBalls;
        double lastFailTime = -1000.0;
        Point2D lastDecisionCuePos;
        int    simsUsed = 0;
        int    potCands = 0;      // diagnostics: sims that potted a legal ball
        int    safeCands = 0;     // diagnostics: legal contact without a pot
        int    invalidCands = 0;  // diagnostics: fouls / scratches / bad contact
    };

    // --- Stage functions (implemented in HumanScan.impl.h) ---
    bool AnalyzeTable();                    // live read, zero sim
    void ChooseGroup();                     // zero sim
    void CreateCandidates();                // direct shots only, zero sim
    void CreateBankCandidates();             // 1-rail bank candidates for rule-required 8-ball shots
    void AppendIndirectCandidates();        // combos + general 1-rail banks, zero sim
    void EasyFilter();                      // reject unsuitable candidates, zero sim
    void RankCandidates();                  // keep top MAX_CANDIDATES, zero sim
    SimStatus SimulateCandidate();          // up to 4 full sims, sets ctx.best/safeShot
    void LookAheadOneShot(HumanCandidate& c); // geometric only, zero extra sim
    void CommitShot(const HumanCandidate& c);
    void ExecuteShot();
    bool BreakPhase();                      // ~41-sim radial sweep at full power
    bool RunIfReady();                      // guarded entry from AutoPlay::Update
    void ResetTurn();

    inline Context ctx;
}
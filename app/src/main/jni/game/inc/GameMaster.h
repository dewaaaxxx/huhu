#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include "GameConstants.h"
#include "Vector/Vectors.h"
#include "game/Ball.h"
#include "game/inc/HumanScan.h"

// =====================================================================
// GameMaster — Human-style shot planner (8-Ball)
//
// After the break, plans the easiest route through the group balls:
//   1. SelectGroup  — pick the easier group
//   2. BuildPlan     — order balls by ease, plan current + next position
//   3. ExecuteStep   — play with simple, natural spin
//
// Human principles:
//   - Prefer center-ball hits (no extreme english)
//   - Keep cue ball in open areas away from cushions and clusters
//   - Take the easiest available shot; don't force position heroics
//   - Simple spin grid: 14 variants (no extreme combos)
// =====================================================================

namespace GameMaster {

    struct PlannedStep {
        int   ballIdx = -1;
        int   pocket = -1;
        double angle = 0.0;
        double power = 0.0;
        double spinX = 0.0;
        double spinY = 0.0;
        int   spinPreset = -1;
        const char* spinTag = "-";
        double cueDist = 0.0;
        double potDist = 0.0;
        double shotScore = 1e18;   // geometric difficulty
        double posScore = 1e18;    // position quality for next ball
    };

    struct GamePlan {
        std::vector<int> ballOrder;  // ordered target ball indices
        int   currentIdx = 0;
        bool  initialized = false;
        bool  complete = false;
        int   replanCount = 0;
        int   simsUsed = 0;
        int   activeTarget = -1;
        int   activePocket = -1;
        uint32_t activeBeforeMask = 0;
        Point2D predictedCueRest;
        bool   activeStep = false;
    };

    struct SpinVariantSimple {
        double x, y;        // raw english
        double pMult;       // power multiplier
        int    preset;      // AutoPlay::SpinPreset
        const char* tag;
    };

    // Natural human spin variants — the planner also searches a wider power
    // grid for the center-ball variant.
    constexpr int SPIN_SIMPLE_COUNT = 20;
    inline const SpinVariantSimple SPIN_SIMPLE[SPIN_SIMPLE_COUNT] = {
        { 0.00,  0.00, 1.00, AutoPlay::SPIN_CENTER, "c"  },  // center
        { 0.00,  0.55, 1.00, AutoPlay::SPIN_TOP,    "t"  },  // soft follow
        { 0.00, -0.55, 1.00, AutoPlay::SPIN_BOTTOM, "b"  },  // soft draw
        { 0.40,  0.00, 1.00, AutoPlay::SPIN_RIGHT,  "r"  },  // soft right
        {-0.40,  0.00, 1.00, AutoPlay::SPIN_LEFT,   "l"  },  // soft left
        { 0.35,  0.45, 1.00, AutoPlay::SPIN_RIGHT,  "rt" },  // follow+right
        {-0.35,  0.45, 1.00, AutoPlay::SPIN_LEFT,   "lt" },  // follow+left
        { 0.35, -0.45, 1.00, AutoPlay::SPIN_RIGHT,  "rb" },  // draw+right
        {-0.35, -0.45, 1.00, AutoPlay::SPIN_LEFT,   "lb" },  // draw+left
        { 0.00,  0.00, 0.90, AutoPlay::SPIN_CENTER, "s"  },  // soft center
        { 0.00,  0.00, 1.10, AutoPlay::SPIN_CENTER, "h"  },  // firm center
        { 0.00,  0.80, 1.00, AutoPlay::SPIN_TOP,    "T"  },  // full follow
        { 0.00, -0.80, 1.00, AutoPlay::SPIN_BOTTOM, "B"  },  // full draw
        { 0.65,  0.00, 1.00, AutoPlay::SPIN_RIGHT,  "R"  },  // full right
        { 0.25,  0.00, 1.00, AutoPlay::SPIN_RIGHT,  "rS" },
        {-0.25,  0.00, 1.00, AutoPlay::SPIN_LEFT,   "lS" },
        { 0.00,  0.35, 1.00, AutoPlay::SPIN_TOP,    "tS" },
        { 0.00, -0.35, 1.00, AutoPlay::SPIN_BOTTOM, "bS" },
        { 0.75,  0.00, 1.00, AutoPlay::SPIN_RIGHT,  "rH" },
        {-0.75,  0.00, 1.00, AutoPlay::SPIN_LEFT,   "lH" },
    };

    // --- Phase functions ---
    bool SelectGroup(const HumanScan::TableInfo& table);
    GamePlan BuildPlan(const HumanScan::TableInfo& table);
    bool TryPlayCurrentBall(const HumanScan::TableInfo& table, bool isLastGroup);
    bool RunMasterPlan();

    inline GamePlan g_plan;

    inline void ResetPlan() {
        g_plan = GamePlan{};
    }

} // namespace GameMaster

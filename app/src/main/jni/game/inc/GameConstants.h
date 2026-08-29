#pragma once

// BALL PROPERTIES

inline constexpr int MAX_BALLS_COUNT = 16;
inline constexpr double BALL_RADIUS = 3.800475;
inline constexpr double BALL_RADIUS_SQUARE = BALL_RADIUS * BALL_RADIUS;



// TABLE SIZE CONSTANTS

inline constexpr double TABLE_WIDTH = 254.0;
inline constexpr double TABLE_HEIGHT = 127.0;

inline constexpr double TABLE_HALF_WIDTH = TABLE_WIDTH / 2.0;
inline constexpr double TABLE_HALF_HEIGHT = TABLE_HEIGHT / 2.0;

inline constexpr double TABLE_BOUND_LEFT = -TABLE_HALF_WIDTH + BALL_RADIUS;
inline constexpr double TABLE_BOUND_TOP = -TABLE_HALF_HEIGHT + BALL_RADIUS;
inline constexpr double TABLE_BOUND_RIGHT = TABLE_HALF_WIDTH - BALL_RADIUS;
inline constexpr double TABLE_BOUND_BOTTOM = TABLE_HALF_HEIGHT - BALL_RADIUS;



// OTHER TABLE PROPERTIES

inline constexpr int TABLE_POCKETS_COUNT = 6;
inline constexpr int TABLE_SHAPE_SIZE = 46;

inline constexpr double POCKET_RADIUS = 8.0;
inline constexpr double POCKET_RADIUS_SQUARE = POCKET_RADIUS * POCKET_RADIUS;
// The game can drop a ball before its center reaches the exact ball-radius
// circle around the pocket center. Keep this separate from the outer jaw
// attraction radius so pocket entry can be calibrated independently.
inline constexpr double POCKET_DROP_RADIUS = 7.0;
inline constexpr double POCKET_DROP_RADIUS_SQUARE = POCKET_DROP_RADIUS * POCKET_DROP_RADIUS;

inline constexpr int MAX_SHOT_RESULT_SIZE = 50000;



// OTHER GAME PROPERTIES

inline constexpr double TIME_PER_TICK = 0.005;
inline constexpr double MIN_TIME = 1E-11;



// ANGLE CONSTANTS

inline constexpr double PI = 3.14159265358979;
inline constexpr double PI_0_5 = PI / 2.0;
inline constexpr double PI_1_5 = PI * 1.5;

inline constexpr double MAX_ANGLE_RADIANS = 360.0 / (180.0 / PI);
inline constexpr double MIN_ANGLE_STEP_RADIANS = 0.0174;

struct Candidate {
    int idx;
    double angle;
    double score;
    int pocketIndex;      // >= 0: this shot aims at THAT pocket and the sim must
                          // confirm it. -1: no pocket was aimed at (blind sweep /
                          // break probe) - any legal pocket counts.
    double power;
    double dist; // Added for distance-based scoring
    // Spin the scan proved this shot with. The scan used to write spin straight
    // into the live game mid-scan and then let applyAutoSpin() overwrite it at
    // fire time, so the shot was simulated with one spin and fired with another.
    // Carrying it on the candidate keeps selection, validation and execution on
    // one number - the same fix already applied to power (QuantizeShotPower).
    double spinX = 0.0;
    double spinY = 0.0;
    bool   useSpin = false;
    bool operator<(const Candidate& other) const {
        return score < other.score;
    }
};

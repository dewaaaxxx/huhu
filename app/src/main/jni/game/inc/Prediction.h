#pragma once

#include "NumberUtils.h"
#include <array>
#include <chrono>
#include <cstring>

#include "GameConstants.h"
#include <Vector/Vectors.h>
#include <vector>

#include "game/Ball.h"

#include "game/inc/AutoPlay.h"
#include <imgui/inc/persistence.h>

#include "game/GameManager.h"

static Vec4d table_bounds;
static bool fastCalc = true;

// Diagnostic: when true, determineBallTableCollision logs ONLY suspicious rail
// hits — those whose impact point lies inside a pocket's suction zone, which is
// where the corner-pocket back-wall tunnelling bug lived. Off by default: it
// costs a per-segment pocket-distance test and must not affect gameplay feel.
static bool logSuspiciousRailHits = false;

// Diagnostic: per-collision tracing of the prediction sim (RUN header, every
// ball-ball resolution, every rail response, and the predicted end state). ON
// for the live comparison session. Every call site is additionally gated on the
// run being the HONEST one — !isAuto directly, or !fastCalc inside the
// collision handlers, which is the same test since determineShotResult assigns
// fastCalc = isAuto. Without that gate an AI scan (~869 sims Beast / ~493
// Human per turn) would bury the one shot the player actually takes under
// candidates that never happen.
// predsimWrite is buffered now (flushes at RUN boundaries / 8 KB), so this no
// longer stalls the render thread the way per-line fopen did.
// OFF by default: the diff session it was opened for is over, and left on it
// keeps appending to predsim.log (already 4 MB) from the render thread on every
// honest sim. Flip to true to re-open an engine-vs-prediction comparison.
static bool logPredSimCollisions = false;

// Defined in mod/live_log.h (same TU, included later): writes to logcat and
// appends to <extDir>/engine_dump/predsim.log, so prediction-sim events
// survive even when the logcat ring buffer evicts them.
static void predsimWrite(const char *fmt, ...);
static void predsimFlush();
static void predsimReset();

// Predicted-vs-real end state. The drawn line IS the prediction's ball path, so
// "the line is right but the balls settle a bit off it" is exactly a mismatch
// between the last predicted rest position and the engine's real one. Snapping
// both and subtracting turns that complaint into a per-ball number, which says
// which ball and roughly which stage drifted instead of guessing between
// ball-ball and rail.
struct PredEndSnapshot {
    unsigned long long run = 0;
    int count = 0;
    int rails = 0;
    double angle = 0.0, power = 0.0;
    bool onTable[MAX_BALLS_COUNT] = {};
    Point2D pos[MAX_BALLS_COUNT] = {};
};
static PredEndSnapshot g_predEndLive;   // refreshed by every honest sim
static PredEndSnapshot g_predEndArmed;  // frozen the frame the real balls start moving
static bool g_predEndArmedValid = false;
static void capturePredVsRealEnd();

struct Prediction {
    static bool pocketStatus[TABLE_POCKETS_COUNT];

    Prediction() = default;
    ~Prediction() = default;

    static bool forceFullSimulation;

    // forceFullSimulation was doing two unrelated jobs: bypass the caching
    // early-outs (genuinely needed by every AI sim - see the fps throttle and the
    // scene-dedup return below) AND record the per-ball trajectory polyline
    // (needed only by the sims whose result gets DRAWN). Because the AI scans set
    // it on every sim, ~90 simulations per frame were each building point lists
    // for 16 balls - measured at ~37 points per ball on average and up to 352 -
    // and the scan then read nothing but onTable / pocketIndex /
    // predictedPosition / firstHitBall / railCollisions. The per-frame SceneData
    // save/restore in the scanners copies those vectors too, so the capacity
    // never even carries over. This flag lets the hot scan loops opt out of the
    // recording half while keeping the cache-bypass half.
    //
    // Suppressed sims still leave positions = {start, end}, so anything that
    // happens to draw them degrades to a straight line rather than crashing.
    // Never set it around a sim whose result is what the overlay renders.
    static bool suppressTrajectories;

    // RAII guard: guarantees the flag is cleared on every exit path, including
    // the `continue`s that the scan's rule gates are made of.
    struct ScanSimScope {
        ScanSimScope()  { Prediction::forceFullSimulation = true;  Prediction::suppressTrajectories = true;  }
        ~ScanSimScope() { Prediction::forceFullSimulation = false; Prediction::suppressTrajectories = false; }
    };

    bool determineShotResult(bool isAuto, double shotAngle = sharedGameManager.mVisualCue().getShotAngle(), double shotPower = sharedGameManager.mVisualCue().getShotPower(), Vec2d shotSpin = sharedGameManager.getShotSpin(), Candidate cand = {-1});
    bool mockPredictShotResult();

    struct Ball {
        int index; // ball index 0..15
        ::Ball::Classification classification;
        ::Ball::State state;
        bool originalOnTable;
        bool onTable;
        int pocketIndex = -1;

        Point2D velocity;
        Vec3d spin;
        Point2D initialPosition;
        Point2D predictedPosition;
        std::vector<Point2D> positions;

        // نقاط اصطدام الكرة بالطاولة فقط.
        std::vector<Point2D> tableImpactPoints;

        void findNextCollision(void *pData, double *time);
        void calcVelocity();
        void calcVelocityPostCollision(const double &angle);
        void move(const double &time);
        void applyPocketSuction(const double &time);
        bool isMovingOrSpinning() const;

        Ball() : index(0), classification(::Ball::Classification::ERR_CLASSIFICATION),
                 state(::Ball::State::ERR_STATE),
                 originalOnTable(false), onTable(false), velocity(Point2D()), spin(Vec3d()),
                 initialPosition(Point2D()), predictedPosition(Point2D()),
                 positions(std::vector<Point2D>()) {}

        ~Ball() = default;

        bool isBallBallCollision(double *smallestTime, Prediction::Ball &otherBall) const; // sub_1C29FA0 5.8.0
        bool willCollideWithTable(const double *smallestTime) const; // sub_1BF9ADC 5.8.0
        void determineBallTableCollision(void *pData, double *smallestTime); // sub_1BF9BD8 5.8.0
        bool isBallLineCollision(double *pTime_1, const Point2D &tableShapePointA, const Point2D &tableShapePointB) const; // sub_1C2A2C0 5.8.0
        bool isBallPointCollision(double *smallestTime, const Point2D &tableShapePoint) const; // sub_1C2A594 5.8.0
    };

    struct Collision {
        Collision() : valid(false), type(Type::POINT), angle(0.0), point{}, ballA(nullptr), ballB(nullptr), firstHitBall(nullptr) {}
        ~Collision() = default;

        enum Type : int {
            BALL,
            LINE,
            POINT
        };

        bool valid;
        Type type;
        double angle;
        Point2D point;
        Prediction::Ball *ballA;
        Prediction::Ball *ballB;
        Prediction::Ball *firstHitBall;
        int railCollisions;
    };

    struct SceneData {
        int ballsCount;
        Ball balls[MAX_BALLS_COUNT];

        Collision collision;
        bool shotState;

        SceneData() : ballsCount(0), balls{}, collision{}, shotState(false) {}
        ~SceneData() = default;
    } guiData;

    int shotResultSize = 0;
    static float shotResult[MAX_SHOT_RESULT_SIZE];

    bool firstHitIsTarget = false;
    Candidate m_candidate = {-1};

    void calculateShotResultSize();
    void initBalls();
    void initCueBall(double shotAngle, double shotPower, const Point2D& shotSpin);
    void mockInitBalls();
    void determineBallsPositions();
    void handleCollision();
    void handleBallBallCollision() const;
    void logRailResponse(const char *kind, Ball &ball, const double &angle) const;
    void determineShotState();
};

extern Prediction *gPrediction;
inline static Prediction prediction;
inline Prediction *gPrediction = &prediction;

inline bool Prediction::pocketStatus[TABLE_POCKETS_COUNT] = {};
inline float Prediction::shotResult[MAX_SHOT_RESULT_SIZE] = {};
inline bool Prediction::forceFullSimulation = false;
inline bool Prediction::suppressTrajectories = false;

static double prevAngle = 0.0;
static double prevPower = 0.0;
static bool prevIsAuto = false;
static Point2D prevSpin = {0.0, 0.0};
static Point2D prevCuePos = {0.0, 0.0};
static uint64_t prevSceneHash = 0;

// guiData is a SINGLE shared buffer: the drawn overlay line and every AI scan
// simulation both write it. Both early-outs below (fps throttle, scene dedup)
// return false on the assumption "guiData still holds my last honest result" —
// an assumption the AI violates, because AutoPlay::Update() runs its scan sims
// earlier in the same frame than the honest recompute in menu.h. On the BREAK
// that is worst-case: a scan is ~869 sims (Beast) / ~493 (Human) per turn, so
// on any frame where the honest recompute is throttled or deduped away, the
// line drawn is an arbitrary mid-scan candidate instead of the real shot.
// A generation counter makes the early-outs buffer-aware: bumped by every
// completed sim, latched by honest ones. gen != latched => somebody else wrote
// guiData since, so the buffer is dirty and must be recomputed regardless.
static uint64_t g_predSimGeneration = 0;
static uint64_t g_predHonestGeneration = ~0ULL;

// Pocket positions live in game memory, so getPockets() walked
// sharedGameManager -> mTable -> mTableProperties -> mPockets and rebuilt all six
// Point2D on EVERY call. findNextCollision and applyPocketSuction each call it
// once per ball per sub-step, so one simulation did that walk thousands of times
// for a value that physically cannot change while a shot is being simulated.
// The epoch is bumped once per simulation (initBalls), which keeps the live read
// - a scan on a differently-sized table still picks up the real pockets - while
// collapsing the per-sub-step repeats to a single read.
static uint32_t g_pocketEpoch = 1;
static uint32_t g_pocketCachedEpoch = 0;

static uint64_t HashSceneDouble(double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits ^ (bits >> 29) ^ (bits << 17);
}

bool Prediction::determineShotResult(bool isAuto, double shotAngle, double shotPower, Vec2d shotSpin, Candidate cand) { // returns isShouldReDraw
    extern std::map<std::string, float> persistent_float;
    static auto lastCalcTime = std::chrono::steady_clock::now();
    // Dirty when a scan sim (or any other caller) overwrote guiData since our
    // last honest run: the cached result the early-outs would reuse is gone.
    const bool bufferDirty = !isAuto && (g_predSimGeneration != g_predHonestGeneration);
    if (!isAuto && !forceFullSimulation && !bufferDirty) {
        float targetFps = persistent_float.count(O("fPredictionFps")) ? persistent_float[O("fPredictionFps")] : 30.0f;
        if (targetFps > 30.0f) targetFps = 30.0f;
        if (targetFps < 1.0f) targetFps = 1.0f;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCalcTime).count();
        double minInterval = 1000.0 / targetFps;
        if (elapsed < minInterval) {
            return false;
        }
        lastCalcTime = now;
    }

    Point2D cuePos = {0.0, 0.0};
    uint64_t sceneHash = 1469598103934665603ULL;
    if (sharedGameManager) {
        Table table = sharedGameManager.mTable;
        if (table) {
            auto& balls = table.mBalls();
            if (balls) {
                for (int i = 0; i < balls.Count; i++) {
                    auto pos = balls[i].position();
                    sceneHash ^= HashSceneDouble(pos.x) + (uint64_t)(i * 31 + 1);
                    sceneHash *= 1099511628211ULL;
                    sceneHash ^= HashSceneDouble(pos.y) + (uint64_t)(i * 37 + 3);
                    sceneHash *= 1099511628211ULL;
                    sceneHash ^= (uint64_t)balls[i].state() + (uint64_t)(i * 41 + 5);
                    sceneHash *= 1099511628211ULL;
                }
                if (balls.Count > 0) {
                    cuePos = balls[0].position();
                }
            }
        }
    }

    if (!forceFullSimulation && !bufferDirty && shotAngle == prevAngle && shotPower == prevPower && shotSpin == prevSpin && isAuto == prevIsAuto && cuePos == prevCuePos && sceneHash == prevSceneHash)
        return false;

    // DIAG: the sim is about to run. If the live cue ball position read near
    // the table origin (0,0), the drawn line will start from the wrong spot
    // (line disconnected from the ball -> "shot completely wrong").
    if (cuePos.x * cuePos.x + cuePos.y * cuePos.y < 1.0) {
        int sid = -1;
        if (sharedGameManager) { GameStateManager gsm = sharedGameManager.mStateManager; if (gsm) sid = gsm.getCurrentStateId(); }
        bool moving = false;
        if (sharedGameManager) { Table t = sharedGameManager.mTable; if (t) { auto& bs = t.mBalls(); if (bs) for (int i = 0; i < bs.Count; i++) { auto v = bs[i].velocity(); if (v.x*v.x + v.y*v.y > 0.000001) { moving = true; break; } } } }
        LOGI("[FireDbg] SIMRUN-ZERO stateId=%d cue=(%.2f,%.2f) angle=%.4f power=%.2f isAuto=%d moving=%d",
             sid, cuePos.x, cuePos.y, shotAngle, shotPower, isAuto ? 1 : 0, moving ? 1 : 0);
    }

    prevAngle = shotAngle;
    prevPower = shotPower;
    prevSpin  = shotSpin;
    prevIsAuto = isAuto;
    prevCuePos = cuePos;
    prevSceneHash = sceneHash;

    this->m_candidate = cand;
    fastCalc = isAuto;

    this->guiData.ballsCount = 0;
    this->initBalls();
    // initBalls already overwrote the shared buffer, so claim it even on this
    // bail-out path; otherwise the dirty check below would still believe the
    // previous honest result is intact.
    if (this->guiData.ballsCount <= 0) { ++g_predSimGeneration; return false; }
    this->initCueBall(shotAngle, shotPower, shotSpin);
    this->guiData.collision.firstHitBall = nullptr;
    this->firstHitIsTarget = false;
    
    for (bool &_pocketStatus : pocketStatus) _pocketStatus = false;
    this->guiData.collision.railCollisions = 0;

    static uint64_t runCounter = 0;
    ++runCounter;
    if (logPredSimCollisions && !isAuto) {
        predsimWrite("==== RUN#%llu start angle=%.4f power=%.2f spin(%.4f,%.4f) cue=(%.3f,%.3f) ====",
                     (unsigned long long)(runCounter), shotAngle, shotPower,
                     shotSpin.x, shotSpin.y, cuePos.x, cuePos.y);
        // Start state matters as much as the end state: a drift that is already
        // present at t=0 is a state-read bug, not a physics bug, and only the
        // pairing of START and PREDEND/REALEND can tell those two apart.
        for (int i = 0; i < this->guiData.ballsCount; i++) {
            Ball &b = this->guiData.balls[i];
            predsimWrite("[PREDSTART] B#%d pos(%.4f,%.4f) vel(%.4f,%.4f) spin(%.4f,%.4f,%.4f)",
                         i, b.predictedPosition.x, b.predictedPosition.y,
                         b.velocity.x, b.velocity.y, b.spin.x, b.spin.y, b.spin.z);
        }
    }
    this->determineBallsPositions();
    // if (dynamic_bool["isDrawShotStateEnabled", false]) this->determineShotState();

    for (int i = 0; i < this->guiData.ballsCount; i++) {
        Ball &ball = this->guiData.balls[i];
        if (ball.positions.back() != ball.predictedPosition) {
            ball.positions.push_back(ball.predictedPosition);
        }
    }

    // Predicted end state. Paired with REALEND (logged once the real balls
    // settle) this measures the drift directly: same ball index, same shot,
    // so the per-ball delta attributes the error to a specific ball rather
    // than to a guess about which stage is wrong.
    if (logPredSimCollisions && !isAuto) {
        predsimWrite("[PREDEND] RUN#%llu rails=%d", (unsigned long long)runCounter,
                     this->guiData.collision.railCollisions);
        for (int i = 0; i < this->guiData.ballsCount; i++) {
            Ball &ball = this->guiData.balls[i];
            predsimWrite("[PREDEND] B#%d onTable=%d pos(%.4f,%.4f) pts=%d",
                         i, ball.onTable ? 1 : 0,
                         ball.predictedPosition.x, ball.predictedPosition.y,
                         (int)ball.positions.size());
        }

        // Keep the same numbers in memory. The player fires some frames after
        // this run, so the comparison cannot happen here - menu.h freezes this
        // snapshot the moment the real balls start moving and diffs it against
        // the engine's own rest positions once they stop.
        g_predEndLive.run    = (unsigned long long)runCounter;
        g_predEndLive.count  = this->guiData.ballsCount;
        g_predEndLive.rails  = this->guiData.collision.railCollisions;
        g_predEndLive.angle  = shotAngle;
        g_predEndLive.power  = shotPower;
        for (int i = 0; i < this->guiData.ballsCount && i < MAX_BALLS_COUNT; i++) {
            g_predEndLive.onTable[i] = this->guiData.balls[i].onTable;
            g_predEndLive.pos[i]     = this->guiData.balls[i].predictedPosition;
        }
    }

    // Claim the buffer. An honest run latches the generation, so the early-outs
    // may reuse it; a scan run only bumps it, marking the buffer dirty for the
    // next honest call.
    ++g_predSimGeneration;
    if (!isAuto) g_predHonestGeneration = g_predSimGeneration;

    return true;
}

/* ============================================================================================== */

/* PREDICTION PRIVATE METHODS =================================================================== */

void Prediction::initBalls() {
    this->guiData.ballsCount = 0;
    // One simulation = one pocket epoch. Every getPockets() inside the sub-step
    // loops below now reuses a single live read instead of repeating the walk
    // into game memory per ball per sub-step.
    ++g_pocketEpoch;
    Table table = sharedGameManager.mTable;
    if (!table) return;
    
    auto& balls = table.mBalls();
    if (!balls) return;

    table_bounds = table.mTableCollisionBounds();

    // MemoryManager::Balls::initializeBallsList();
    if (balls.Count <= 0) return;
    if (balls.Count > MAX_BALLS_COUNT) {
        LOGI("[Prediction] clamping ball count %d to %d", balls.Count, MAX_BALLS_COUNT);
    }
    this->guiData.ballsCount = std::min((int)balls.Count, MAX_BALLS_COUNT);
    for (int i = 0; i < this->guiData.ballsCount; i++) {
        Ball &ball = this->guiData.balls[i];
        ball.index = i;
        ball.state = balls[i].state();
        ball.originalOnTable = balls[i].isOnTable();
        ball.onTable = ball.originalOnTable;
        ball.classification = balls[i].classification();
        ball.initialPosition = balls[i].position();
        ball.predictedPosition = ball.initialPosition;
        ball.velocity.nullify();
        ball.spin.nullify();
        ball.pocketIndex = -1; // CRITICAL: Reset pocketIndex every simulation - prevents stale pocket
                               // data from previous simulation contaminating current one.
                               // Without this, black ball might retain pocketIndex=3 from simulation N,
                               // even when simulation N+1 uses a different angle where ball goes elsewhere.
        if (!ball.positions.empty()) ball.positions.clear();
        if (!ball.tableImpactPoints.empty()) ball.tableImpactPoints.clear();
        ball.positions.reserve(20);
        ball.positions.push_back(ball.initialPosition);
    }
}

void Prediction::initCueBall(double shotAngle, double shotPower, const Point2D &shotSpin) {
    double angleCos = round(cos(shotAngle) * 10000.0) / 10000.0;
    double angleSin = round(sin(shotAngle) * 10000.0) / 10000.0;
    Ball &cueBall = this->guiData.balls[0];
    cueBall.velocity.x = shotPower * angleCos;
    cueBall.velocity.y = shotPower * angleSin;
    double spinFactor = shotPower / BALL_RADIUS;
    double v31 = -shotSpin.y * spinFactor;
    cueBall.spin.x = -(angleSin * v31);
    cueBall.spin.y = angleCos * v31;
    cueBall.spin.z = shotSpin.x * spinFactor;
}

void Prediction::determineBallsPositions() {
    int i;
    bool isAnyBallMovingOrSpinning;
    double time;
    double time2;
    do {
        time = TIME_PER_TICK;
        // A zero-length sub-step is legitimate: it is how an already-touching
        // pair left over from a simultaneous collision gets resolved (only one
        // collision record exists per sub-step, so a genuine tie needs a second
        // pass at t=0). It does not advance `time`, so it needs its own bound -
        // without one, a ball wedged between two others could alternate forever.
        // 256 >> the 120 distinct pairs 16 balls can form.
        int zeroSteps = 0;
        do {
            time2 = time;
            this->guiData.collision.valid = false;
            // find the next collision for each ball
            for (i = 0; i < this->guiData.ballsCount; i++) {
                Ball &ball = this->guiData.balls[i];
                if (ball.onTable) {
                    ball.findNextCollision(&this->guiData, &time2);
                }
            }
            // The move loop is skipped on a zero-length step: move() appends a
            // trajectory point whenever the new position is not collinear with
            // the last two, and a zero-length move makes that test degenerate
            // (both segment lengths are 0), so it would append a duplicate point
            // for every zero step.
            if (time2 > 0.0) {
                zeroSteps = 0;
                // move all balls to their collision positions
                for (i = 0; i < this->guiData.ballsCount; i++) {
                    Ball &ball = this->guiData.balls[i];
                    if (ball.onTable && ball.isMovingOrSpinning()) {
                        ball.move(time2);
                    }
                }
            } else if (++zeroSteps > 256) {
                break;
            }
            if (this->guiData.collision.valid) {
                this->handleCollision();
                if (this->guiData.collision.firstHitBall != nullptr && this->m_candidate.idx != -1) {
                    this->firstHitIsTarget = (this->guiData.collision.firstHitBall->index == this->m_candidate.idx);
                    // if (!this->firstHitIsTarget) return; // Removed to enable full combination shots calculations
                }
            }
            // Suction runs LAST, on the sub-step that actually elapsed, so the
            // next scan solves every pair against the velocity the balls will
            // really travel with. Applying it before the move would reintroduce
            // the mismatch the ordering fix removes (TOI solved for v, motion
            // integrated with v + pull), which is what produced the overlap.
            // Using time2 rather than the remaining-time bound also makes the
            // total pull over one tick TIME_PER_TICK * 120 regardless of how
            // many collisions subdivided it - previously it scaled with the
            // subdivision count and with ball index order.
            if (time2 > 0.0) {
                for (i = 0; i < this->guiData.ballsCount; i++) {
                    Ball &ball = this->guiData.balls[i];
                    if (ball.onTable) ball.applyPocketSuction(time2);
                }
            }
            time -= time2;
        } while (time > MIN_TIME);
        isAnyBallMovingOrSpinning = false;
        for (i = 0; i < this->guiData.ballsCount; i++) {
            Ball &ball = this->guiData.balls[i];
            if (ball.onTable && ball.isMovingOrSpinning()) {
                ball.calcVelocity();
                if (ball.isMovingOrSpinning()) {
                    isAnyBallMovingOrSpinning = true;
                }
            }
        }
    } while (isAnyBallMovingOrSpinning);
}

void Prediction::handleCollision() {
    Ball &ballA = *(this->guiData.collision.ballA);
    if ((!fastCalc || forceFullSimulation) && !suppressTrajectories) ballA.positions.push_back(ballA.predictedPosition);
    
    switch (this->guiData.collision.type) {
        case Collision::Type::BALL: {
            // ballB is only ever set for BALL collisions; LINE/POINT leave it
            // null, so dereferencing it here (top of the function) was UB.
            Ball &ballB = *(this->guiData.collision.ballB);
            this->handleBallBallCollision();
            if ((!fastCalc || forceFullSimulation) && !suppressTrajectories) ballB.positions.push_back(ballB.predictedPosition);
            if (this->guiData.collision.firstHitBall == nullptr) this->guiData.collision.firstHitBall = &ballB;
            break;
        }
        case Collision::Type::LINE:
            // تسجيل الاصطدام فقط لإطلاق شرارة من موضع الكرة.
            ballA.tableImpactPoints.push_back(ballA.predictedPosition);
            this->logRailResponse("LINE", ballA, this->guiData.collision.angle);
            this->guiData.collision.railCollisions++;
            break;
        default:
            Point2D delta = {
                this->guiData.collision.point.y - ballA.predictedPosition.y,
                -(this->guiData.collision.point.x - ballA.predictedPosition.x)
            };
            this->guiData.collision.angle = -NumberUtils::calcAngle(delta);
            this->logRailResponse("POINT", ballA, this->guiData.collision.angle);
            this->guiData.collision.railCollisions++;
            break;
    }
}

// Calls calcVelocityPostCollision and, on the honest run only, records the same
// fields the LiveRail engine hook records (pos, pre/post velocity, post spin,
// angle). Rail response is already engine-delegated, so these two lines should
// agree exactly for the same contact — which is what makes the rail a cheap
// thing to rule out before blaming ball-ball.
void Prediction::logRailResponse(const char *kind, Ball &ball, const double &angle) const {
    if (!logPredSimCollisions || fastCalc) {
        ball.calcVelocityPostCollision(angle);
        return;
    }
    double v0x = ball.velocity.x, v0y = ball.velocity.y;
    double s0x = ball.spin.x, s0y = ball.spin.y, s0z = ball.spin.z;
    ball.calcVelocityPostCollision(angle);
    // %.6f and angle at %.9f to match mod/live_log.h: at %.3f/%.4f the print
    // quantum sat above the per-collision residual being measured, so engine and
    // prediction lines compared equal while the true values differed.
    predsimWrite("[PREDSIM] RAIL-%s B#%d pos(%.6f,%.6f) vel(%.6f,%.6f) spin(%.6f,%.6f,%.6f) angle=%.9f -> vel(%.6f,%.6f) spin(%.6f,%.6f,%.6f)",
                 kind, ball.index, ball.predictedPosition.x, ball.predictedPosition.y,
                 v0x, v0y, s0x, s0y, s0z, angle,
                 ball.velocity.x, ball.velocity.y,
                 ball.spin.x, ball.spin.y, ball.spin.z);
}

void Prediction::handleBallBallCollision() const {
    Ball &ballA = *(this->guiData.collision.ballA);
    Ball &ballB = *(this->guiData.collision.ballB);
    Point2D relativePosition = ballA.predictedPosition - ballB.predictedPosition;
    double distanceSq = relativePosition.square();
    if (distanceSq < 1e-12) {
        // Coincident centers: nudge apart so the pair cannot ghost through
        // each other for the rest of the simulation.
        ballA.predictedPosition.x += 1e-6;
        relativePosition = ballA.predictedPosition - ballB.predictedPosition;
        distanceSq = relativePosition.square();
        if (distanceSq < 1e-12) return;
    }
    double invDistance = 1.0 / sqrt(distanceSq);
    Point2D collisionNormal = relativePosition * invDistance;

    // Perfectly elastic equal-mass swap: exchange the normal velocity
    // components; tangential components pass through unchanged.
    double velocityAN = ballA.velocity.x * collisionNormal.x + ballA.velocity.y * collisionNormal.y;
    double velocityBN = ballB.velocity.x * collisionNormal.x + ballB.velocity.y * collisionNormal.y;
    // Engine (libmain+0x2ca6db8) applies the swap unconditionally: separating
    // but still-touching balls also get the push, which keeps breaks/pockets
    // clean. No separating guard here.
    double impulse = velocityBN - velocityAN;
    double va0x = ballA.velocity.x, va0y = ballA.velocity.y;
    double vb0x = ballB.velocity.x, vb0y = ballB.velocity.y;
    ballA.velocity += collisionNormal * impulse;
    ballB.velocity -= collisionNormal * impulse;

    // Spin is logged deliberately even though this function never touches it:
    // the engine's ball-ball (libmain+0x2ca6db8) has a flag-gated branch at
    // +0x148 that rewrites the normal, and its own hook records spin AFTER the
    // call. If the live log shows the engine changing either ball's spin at
    // contact while these values stay put, the spin-free swap here is the
    // divergence — and spin feeds the friction integrator, so the error shows
    // up as a resting position that drifts off the drawn line.
    if (logPredSimCollisions && !fastCalc) {
        predsimWrite("[PREDSIM] BALLBALL A#%d pos(%.6f,%.6f) vel(%.6f,%.6f) | B#%d pos(%.6f,%.6f) vel(%.6f,%.6f) | dist=%.6f -> A vel(%.6f,%.6f) spin(%.6f,%.6f,%.6f) B vel(%.6f,%.6f) spin(%.6f,%.6f,%.6f)",
                     ballA.index, ballA.predictedPosition.x, ballA.predictedPosition.y,
                     va0x, va0y, ballB.index,
                     ballB.predictedPosition.x, ballB.predictedPosition.y, vb0x, vb0y,
                     sqrt(distanceSq),
                     ballA.velocity.x, ballA.velocity.y, ballA.spin.x, ballA.spin.y, ballA.spin.z,
                     ballB.velocity.x, ballB.velocity.y, ballB.spin.x, ballB.spin.y, ballB.spin.z);
    } else {
        (void)va0x; (void)va0y; (void)vb0x; (void)vb0y;
    }
}

 void Prediction::determineShotState() {
    this->guiData.shotState = false;
    // cue ball didn't hit any other ball
    if (this->guiData.collision.firstHitBall == nullptr) {
        return;
    }
    // cue ball potted
    if (!this->guiData.balls[0].onTable) {
        return;
    }
    BallEnums::Classification playerClassification = sharedGameManager.getPlayerClassification();
    // 8-ball before break
    if (playerClassification == BallEnums::ANY) {
        if (this->guiData.collision.firstHitBall->classification ==
            BallEnums::EIGHT_BALL) {
            return;
        }
        for (int i = 0; i < this->guiData.ballsCount; i++) {
            Ball &ball = this->guiData.balls[i];
            // any ball except 8-ball has been potted during current shot
            if (ball.originalOnTable != ball.onTable) {
                this->guiData.shotState = this->guiData.balls[8].onTable;
                return;
            }
        }
    } else {
        // after break
        if (this->guiData.collision.firstHitBall->classification != playerClassification) {
            return;
        }
        // 9-ball mode
        if (playerClassification == BallEnums::NINE_BALL_RULE) {
            for (int i = 1; i < this->guiData.ballsCount; i++) {
                Ball &ball = this->guiData.balls[i];
                // ball has been potted during current shot
                if (ball.originalOnTable != ball.onTable) {
                    this->guiData.shotState = true;
                    return;
                }
            }
            return;
        }
    }
    // 8-ball mode after break
    if (playerClassification == BallEnums::EIGHT_BALL) {
        // 8-ball has been potted during current shot
        this->guiData.shotState = !this->guiData.balls[8].onTable;
        return;
    }
    // to only check balls with correct classification
    int startBall = (playerClassification == BallEnums::SOLID) ? 1 : 9;
    for (int i = startBall; i < startBall + 7; i++) {
        Ball &ball = this->guiData.balls[i];
        // any ball except 8-ball has been potted during current shot
        if (ball.originalOnTable != ball.onTable) {
            this->guiData.shotState = this->guiData.balls[8].onTable;
            return;
        }
    }
} 

/* ============================================================================================== */

/* ============================================================================================== */

/* BALL PUBLIC METHODS ========================================================================== */

#ifndef PREDICTION_HELPERS
#define PREDICTION_HELPERS
inline const std::array<Point2D, TABLE_POCKETS_COUNT>& getPockets() {
    static std::array<Point2D, TABLE_POCKETS_COUNT> dynPockets;
    static bool dynInit = false;

    // Refresh from live memory at most once per epoch (see g_pocketEpoch). Only a
    // SUCCESSFUL read latches the epoch, so a failed walk still retries on the
    // very next call exactly as the uncached version did.
    if (g_pocketCachedEpoch != g_pocketEpoch && sharedGameManager) {
        Table table = sharedGameManager.mTable;
        if (table) {
            auto props = table.mTableProperties();
            if (props) {
                Vec2d* pArr = props.mPockets();
                if (pArr) {
                    for (int i = 0; i < 6; i++) {
                        dynPockets[i] = Point2D(pArr[i].x, pArr[i].y);
                    }
                    dynInit = true;
                    g_pocketCachedEpoch = g_pocketEpoch;
                }
            }
        }
    }

    if (dynInit) return dynPockets;

    static const std::array<Point2D, TABLE_POCKETS_COUNT> POCKET_POSITIONS = {
            Point2D(-130.8, -67.3),
            Point2D(0, -72),
            Point2D(130.8, -67.3),
            Point2D(130.8, 67.3),
            Point2D(0, 72),
            Point2D(-130.8, 67.3)
    };
    return POCKET_POSITIONS;
}

inline const std::array<Point2D, TABLE_SHAPE_SIZE>& getTableShape() {
    static const std::array<Point2D, TABLE_SHAPE_SIZE> TABLE_SHAPE = {
            Point2D(-127, 53.5),
            Point2D(-136.9, 64.1),
            Point2D(-138.2, 69.2),
            Point2D(-136.7, 73.2),
            Point2D(-132.7, 74.7),
            Point2D(-127.6, 73.4),
            Point2D(-117, 63.5),
            Point2D(-7.8, 63.5),
            Point2D(-6.1, 68.6),
            Point2D(-5.7, 72.7),
            Point2D(-3.7, 75.4),
            Point2D(0, 76.7),
            Point2D(3.7, 75.4),
            Point2D(5.7, 72.7),
            Point2D(6.1, 68.6),
            Point2D(7.8, 63.5),
            Point2D(117, 63.5),
            Point2D(127.6, 73.4),
            Point2D(132.7, 74.7),
            Point2D(136.7, 73.2),
            Point2D(138.2, 69.2),
            Point2D(136.9, 64.1),
            Point2D(127, 53.5),
            Point2D(127, -53.5),
            Point2D(136.9, -64.1),
            Point2D(138.2, -69.2),
            Point2D(136.7, -73.2),
            Point2D(132.7, -74.7),
            Point2D(127.6, -73.4),
            Point2D(117, -63.5),
            Point2D(7.8, -63.5),
            Point2D(6.1, -68.6),
            Point2D(5.7, -72.7),
            Point2D(3.7, -75.4),
            Point2D(0, -76.7),
            Point2D(-3.7, -75.4),
            Point2D(-5.7, -72.7),
            Point2D(-6.1, -68.6),
            Point2D(-7.8, -63.5),
            Point2D(-117, -63.5),
            Point2D(-127.6, -73.4),
            Point2D(-132.7, -74.7),
            Point2D(-136.7, -73.2),
            Point2D(-138.2, -69.2),
            Point2D(-136.9, -64.1),
            Point2D(-127, -53.5)
    };
    return TABLE_SHAPE;
}

// Finds the first time a moving ball enters the pocket drop circle during
// the current integration window. Checking only the current position misses
// fast shots that cross the pocket between two physics ticks.
inline bool FindPocketEntryTime(const Point2D& position, const Point2D& velocity,
                                const Point2D& pocket, double radius,
                                double maxTime, double& entryTime) {
    const Point2D relative = position - pocket;
    const double c = relative.square() - radius * radius;
    if (c <= 0.0) {
        entryTime = 0.0;
        return true;
    }

    const double a = velocity.square();
    if (a < 1e-12 || maxTime < 0.0) return false;
    const double b = 2.0 * (relative.x * velocity.x + relative.y * velocity.y);
    const double discriminant = b * b - 4.0 * a * c;
    if (discriminant < 0.0) return false;

    const double root = sqrt(discriminant);
    const double t0 = (-b - root) / (2.0 * a);
    const double t1 = (-b + root) / (2.0 * a);
    const double first = (t0 >= -MIN_TIME) ? t0 : t1;
    if (first < -MIN_TIME || first > maxTime + MIN_TIME) return false;
    entryTime = std::max(0.0, first);
    return true;
}
#endif

inline void Prediction::Ball::findNextCollision(void *pData, double *time) {
    auto *data = reinterpret_cast<SceneData *>(pData);
    const auto& pockets = getPockets();
    // find collisions with other balls
    if (this->state == ::Ball::State::DEFAULT) {
        for (int i = this->index + 1; i < data->ballsCount; i++) {
            Ball &otherBall = data->balls[i];
            // One unconditional test per pair, exactly like the engine loop at
            // libmain+0x3724530 (pure index counter x26 = x19 + 1, increment,
            // compare-to-count). The engine has NO preferred-ball concept, so
            // neither do we: the widened +1e-11 probe window that used to live
            // here let *time advance PAST another pair's true TOI, which made
            // that pair start the next sub-tick already overlapping. Overlap
            // means c < 0, the earlier root goes negative, FUN_02b1b2d0 rejects
            // it at the `0.0 <= dVar1` gate, and once the pair separates the
            // approach gate (b > 0) fails too - so the two balls ghost through
            // each other for the rest of the simulation. A cliff, not a drift.
            if (otherBall.state == ::Ball::State::DEFAULT && this->isBallBallCollision(time, otherBall)) {
                data->collision.valid = true;
                data->collision.ballA = this;
                data->collision.type = Collision::Type::BALL;
                data->collision.ballB = &otherBall;
            }
        }
    }
    // POCKET CAPTURE — checked every tick, NOT gated on willCollideWithTable.
    // The table-bounds AABB is a coarse rectangle that sits INSIDE the pocket
    // jaws (jaw points protrude past the play rectangle, pocket centers are
    // outside it). A ball already past the rectangle and heading into the mouth
    // used to fail the bounds sweep -> the pocket test was skipped -> the ball
    // drifted through the jaws and got ejected off the table.
    if (this->state == ::Ball::State::DEFAULT) { // check if this ball is potted
        for (int i = 0; i < TABLE_POCKETS_COUNT; i++) {
            Point2D delta = pockets[i] - this->predictedPosition;
            double deltaSquare = delta.square();
            // ENGINE-EXACT CAPTURE: an INSTANTANEOUS centre-within-BALL_RADIUS
            // test, which is what libmain does (and all five references:
            // `deltaSquare < BALL_RADIUS_SQUARE`). A ball only reaches that
            // inner circle because the 120.0 mouth suction drags it there over
            // several ticks, and a fast tangential ball can out-run the pull and
            // leave again. That escape IS the engine's rattle behaviour.
            //
            // The previous test swept-captured anything whose PATH grazed a 7.0
            // circle: 3.4x the engine's capture area, tested along the whole
            // sub-step instead of at a point, and short-circuiting the pull
            // entirely - so a ball merely skimming a mouth was potted in the
            // prediction while the engine rolled it on. The line said "in", the
            // ball stayed out. MIDDLE pockets suffer most: a ball tracking the
            // long rail passes within ~5-7 of a middle pocket centre routinely,
            // i.e. inside the fake circle and outside the real one. 7.0 appears
            // in no reference and nowhere in the engine's physics constant table
            // (which does hold 8.0, 1.5, 120.0, 2.5, 0.804 and 1e-11).
            if (deltaSquare < BALL_RADIUS_SQUARE) {
                this->state = ::Ball::State::IN_POCKET;
                this->pocketIndex = i;
                Prediction::pocketStatus[i] = true;
                break;
            }
            // Suction is NOT applied here any more - see applyPocketSuction.
            // findNextCollision is a query: every pair's time-of-impact is
            // computed from the velocities as they stand. Mutating velocity
            // mid-scan invalidated the TOIs of every pair already tested this
            // sub-step (pairs (j,i) with j < i were solved with ball i's OLD
            // velocity), so those balls travelled a different distance than the
            // solver assumed and ended the sub-step OVERLAPPING. That is the
            // pocket-edge error: it can only trigger inside the suction zone,
            // which is exactly where the reported misdraws happen.
        }
    }

    // The rails ALWAYS run. The engine (libmain+0x3606d2c) calls
    // determineBallTableCollision unconditionally; it has no notion of
    // suppressing a jaw. Skipping it while the ball sat inside the pocket
    // suction zone let the ball tunnel straight through the corner-pocket
    // BACK WALL (shape segments 2-3, 18-19, 25-26, 41-42): those vertices lie
    // 7.64-8.34 from the pocket centre, i.e. OUTSIDE the 7.0 drop circle but
    // INSIDE the 8.0 pull zone, so a glancing ball was neither potted nor
    // reflected. Once past the wall the one-sided normal test (v.n <= 0)
    // rejects every segment from the outside, so nothing could bring it back
    // and the ball left the table. The middle pockets never showed this: their
    // back wall sits 4.70-5.74 out, wholly inside the drop circle, so capture
    // always fired first. That asymmetry is why only the corner rails misbehaved.
    if (this->state != ::Ball::State::IN_POCKET) {
        if (this->willCollideWithTable(time)) {
            this->determineBallTableCollision(pData, time);
        }
    }

    if (this->state == ::Ball::State::IN_POCKET) {
        this->state = ::Ball::State::UNKNOWN;
        this->onTable = false;
        this->velocity.nullify();
        this->spin.nullify();
    }
}

/* void Prediction::Ball::calcVelocity() {
    if (!this->isMovingOrSpinning()) {
        return;
    }
    
    double v15 = BALL_RADIUS * this->spin.x - this->velocity.y;
    double v16 = -this->velocity.x - this->spin.y * BALL_RADIUS;
    double v17 = sqrt(v16 * v16 + v15 * v15);
    double v18 = v17 * 0.0014577259475218659; // _frictionProperties._timeOfequilibriumFactor
    if (v18 > unk_35B3F80) {
        double v20 = (v18 < TIME_PER_TICK) ? (v17 * 0.0014577259475218659) : TIME_PER_TICK;
        double v21 = 196.0 * v20 / v17; // _frictionProperties._velocityReductionSlidingFactor
        double v22 = v16 * v21;
        double v23 = v15 * v21;
        this->velocity.x += v22;
        this->velocity.y += v23;
        this->spin.x -= v23 * 0.6578125102783204; // unk_35B3F88 / BALL_RADIUS
        this->spin.y += v22 * 0.6578125102783204; // unk_35B3F88 / BALL_RADIUS
    }
    if (v18 < TIME_PER_TICK) {
        double v24 = this->velocity.x;
        double v25 = this->velocity.y;
        double v27 = (TIME_PER_TICK - v18) * 10.878; // frictionProperties._velocityReductionRollingFactor
        double v28 = 1.0 - v27 / sqrt(v25 * v25 + v24 * v24);
        v28 = (v28 < 0.0) ? 0.0 : v28;
        this->velocity.x = v24 * v28;
        this->velocity.y = v25 * v28;
        this->spin.x = v25 * v28 / BALL_RADIUS;
        this->spin.y = -(v24 * v28) / BALL_RADIUS;
    }
    constexpr double v29 = 9.8 * TIME_PER_TICK; // frictionProperties._deltaSpinFactor
    this->spin.z = (this->spin.z > 0.0) ? fmax(this->spin.z - v29, 0.0) : fmin(this->spin.z + v29, 0.0);
} */

// DAT_04c8b9a8 2.0E
// FUN_02b1bb3c
/* void Prediction::Ball::calcVelocityPostCollision(const double &angle) {
    double angleCos = round(cos(angle) * 10000.0) / 10000.0;
    double angleSin = round(sin(angle) * 10000.0) / 10000.0;
    double velocityX = angleCos * this->velocity.x - angleSin * this->velocity.y;
    double velocityY = angleSin * this->velocity.x + angleCos * this->velocity.y;
    double spinFactor = velocityX - BALL_RADIUS * this->spin.z;
    double absSpinFactor = (spinFactor > 0.0) ? spinFactor : -spinFactor;
    double velocityFactor = absSpinFactor / 2.5;
    double absVelocityY = (velocityY > 0.0) ? velocityY : -velocityY;
    double spinDirection = (spinFactor > 0.0) ? 1.0 : -1.0; // DAT_04c8b9a0 1.0E
    double minSpinFactor = 0.4 * absVelocityY;
    if (velocityFactor < minSpinFactor) minSpinFactor = velocityFactor;
    double spinChange = spinDirection * minSpinFactor;
    double newVelocityX = velocityX - spinChange / 2.5; // DAT_04c8bc80 2.5E
    double newVelocityY = -0.804 * velocityY; // -(velocityY * dword_35B7978) // DAT_04cb4410 0.804E
    this->velocity.x = angleSin * newVelocityY + angleCos * newVelocityX;
    this->velocity.y = angleCos * newVelocityY - newVelocityX * angleSin;
    double newSpinX = angleSin * this->spin.x + angleCos * this->spin.y;
    double newSpinY = angleCos * this->spin.x - angleSin * this->spin.y - velocityY * 0.1420875022201172; // dword_35B7988 / BALL_RADIUS   DAT_04cb4420 0.54E
    double newSpinZ = this->spin.z + spinChange * 0.6578125102783204; // unk_35B7A28 / BALL_RADIUS
    this->spin.x = angleSin * newSpinX + angleCos * newSpinY;
    this->spin.y = angleCos * newSpinX - newSpinY * angleSin;
    this->spin.z = newSpinZ;
} */

inline void Prediction::Ball::move(const double &time) {
    if (this->velocity) {
        this->predictedPosition.x += this->velocity.x * time;
        this->predictedPosition.y += this->velocity.y * time;
        
        if ((!fastCalc || forceFullSimulation) && !Prediction::suppressTrajectories) {
            auto lastIndex = this->positions.size() - 1;
            if (lastIndex > 1) {
                auto &a = this->positions[lastIndex - 1];
                auto &b = this->positions[lastIndex];
                auto &c = this->predictedPosition;
                // Angle-relative tolerance instead of a fixed cross-product
                // epsilon. The old 1e-6 dropped the tail points of slow curves
                // (cross ~ L^2 * sin(theta) shrinks with segment length),
                // chording across the curve. Comparing sin(theta) to the
                // segment lengths keeps the curve dense at any speed.
                double cross = (b.y - a.y) * (c.x - b.x) - (c.y - b.y) * (b.x - a.x);
                double lenSq1 = (b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y);
                double lenSq2 = (c.x - b.x) * (c.x - b.x) + (c.y - b.y) * (c.y - b.y);
                if (cross * cross < 1e-6 * lenSq1 * lenSq2) return;
            } this->positions.push_back(this->predictedPosition);
        }
    }
}

inline bool Prediction::Ball::isMovingOrSpinning() const {
    return bool(this->velocity) || bool(this->spin);
}

// Pocket-mouth suction, moved out of findNextCollision so the collision scan
// stays a pure query. Applied to every ball in its own pass AFTER the sub-step's
// collisions are resolved, so all balls see the same integration window and no
// ball's velocity changes underneath a time-of-impact that was already solved.
// The pull itself (non-normalised, delta * time * 120) is unchanged.
inline void Prediction::Ball::applyPocketSuction(const double &time) {
    if (this->state != ::Ball::State::DEFAULT) return;
    const auto& pockets = getPockets();
    for (int i = 0; i < TABLE_POCKETS_COUNT; i++) {
        Point2D delta = pockets[i] - this->predictedPosition;
        if (delta.square() < POCKET_RADIUS_SQUARE) {
            double pull = time * 120.0; // old F(double, libmain+0x4dae0c0) reads 0.0 (dead libm table)
            this->velocity.x += delta.x * pull;
            this->velocity.y += delta.y * pull;
        }
    }
}

// Called every frame from menu.h. Watches the real balls for the moving->stopped
// transition around a shot and, at the moment they stop, writes the per-ball
// delta between the prediction's rest positions (frozen when the shot started)
// and the engine's actual rest positions.
//
// The freeze matters: the prediction keeps recomputing from the live table while
// the balls roll, so by the time they stop g_predEndLive describes a completely
// different shot. Only the snapshot taken as motion begins corresponds to the
// line the player was looking at when they fired.
static void capturePredVsRealEnd() {
    if (!logPredSimCollisions) return;
    if (!sharedGameManager) return;
    Table table = sharedGameManager.mTable;
    if (!table) return;
    auto& balls = table.mBalls();
    if (!balls) return;

    // Reuse AutoPlay's motion test rather than re-scanning: it applies the same
    // 1e-6 velocity/spin threshold and menu.h already calls it this frame, so
    // the per-ball isInstanceOf checks stay at one pass.
    const bool moving = AutoPlay::AreBallsMoving();

    static bool wasMoving = false;
    // RUN# of the prediction already reported, so a settle that re-arms from the
    // SAME stale prediction is not reported twice. Previously a shot that came to
    // rest, twitched, and rested again emitted a second block comparing the old
    // prediction against the new rest state - which is where the absurd
    // d=171/99/44 values came from. They were latch artifacts, not drift.
    static uint64_t lastReportedRun = ~0ULL;
    if (moving && !wasMoving) {
        // Shot just started: freeze the prediction that produced the drawn line.
        g_predEndArmed = g_predEndLive;
        g_predEndArmedValid = (g_predEndArmed.count > 0 && g_predEndArmed.run != lastReportedRun);
        wasMoving = true;
        return;
    }
    if (moving) return;
    if (!wasMoving) return;
    wasMoving = false;

    if (!g_predEndArmedValid) return;
    g_predEndArmedValid = false;
    lastReportedRun = g_predEndArmed.run;

    const PredEndSnapshot &p = g_predEndArmed;
    // Sequential shot number for a full-game capture: RUN# counts every overlay
    // recompute (thousands per game, one per aim adjustment), so it is useless
    // for correlating with "the 7th shot went wrong". This counts actual shots.
    static int shotNo = 0;
    ++shotNo;
    predsimWrite("[REALEND] SHOT#%d vs RUN#%llu angle=%.4f power=%.2f predRails=%d",
                 shotNo, p.run, p.angle, p.power, p.rails);
    double worst = 0.0;
    int worstIdx = -1;
    for (int i = 0; i < balls.Count && i < p.count; i++) {
        ::Ball b = balls[i];
        if (!b) continue;
        bool realOnTable = b.isOnTable();
        auto rp = b.position();
        // Both sides agreeing the ball is off the table is agreement, not a
        // mismatch. The old test fired whenever either side was off-table, so a
        // ball potted long before this shot (identical position on both sides)
        // was reported as STATE-MISMATCH in every subsequent block.
        if (!realOnTable && !p.onTable[i]) continue;
        if (!realOnTable || !p.onTable[i]) {
            // A pot/no-pot disagreement is a different class of error than a
            // drift, so it is reported as such rather than as a big distance.
            predsimWrite("[REALEND] B#%d STATE-MISMATCH pred(onTable=%d pos %.4f,%.4f) real(onTable=%d pos %.4f,%.4f)",
                         i, p.onTable[i] ? 1 : 0, p.pos[i].x, p.pos[i].y,
                         realOnTable ? 1 : 0, rp.x, rp.y);
            continue;
        }
        double dx = rp.x - p.pos[i].x, dy = rp.y - p.pos[i].y;
        double d = sqrt(dx * dx + dy * dy);
        if (d > worst) { worst = d; worstIdx = i; }
        predsimWrite("[REALEND] B#%d pred(%.4f,%.4f) real(%.4f,%.4f) d=%.4f (%.1f%%R)",
                     i, p.pos[i].x, p.pos[i].y, rp.x, rp.y, d, 100.0 * d / BALL_RADIUS);
    }
    predsimWrite("[REALEND] SHOT#%d worst B#%d d=%.4f (%.1f%%R)", shotNo, worstIdx, worst, 100.0 * worst / BALL_RADIUS);
    predsimFlush();
}

/* ============================================================================================== */

/* BALL PRIVATE METHODS */

#include "Prediction.update.h"

/* bool Prediction::Ball::willCollideWithTable(const double *smallestTime) const {
    double currentX = this->predictedPosition.x;
    double currentY = this->predictedPosition.y;
    double predictedX = currentX + this->velocity.x * *smallestTime;
    double predictedY = currentY + this->velocity.y * *smallestTime;
    double leftX;
    double rightX;
    double bottomY;
    double topY;
    if (this->velocity.x > 0.0) {
        leftX = currentX;
        rightX = predictedX;
    } else {
        leftX = predictedX;
        rightX = currentX;
    }
    if (this->velocity.y > 0.0) {
        topY = currentY;
        bottomY = predictedY;
    } else {
        topY = predictedY;
        bottomY = currentY;
    }
    return (leftX < TABLE_BOUND_LEFT || rightX > TABLE_BOUND_RIGHT || topY < TABLE_BOUND_TOP ||
            bottomY > TABLE_BOUND_BOTTOM);
} */

inline void Prediction::Ball::determineBallTableCollision(void *pData, double *smallestTime) {
    double angle;
    auto *data = reinterpret_cast<Prediction::SceneData *>(pData);
    const auto& tableShape = getTableShape();

    // Diagnostic: check if this ball is inside any pocket's suction zone.
    bool inSuctionZone = false;
    if (logSuspiciousRailHits) {
        const auto& pockets = getPockets();
        for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
            Point2D delta = pockets[p] - this->predictedPosition;
            if (delta.square() < POCKET_RADIUS_SQUARE) {
                inSuctionZone = true;
                break;
            }
        }
    }

    for (int i = 0; i < TABLE_SHAPE_SIZE; i++) {
        const Point2D &point = tableShape[i];
        const Point2D &nextPoint = tableShape[(i + 1) % TABLE_SHAPE_SIZE];
        if (this->isBallLineCollision(smallestTime, point, nextPoint)) {
            angle = NumberUtils::calcAngle(nextPoint, point);
            data->collision.valid = true;
            data->collision.ballA = this;
            data->collision.type = Collision::Type::LINE;
            data->collision.angle = -angle;

            // Log only suspicious rail hits: those inside a pocket suction zone,
            // where the corner-pocket back-wall tunnelling bug lived.
            if (inSuctionZone) {
                Point2D midpoint = {(point.x + nextPoint.x) * 0.5, (point.y + nextPoint.y) * 0.5};
                predsimWrite("SUSPIC_RAIL B#%d seg=%d pos(%.2f,%.2f) vel(%.2f,%.2f) mid(%.2f,%.2f) angle=%.4f",
                             this->index, i, this->predictedPosition.x, this->predictedPosition.y,
                             this->velocity.x, this->velocity.y, midpoint.x, midpoint.y, -angle);
            }
        }
        // NOT else-if: a ball entering the jaw can hit the BALL_RADIUS circle
        // around the corner point EARLIER than the line offset by BALL_RADIUS.
        // Both finders only accept a time earlier than the current bound, so
        // testing both keeps the true earliest (point) response instead of
        // letting the later line response win.
        if (this->isBallPointCollision(smallestTime, point)) {
            data->collision.valid = true;
            data->collision.ballA = this;
            data->collision.point = point;
            data->collision.type = Collision::Type::POINT;
        }
    }
}

/* bool Prediction::Ball::isBallLineCollision(double *pTime_1, const Point2D &tableShapePointA,
                                           const Point2D &tableShapePointB) const {
    if (!this->velocity) {
        return false;
    }
    Point2D delta = tableShapePointB - tableShapePointA;
    double v17 = delta.y * this->velocity.x - delta.x * this->velocity.y;
    if (v17 == 0.0) {
        return false;
    }
    double invDistance = 1.0 / sqrt(delta.square());
    double v21 = invDistance * BALL_RADIUS;
    double v22 = this->predictedPosition.x - tableShapePointA.x - delta.y * v21;
    double v23 = this->predictedPosition.y - tableShapePointA.y + delta.x * v21;
    double v24 = (v22 * -this->velocity.y - v23 * -this->velocity.x) / v17;
    if (v24 <= 0.0 || v24 >= 1.0) {
        return false;
    }
    double time = (delta.x * v23 - delta.y * v22) / v17;
    if (time <= 0.0 || (time - 1E-11 > *pTime_1)) {
        return false;
    }
    if (this->velocity.x * (delta.y * invDistance) + this->velocity.y * -(delta.x * invDistance) >
        0.0) {
        return false;
    }
    *pTime_1 = time;
    return true;
} */

/* bool Prediction::Ball::isBallPointCollision(double *smallestTime, const Point2D &tableShapePoint) const {
    Point2D delta = tableShapePoint - this->predictedPosition;
    double v16 = -(this->velocity.x * delta.x * 2.0) - (this->velocity.y * delta.y * 2.0);
    if (v16 >= 0.0) {
        return false;
    }
    double velocitySquare = this->velocity.square();
    double distanceSquare = delta.square();
    double unkSquare = v16 * v16;
    if (distanceSquare - unkSquare / (velocitySquare * 4.0) >= BALL_RADIUS_SQUARE) {
        return false;
    }
    double v22 = (-v16 -
                  sqrt(unkSquare - velocitySquare * 4.0 * (distanceSquare - BALL_RADIUS_SQUARE))) /
                 (velocitySquare * 2.0);
    if (v22 < 0.0) {
        return false;
    }
    if (v22 - unk_35B7A20 > *smallestTime) {
        return false;
    }
    *smallestTime = v22;
    return true;
} */

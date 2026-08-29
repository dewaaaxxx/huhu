// =====================================================================
// HumanScan.impl.h — Human AI implementation (8-Ball, Direct Shots Only)
//
// Included from game.h AFTER AutoPlay.impl.h (single TU), so it can reuse
// AutoPlay statics, CalculateRequiredPower, DistToSegmentSq and Prediction.
// =====================================================================

#include "HumanScan.h"
#include "game/GameManager.h"
#include "Prediction.h"

#include <math.h>
#include <algorithm>
#include <utility>

namespace HumanScan {

namespace {

    // --- EasyFilter thresholds (v1) ---
    constexpr double CUT_MIN = 0.28;            // keep a useful margin for human aim error
    constexpr double MAX_CUE_DIST = 285.0;      // cue -> ghost max distance
    constexpr double MAX_POCKET_DIST = 300.0;   // ball -> pocket max distance
    constexpr double BLOCK_MARGIN_SQ = (2.0 * BALL_RADIUS) * (2.0 * BALL_RADIUS);
    constexpr double TOO_CLOSE_TO_NEXT = 2.5 * BALL_RADIUS; // cue rests too near the next ball to shoot

    // --- Ranking ---
    constexpr int    MAX_CANDIDATES = 20;
    // Safety valve: cap total sims per decision so a wide candidate set can
    // never stall the render thread for seconds.
    constexpr int    SIM_BUDGET = 600;

    // --- LookAhead / Position Play ---
    // ComputeShape now adds CueRestPenalty (capped at 120) on top of the raw
    // next-shot geometry, so this "a follow-up exists" ceiling is lifted by
    // the same amount. Without the lift, a perfectly good leave that happens
    // to sit near a rail would be reported as having NO follow-up and the AI
    // would talk itself into a needless safety.
    constexpr double LOOKAHEAD_MAX_SCORE = 300.0;
    constexpr double SHAPE_WEIGHT = 1.2;         // weight of next-shot ease vs current shot ease
    constexpr double SHAPE_WEIGHT_LAST = 2.0;    // heavier weight when positioning for the 8-ball
    constexpr double SHAPE_BAD = 1e18;           // no playable follow-up from the resting spot
    constexpr int    MAX_REFINE = 5;             // potting candidates refined with the spin grid
    constexpr int    MAX_SAFE_REFINE = 4;        // safe candidates refined to try converting to pots

    // 8-Ball endgame: shots on the 8-ball are always played firmly so the
    // stroke never reads as an ultra-soft (invalid) tap in-game.
    constexpr double EIGHTBALL_MIN_POWER = 300.0;
    // Floor for a controlled "nothing to pot" contact: strong enough to be a
    // valid shot but not a 300-power blast that risks scratching into a loss.
    constexpr double EIGHTBALL_VALID_FLOOR = 200.0;
    constexpr double POST_BREAK_MIN_POWER = 180.0;

    // Spin/power grid for position play (Phase B). x/y = raw english
    // (mEnglish), pMult = power multiplier vs the computed potting power.
    struct SpinVariant {
        double x, y;
        double pMult;
        int    preset;
        const char* tag;
    };
    const SpinVariant SPIN_VARIANTS[] = {
        { 0.00,  0.00, 1.00, AutoPlay::SPIN_CENTER, "c"  },
        { 0.00,  0.65, 1.00, AutoPlay::SPIN_TOP,    "t"  },
        { 0.00, -0.65, 1.00, AutoPlay::SPIN_BOTTOM, "b"  },
        { 0.00,  0.90, 1.00, AutoPlay::SPIN_TOP,    "T"  },
        { 0.00, -0.90, 1.00, AutoPlay::SPIN_BOTTOM, "F"  }, // full draw — pulls the cue back into the open
        { 0.55,  0.00, 1.00, AutoPlay::SPIN_RIGHT,  "r"  },
        {-0.55,  0.00, 1.00, AutoPlay::SPIN_LEFT,   "l"  },
        { 0.45,  0.65, 1.00, AutoPlay::SPIN_RIGHT,  "rT" }, // follow + right english
        {-0.45,  0.65, 1.00, AutoPlay::SPIN_LEFT,   "lT" }, // follow + left english
        { 0.45, -0.65, 1.00, AutoPlay::SPIN_RIGHT,  "rB" }, // draw + right english
        {-0.45, -0.65, 1.00, AutoPlay::SPIN_LEFT,   "lB" }, // draw + left english
        { 0.00,  0.00, 0.90, AutoPlay::SPIN_CENTER, "s"  },
        { 0.00,  0.00, 1.10, AutoPlay::SPIN_CENTER, "h"  },
        { 0.00,  0.00, 1.25, AutoPlay::SPIN_CENTER, "H"  },
        { 0.25,  0.00, 1.00, AutoPlay::SPIN_RIGHT,  "rS" },
        {-0.25,  0.00, 1.00, AutoPlay::SPIN_LEFT,   "lS" },
        { 0.00,  0.35, 1.00, AutoPlay::SPIN_TOP,    "tS" },
        { 0.00, -0.35, 1.00, AutoPlay::SPIN_BOTTOM, "bS" },
        { 0.75,  0.00, 1.00, AutoPlay::SPIN_RIGHT,  "rH" },
        {-0.75,  0.00, 1.00, AutoPlay::SPIN_LEFT,   "lH" },
        { 0.55,  0.80, 1.00, AutoPlay::SPIN_RIGHT,  "rHT" },
        {-0.55,  0.80, 1.00, AutoPlay::SPIN_LEFT,   "lHT" },
        { 0.55, -0.80, 1.00, AutoPlay::SPIN_RIGHT,  "rHB" },
        {-0.55, -0.80, 1.00, AutoPlay::SPIN_LEFT,   "lHB" },
    };
    constexpr int SPIN_VARIANTS_COUNT = (int)(sizeof(SPIN_VARIANTS) / sizeof(SPIN_VARIANTS[0]));

    // --- Run-out planner (multi-shot position play) ---
    constexpr int    RUN_BEAM = 2;     // best branches kept per planning level
    constexpr int    RUN_BALLS = 2;    // candidate balls tried per level
    constexpr int    RUN_BUDGET = 140; // hard cap on planner-only simulations
    constexpr double RUN_MAX_SHOT_SCORE = 320.0; // planned steps must stay this easy (no hero shots)

    // --- Re-analysis guard ---
    constexpr double RETRY_WINDOW = 2.0;        // seconds before retrying a failed decision
    constexpr double CUE_MOVE_EPS_SQ = 0.0025;  // cue moved enough to re-decide

    inline Point2D Normalize2D(const Point2D& v) {
        double l = sqrt(v.square());
        if (l < 1e-6) return Point2D(0.0, 0.0);
        return v * (1.0 / l);
    }

    inline bool PocketIsLocked(const TableInfo& table, int pocket) {
        return table.only8BallLeft && table.nominationMode == 1 &&
               table.nominatedPocket >= 0 && table.nominatedPocket < 6 &&
               pocket != table.nominatedPocket;
    }

    inline bool TargetPathBlocked(const Point2D& target, const Point2D& pocket,
                                  const std::vector<BallEntry>& balls, int targetIdx) {
        const double marginSq = (2.05 * BALL_RADIUS) * (2.05 * BALL_RADIUS);
        for (const auto& other : balls) {
            if (other.index == targetIdx) continue;
            if (DistToSegmentSq(other.pos, target, pocket) < marginSq) return true;
        }
        return false;
    }

    inline void GetRailBounds(const Point2D* pockets, double& minX, double& maxX,
                              double& minY, double& maxY) {
        minX = 1e18; maxX = -1e18;
        minY = 1e18; maxY = -1e18;
        for (int i = 0; i < TABLE_POCKETS_COUNT; i++) {
            minX = std::min(minX, pockets[i].x);
            maxX = std::max(maxX, pockets[i].x);
            minY = std::min(minY, pockets[i].y);
            maxY = std::max(maxY, pockets[i].y);
        }
        // Pocket centers sit outside the playable cushion face. Move inward
        // by the pocket radius to approximate the actual rail rectangle.
        minX += POCKET_RADIUS;
        maxX -= POCKET_RADIUS;
        minY += POCKET_RADIUS;
        maxY -= POCKET_RADIUS;
    }

    inline bool PointInsideRail(const Point2D& point, const Point2D* pockets, double extraMargin = 0.0) {
        double minX, maxX, minY, maxY;
        GetRailBounds(pockets, minX, maxX, minY, maxY);
        const double margin = BALL_RADIUS * 1.5 + extraMargin;
        return point.x >= minX - margin && point.x <= maxX + margin &&
               point.y >= minY - margin && point.y <= maxY + margin;
    }

    inline double RailAlignmentPenalty(const Point2D& ball, const Point2D& pocket,
                                       const Point2D* pockets) {
        return 0.0; // All pockets equal with zero penalty
    }

    inline double PowerPenalty(double power, double idealPower) {
        if (power <= idealPower) {
            // If a softer stroke still pots, prefer it. This is the natural
            // choice for a center-table ball and reduces unnecessary force.
            return (idealPower - power) * 0.006 + power * 0.003;
        }
        return (power - idealPower) * 0.04 + power * 0.008;
    }

    inline bool PowerAllowedForCurrentPhase(double power) {
        return !ctx.postBreakShot || power >= POST_BREAK_MIN_POWER;
    }

    inline bool BreakStateChanged(const std::vector<BallEntry>& before,
                                  const TableInfo& after) {
        if (before.size() != after.allBalls.size()) return true;
        const double movedSq = (1.5 * BALL_RADIUS) * (1.5 * BALL_RADIUS);
        for (const auto& oldBall : before) {
            bool found = false;
            for (const auto& newBall : after.allBalls) {
                if (newBall.index != oldBall.index) continue;
                found = true;
                if ((newBall.pos - oldBall.pos).square() > movedSq) return true;
                break;
            }
            if (!found) return true;
        }
        return false;
    }

    // Reads all non-cue balls into ctx.table.allBalls once per decision cycle.
    static void cacheAllOnTableBalls() {
        ctx.table.allBalls.clear();
        Table table = sharedGameManager.mTable;
        if (!table) return;
        auto& balls = table.mBalls();
        if (!balls) return;
        for (int i = 1; i < (int)balls.Count; i++) {
            Ball b = balls[i];
            if (!b || !b.isOnTable()) continue;
            BallEntry e;
            e.index = i;
            e.pos = b.position();
            e.cls = b.classification();
            ctx.table.allBalls.push_back(e);
        }
    }

    // Writes the raw english (spin) into the visual english control so both
    // the simulation (getShotSpin) and the eventual fire use the same spin.
    inline void SetEnglish(double x, double y) {
        auto vc = sharedGameManager.mVisualEnglishControl();
        if (vc) vc.mEnglish(Vec2d(x, y));
    }

    // Runs one full simulation of the candidate's current angle/power using
    // whatever english is set on the control right now.
    inline void SimulateShot(HumanCandidate& c) {
        ctx.simsUsed++;
        Candidate cand{c.idx, c.angle, c.score, c.pocket, c.power, c.dist};
        gPrediction->forceFullSimulation = true;
        gPrediction->determineShotResult(true, c.angle, c.power, sharedGameManager.getShotSpin(), cand);
        gPrediction->forceFullSimulation = false;
    }

} // anonymous namespace

// ---------------------------------------------------------------------
// 1. AnalyzeTable — live read, zero simulation.
// ---------------------------------------------------------------------
bool AnalyzeTable() {
    ctx.table = TableInfo{};
    Table table = sharedGameManager.mTable;
    if (!table) return false;
    auto& balls = table.mBalls();
    if (!balls) return false;

    const auto& pockets = getPockets();
    for (int i = 0; i < TABLE_POCKETS_COUNT; i++) ctx.table.pockets[i] = pockets[i];

    ctx.table.gameGroup = sharedGameManager.getPlayerClassification();
    uint nomPocket = sharedGameManager.getNominatedPocket();
    ctx.table.nominatedPocket = (nomPocket < 6) ? (int)nomPocket : 6;

    ctx.table.nominationMode = sharedGameManager.getPocketNominationMode();
    ctx.table.eightBallMustBank = sharedGameManager.is8BallCushionShotRequired();

    int racked = 0;
    for (int i = 0; i < (int)balls.Count; i++) {
        Ball b = balls[i];
        if (!b) continue;
        if (i == 0) {
            ctx.table.cueOnTable = b.isOnTable();
            ctx.table.cuePos = b.position();
            continue;
        }
        if (!b.isOnTable()) continue;
        Point2D p = b.position();
        if (p.x >= 70.0 && p.x <= 120.0) racked++;
        Ball::Classification c = b.classification();
        if (c == Ball::Classification::SOLID) ctx.table.solidsLeft++;
        else if (c == Ball::Classification::STRIPE) ctx.table.stripesLeft++;
    }

    ctx.table.solidsPotted = std::max(0, 7 - ctx.table.solidsLeft);
    ctx.table.stripesPotted = std::max(0, 7 - ctx.table.stripesLeft);

    Ball::Classification g = ctx.table.gameGroup;
    if (g == Ball::Classification::SOLID) {
        ctx.table.only8BallLeft = (ctx.table.solidsLeft == 0);
        ctx.table.onLastGroupBall = (ctx.table.solidsLeft == 1);
    } else if (g == Ball::Classification::STRIPE) {
        ctx.table.only8BallLeft = (ctx.table.stripesLeft == 0);
        ctx.table.onLastGroupBall = (ctx.table.stripesLeft == 1);
    } else if (g == Ball::Classification::EIGHT_BALL) {
        ctx.table.only8BallLeft = true;
    } else {
        ctx.table.only8BallLeft = (ctx.table.solidsLeft == 0 && ctx.table.stripesLeft == 0);
        ctx.table.onLastGroupBall = (ctx.table.solidsLeft + ctx.table.stripesLeft == 1);
    }

    // Ignore a stale pocket nomination left over from a previous 8-ball shot.
    if (!ctx.table.only8BallLeft || ctx.table.nominationMode != 1) {
        ctx.table.nominatedPocket = 6;
    }

    ctx.table.isBreak = (racked >= 13);

    // Cache all balls once per decision cycle.
    cacheAllOnTableBalls();

    return true;
}

// ---------------------------------------------------------------------
// 2. ChooseGroup — zero simulation.
// ---------------------------------------------------------------------
void ChooseGroup() {
    Ball::Classification g = ctx.table.gameGroup;
    if (g == Ball::Classification::SOLID ||
        g == Ball::Classification::STRIPE ||
        g == Ball::Classification::EIGHT_BALL) {
        ctx.table.myGroup = g;
        return;
    }

    // Both groups are still open — evaluate which is easier to clear.
    auto scoreGroup = [&](Ball::Classification group) -> double {
        int open = 0, clustered = 0;
        double totalPocketDist = 0.0, totalCueDist = 0.0;
        int count = 0;

        for (auto& t : ctx.table.allBalls) {
            if (t.cls != group) continue;
            count++;

            // Cue-to-ball distance
            double cueDist = sqrt((t.pos - ctx.table.cuePos).square());
            totalCueDist += cueDist;

            // Nearest pocket distance
            double bestPocket = 1e18;
            Point2D bestPocketPos;
            for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
                double d = sqrt((t.pos - ctx.table.pockets[p]).square());
                if (d < bestPocket) {
                    bestPocket = d;
                    bestPocketPos = ctx.table.pockets[p];
                }
            }
            totalPocketDist += bestPocket;

            // Check if blocked or clustered
            bool blocked = false;
            double nearSq = (3.5 * BALL_RADIUS) * (3.5 * BALL_RADIUS);
            int nearCount = 0;
            for (auto& o : ctx.table.allBalls) {
                if (o.index == t.index) continue;
                double d2 = (o.pos - t.pos).square();
                if (d2 < nearSq) nearCount++;
                double dSeg = DistToSegmentSq(o.pos, ctx.table.cuePos,
                    t.pos - Normalize2D(bestPocketPos - t.pos) * (2.0 * BALL_RADIUS));
                if (dSeg < BLOCK_MARGIN_SQ) blocked = true;
            }
            if (nearCount >= 2) clustered++;
            if (blocked) clustered++;
            else open++;
        }

        if (count == 0) return -1e18;
        double avgPocket = totalPocketDist / count;
        double avgCue = totalCueDist / count;
        return open * 100.0 - avgPocket * 2.5 - avgCue * 2.0 - clustered * 40.0;
    };

    double solidScore = scoreGroup(Ball::Classification::SOLID);
    double stripeScore = scoreGroup(Ball::Classification::STRIPE);

    if (solidScore > stripeScore + 15.0) {
        ctx.table.myGroup = Ball::Classification::SOLID;
    } else if (stripeScore > solidScore + 15.0) {
        ctx.table.myGroup = Ball::Classification::STRIPE;
    } else {
        ctx.table.myGroup = Ball::Classification::ANY; // let the shot decide
    }
}

// ---------------------------------------------------------------------
// 3. CreateCandidates — direct shots only, zero simulation.
// ---------------------------------------------------------------------
void CreateCandidates() {
    ctx.candidates.clear();
    const auto& T = ctx.table;

    for (auto& t : T.allBalls) {
        bool legal = false;
        if (T.only8BallLeft) {
            legal = (t.index == 8);
        } else if (T.myGroup == Ball::Classification::ANY) {
            legal = (t.cls != Ball::Classification::EIGHT_BALL);
        } else {
            legal = (t.cls == T.myGroup);
        }
        if (!legal) continue;

        for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
            if (PocketIsLocked(T, p)) continue;

            Point2D rawPocket = T.pockets[p];
            Point2D pocket = GetEffectivePocketTarget(p, rawPocket, t.pos);
            Point2D toPocket = pocket - t.pos;
            double distP = sqrt(toPocket.square());
            if (distP < 0.1) continue;
            Point2D dirP = toPocket * (1.0 / distP);
            Point2D ghost = t.pos - dirP * (2.0 * BALL_RADIUS);
            if (!PointInsideRail(ghost, T.pockets, BALL_RADIUS * 1.5)) continue;

            Point2D shotLine = ghost - T.cuePos;
            double cueDist = sqrt(shotLine.square());
            double angle = atan2(shotLine.y, shotLine.x);
            if (angle < 0) angle += 2.0 * M_PI;

            Point2D cueToBall = t.pos - T.cuePos;
            double dcb = sqrt(cueToBall.square());
            double cutDot = 1.0;
            if (dcb > 0.001) {
                Point2D dirCue = cueToBall * (1.0 / dcb);
                cutDot = dirCue.x * dirP.x + dirCue.y * dirP.y;
            }

            if ((p == 1 || p == 4) && cutDot <= 0.70) continue; // Relaxed gate: allow angled side-pocket shots
            int nPowers = 1;
            const double pows[1] = { 1.0 };
            for (int k = 0; k < nPowers; k++) {
                double power = CalculateRequiredPower(cueDist, distP, cutDot) * pows[k];
                if (p == 1 || p == 4) {
                    if (power > 400.0) power = 400.0; // Relaxed clamp: allow harder side-pocket shots
                }
                if (power > (double)AutoPlay::powerMax) power = (double)AutoPlay::powerMax;
                if (power < (double)AutoPlay::powerMin) power = (double)AutoPlay::powerMin;

                double score = distP * 1.0 + 0.8 * cueDist + (1.0 - cutDot) * 120.0;
                if (T.only8BallLeft && T.nominationMode == 1 &&
                    T.nominatedPocket >= 0 && T.nominatedPocket < 6) score -= 30.0;

                HumanCandidate c;
                c.idx = t.index;
                c.pocket = p;
                c.angle = angle;
                c.power = power;
                c.dist = distP;
                c.cueDist = cueDist;
                c.cutDot = cutDot;
                c.ballPos = t.pos;
                c.ghost = ghost;
                c.score = score;
                ctx.candidates.push_back(c);
            }
        }
    }
}

// ---------------------------------------------------------------------
// 3c. AppendIndirectCandidates — combination shots (cue -> A -> B -> pocket)
//     and general 1-rail cue banks for any legal ball.
//
//     CreateCandidates only ever produced straight cue->ball->pocket shots,
//     so whenever every legal ball was screened off the Human AI found
//     nothing and dropped into TrySafeTap. These two families are what a
//     real player looks for next, and they are validated by the same sim +
//     evaluateSimulation path as everything else (c.idx stays the FIRST ball
//     contacted, so the first-contact legality check still holds).
//
//     Appended AFTER EasyFilter with a score offset so they rank behind any
//     available direct shot and only get simulated when they are needed.
// ---------------------------------------------------------------------
void AppendIndirectCandidates() {
    const auto& T = ctx.table;

    auto isLegal = [&](int index, Ball::Classification cls) {
        if (T.only8BallLeft) return index == 8;
        if (T.myGroup == Ball::Classification::ANY)
            return cls != Ball::Classification::EIGHT_BALL;
        return cls == T.myGroup;
    };
    auto pathClear = [&](const Point2D& a, const Point2D& b, int skipA, int skipB) {
        for (auto& o : T.allBalls) {
            if (o.index == skipA || o.index == skipB) continue;
            if (DistToSegmentSq(o.pos, a, b) < BLOCK_MARGIN_SQ) return false;
        }
        return true;
    };

    // --- Family 1: combinations. Cue strikes A, A drives B into the pocket.
    for (auto& A : T.allBalls) {
        if (!isLegal(A.index, A.cls)) continue;
        for (auto& B : T.allBalls) {
            if (B.index == A.index) continue;
            if (!isLegal(B.index, B.cls)) continue;

            for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
                if (PocketIsLocked(T, p)) continue;
                Point2D pocket = GetEffectivePocketTarget(p, T.pockets[p], B.pos);
                Point2D toPocket = pocket - B.pos;
                double distBP = sqrt(toPocket.square());
                if (distBP < 0.1 || distBP > MAX_POCKET_DIST) continue;
                Point2D dirBP = toPocket * (1.0 / distBP);

                // Ghost for B: where A must arrive.
                Point2D ghostB = B.pos - dirBP * (2.0 * BALL_RADIUS);
                if (!PointInsideRail(ghostB, T.pockets, BALL_RADIUS)) continue;

                Point2D aToGhostB = ghostB - A.pos;
                double distAB = sqrt(aToGhostB.square());
                if (distAB < 0.1 || distAB > 140.0) continue;
                Point2D dirAB = aToGhostB * (1.0 / distAB);
                if (dirAB.x * dirBP.x + dirAB.y * dirBP.y < 0.45) continue;

                // Ghost for A: where the cue must arrive.
                Point2D ghostA = A.pos - dirAB * (2.0 * BALL_RADIUS);
                if (!PointInsideRail(ghostA, T.pockets, BALL_RADIUS)) continue;
                Point2D shotLine = ghostA - T.cuePos;
                double cueDist = sqrt(shotLine.square());
                if (cueDist < TOO_CLOSE_TO_NEXT || cueDist > MAX_CUE_DIST) continue;

                Point2D cueToA = A.pos - T.cuePos;
                double dca = sqrt(cueToA.square());
                if (dca < 0.001) continue;
                Point2D dirCue = cueToA * (1.0 / dca);
                double cutDot = dirCue.x * dirAB.x + dirCue.y * dirAB.y;
                if (cutDot < 0.55) continue;   // combos need a near-full hit

                if (!pathClear(T.cuePos, ghostA, A.index, -1)) continue;
                if (!pathClear(A.pos, ghostB, A.index, B.index)) continue;
                if (TargetPathBlocked(B.pos, T.pockets[p], T.allBalls, B.index)) continue;

                double angle = atan2(shotLine.y, shotLine.x);
                if (angle < 0) angle += 2.0 * M_PI;
                double power = CalculateRequiredPower(cueDist, distAB + distBP, cutDot) * 1.15;
                if (power > (double)AutoPlay::powerMax) power = (double)AutoPlay::powerMax;
                if (power < (double)AutoPlay::powerMin) power = (double)AutoPlay::powerMin;

                HumanCandidate c;
                c.idx = A.index;          // first contact, as the rules check
                c.pocket = p;
                c.angle = angle;
                c.power = power;
                c.dist = distBP;
                c.cueDist = cueDist;
                c.cutDot = cutDot;
                c.ballPos = A.pos;
                c.ghost = ghostA;
                c.score = distBP + distAB + 0.8 * cueDist +
                          (1.0 - cutDot) * 120.0 + 110.0;
                ctx.candidates.push_back(c);
            }
        }
    }

    // --- Family 2: 1-rail cue banks for ANY legal ball (same mirror geometry
    //     CreateBankCandidates uses, which was reachable only for a must-bank
    //     8-ball). This is how the AI gets out of a screened-off table
    //     instead of surrendering to a safety tap.
    double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
    for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
        if (T.pockets[p].x < minX) minX = T.pockets[p].x;
        if (T.pockets[p].x > maxX) maxX = T.pockets[p].x;
        if (T.pockets[p].y < minY) minY = T.pockets[p].y;
        if (T.pockets[p].y > maxY) maxY = T.pockets[p].y;
    }
    const double co = 0.5;
    struct Cushion { double v; bool isX; };
    const Cushion cush[4] = { { minX + co, true }, { maxX - co, true },
                              { minY + co, false }, { maxY - co, false } };

    for (auto& t : T.allBalls) {
        if (!isLegal(t.index, t.cls)) continue;
        for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
            if (PocketIsLocked(T, p)) continue;
            Point2D pocket = GetEffectivePocketTarget(p, T.pockets[p], t.pos);
            Point2D toPocket = pocket - t.pos;
            double distP = sqrt(toPocket.square());
            if (distP < 0.1 || distP > MAX_POCKET_DIST) continue;
            Point2D dirP = toPocket * (1.0 / distP);
            Point2D ghost = t.pos - dirP * (2.0 * BALL_RADIUS);
            if (!PointInsideRail(ghost, T.pockets, BALL_RADIUS)) continue;
            if (TargetPathBlocked(t.pos, T.pockets[p], T.allBalls, t.index)) continue;

            for (int ci = 0; ci < 4; ci++) {
                Point2D mg = ghost;
                if (cush[ci].isX) mg.x = 2.0 * cush[ci].v - ghost.x;
                else              mg.y = 2.0 * cush[ci].v - ghost.y;

                Point2D shotLine = mg - T.cuePos;
                double cueDist = sqrt(shotLine.square());
                if (cueDist < TOO_CLOSE_TO_NEXT || cueDist > MAX_CUE_DIST * 1.4) continue;

                // Bounce point = where cue->mirroredGhost crosses the cushion.
                Point2D bp;
                if (cush[ci].isX) {
                    double den = mg.x - T.cuePos.x;
                    if (fabs(den) < 1e-6) continue;
                    double u = (cush[ci].v - T.cuePos.x) / den;
                    if (u <= 0.02 || u >= 0.98) continue;
                    bp.x = cush[ci].v;
                    bp.y = T.cuePos.y + u * (mg.y - T.cuePos.y);
                    if (bp.y < minY - 5.0 || bp.y > maxY + 5.0) continue;
                } else {
                    double den = mg.y - T.cuePos.y;
                    if (fabs(den) < 1e-6) continue;
                    double u = (cush[ci].v - T.cuePos.y) / den;
                    if (u <= 0.02 || u >= 0.98) continue;
                    bp.x = T.cuePos.x + u * (mg.x - T.cuePos.x);
                    bp.y = cush[ci].v;
                    if (bp.x < minX - 5.0 || bp.x > maxX + 5.0) continue;
                }

                if (!pathClear(T.cuePos, bp, t.index, -1)) continue;
                if (!pathClear(bp, ghost, t.index, -1)) continue;

                double angle = atan2(shotLine.y, shotLine.x);
                if (angle < 0) angle += 2.0 * M_PI;
                double power = CalculateRequiredPower(cueDist, distP, 0.70) * 1.25;
                if (power > (double)AutoPlay::powerMax) power = (double)AutoPlay::powerMax;
                if (power < (double)AutoPlay::powerMin) power = (double)AutoPlay::powerMin;

                HumanCandidate c;
                c.idx = t.index;
                c.pocket = p;
                c.angle = angle;
                c.power = power;
                c.dist = distP;
                c.cueDist = cueDist;
                c.cutDot = 0.5;   // a banked cue arrives at an unpredictable cut
                c.ballPos = t.pos;
                c.ghost = ghost;
                c.score = distP + cueDist * 1.2 + 170.0 +
                          RailAlignmentPenalty(t.pos, pocket, T.pockets);
                ctx.candidates.push_back(c);
            }
        }
    }
}

// ---------------------------------------------------------------------
// 4. EasyFilter — reject unsuitable candidates before any simulation.
// ---------------------------------------------------------------------
void EasyFilter() {
    std::vector<HumanCandidate> kept;

    for (auto& c : ctx.candidates) {
        if (c.cutDot < CUT_MIN) continue;
        if (c.cueDist > MAX_CUE_DIST) continue;
        if (c.dist > MAX_POCKET_DIST) continue;

        bool blocked = false;
        for (auto& o : ctx.table.allBalls) {
            if (o.index == c.idx) continue;
            double d2 = DistToSegmentSq(o.pos, ctx.table.cuePos, c.ghost);
            if (d2 < BLOCK_MARGIN_SQ) { blocked = true; break; }
        }
        if (blocked) continue;
        if (TargetPathBlocked(c.ballPos, ctx.table.pockets[c.pocket],
                              ctx.table.allBalls, c.idx)) continue;

        kept.push_back(c);
    }

    // De-duplicate near-identical angles per ball, keep the better pocket.
    std::vector<HumanCandidate> dedup;
    for (auto& c : kept) {
        bool dup = false;
        for (auto& k : dedup) {
            if (k.idx == c.idx && fabs(k.angle - c.angle) < 0.02) {
                if (c.score < k.score) k = c;
                dup = true;
                break;
            }
        }
        if (!dup) dedup.push_back(c);
    }
    ctx.candidates.swap(dedup);
}

// ---------------------------------------------------------------------
// 5. RankCandidates — geometric only, keep the best few.
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// 3b. CreateBankCandidates — 1-rail bank shots (cue ball reflects off
//     a cushion before contacting the target). Used when the 8-ball must
//     be banked (is8BallCushionShotRequired) or when no direct path exists.
//     Uses mirror geometry: reflect the ghost ball across the cushion,
//     aim at the mirrored ghost. Up to 12 candidates with sim validation.
// ---------------------------------------------------------------------
void CreateBankCandidates() {
    ctx.candidates.clear();
    const auto& T = ctx.table;
    int targetIdx = T.only8BallLeft ? 8 : -1;
    if (targetIdx < 0) return;

    // Find the target ball
    Point2D ballPos;
    bool found = false;
    for (auto& t : T.allBalls) {
        if (t.index == targetIdx) { ballPos = t.pos; found = true; break; }
    }
    if (!found) return;

    // Four cushions: left(x~0), right(x~254), top(y~0), bottom(y~127)
    // We approximate cushion positions from the pocket extremes.
    double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
    for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
        if (T.pockets[p].x < minX) minX = T.pockets[p].x;
        if (T.pockets[p].x > maxX) maxX = T.pockets[p].x;
        if (T.pockets[p].y < minY) minY = T.pockets[p].y;
        if (T.pockets[p].y > maxY) maxY = T.pockets[p].y;
    }
    // Slight inward offset so the mirror is on the cushion face
    const double cushionOffset = 0.5;

    struct Cushion { double v; bool isX; };
    Cushion cushions[4] = {
        { minX + cushionOffset, true  },  // left
        { maxX - cushionOffset, true  },  // right
        { minY + cushionOffset, false },  // top
        { maxY - cushionOffset, false },  // bottom
    };

    struct BankCandidate { double angle; double power; double score; Point2D ghost; int pocket; };

    for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
        if (PocketIsLocked(T, p)) continue;
        Point2D rawPocket = T.pockets[p];
        Point2D pocket = GetEffectivePocketTarget(p, rawPocket, ballPos);

        for (int ci = 0; ci < 4; ci++) {
            // Mirror the ghost ball across the cushion
            Point2D dirToPocket = pocket - ballPos;
            double distToPocket = sqrt(dirToPocket.square());
            if (distToPocket < 0.1) continue;
            Point2D dirP = dirToPocket * (1.0 / distToPocket);
            Point2D ghost = ballPos - dirP * (2.0 * BALL_RADIUS);
            if (!PointInsideRail(ghost, T.pockets)) continue;

            // Mirror ghost across the cushion
            Point2D mirrorGhost = ghost;
            if (cushions[ci].isX) {
                mirrorGhost.x = 2.0 * cushions[ci].v - ghost.x;
            } else {
                mirrorGhost.y = 2.0 * cushions[ci].v - ghost.y;
            }

            Point2D shotLine = mirrorGhost - T.cuePos;
            double cueDist = sqrt(shotLine.square());
            if (cueDist < 0.1) continue;
            double angle = atan2(shotLine.y, shotLine.x);
            if (angle < 0) angle += 2.0 * M_PI;

            // The bounce point is the intersection of cue->mirrorGhost with the cushion
            Point2D bouncePoint;
            if (cushions[ci].isX) {
                double denominator = mirrorGhost.x - T.cuePos.x;
                if (fabs(denominator) < 1e-6) continue;
                double t = (cushions[ci].v - T.cuePos.x) / denominator;
                bouncePoint.x = cushions[ci].v;
                bouncePoint.y = T.cuePos.y + t * (mirrorGhost.y - T.cuePos.y);
            } else {
                double denominator = mirrorGhost.y - T.cuePos.y;
                if (fabs(denominator) < 1e-6) continue;
                double t = (cushions[ci].v - T.cuePos.y) / denominator;
                bouncePoint.x = T.cuePos.x + t * (mirrorGhost.x - T.cuePos.x);
                bouncePoint.y = cushions[ci].v;
            }

            // Validate bounce point is on the cushion segment (between pockets)
            bool onCushion = false;
            if (cushions[ci].isX) {
                onCushion = (bouncePoint.y >= minY - 5.0 && bouncePoint.y <= maxY + 5.0);
            } else {
                onCushion = (bouncePoint.x >= minX - 5.0 && bouncePoint.x <= maxX + 5.0);
            }
            if (!onCushion) continue;

            // Path from cue to bounce point must be clear
            bool blocked = false;
            for (auto& o : T.allBalls) {
                if (o.index == targetIdx) continue;
                double d2 = DistToSegmentSq(o.pos, T.cuePos, bouncePoint);
                if (d2 < BLOCK_MARGIN_SQ) { blocked = true; break; }
            }
            if (blocked) continue;
            if (TargetPathBlocked(ballPos, pocket, T.allBalls, targetIdx)) continue;

            // Path from bounce to ghost must be clear
            for (auto& o : T.allBalls) {
                if (o.index == targetIdx) continue;
                double d2 = DistToSegmentSq(o.pos, bouncePoint, ghost);
                if (d2 < BLOCK_MARGIN_SQ) { blocked = true; break; }
            }
            if (blocked) continue;

            // Reject if cueDist or distToPocket are too large
            if (cueDist > MAX_CUE_DIST * 1.4) continue;
            if (distToPocket > MAX_POCKET_DIST) continue;

            // Power: bank shots need firmer hits (cushion energy loss)
            double power = CalculateRequiredPower(cueDist, distToPocket, 0.70) * 1.25;
            if (power > (double)AutoPlay::powerMax) power = (double)AutoPlay::powerMax;
            if (power < (double)AutoPlay::powerMin) power = (double)AutoPlay::powerMin;

            double score = distToPocket + cueDist * 1.2 + 150.0 +
                           RailAlignmentPenalty(ballPos, pocket, T.pockets);
            if (T.nominationMode == 1 && T.nominatedPocket >= 0 && T.nominatedPocket < 6) score -= 30.0;

            HumanCandidate c;
            c.idx = targetIdx;
            c.pocket = p;
            c.angle = angle;
            c.power = power;
            c.dist = distToPocket;
            c.cueDist = cueDist;
            c.cutDot = 0.5; // bank shots have unpredictable cut
            c.ballPos = ballPos;
            c.ghost = ghost;
            c.score = score;
            ctx.candidates.push_back(c);
        }
    }

    // Apply EasyFilter (dedup, sort) on bank candidates
    std::vector<HumanCandidate> filtered;
    for (auto& c : ctx.candidates) {
        if (c.dist > MAX_POCKET_DIST * 1.3) continue;
        filtered.push_back(c);
    }

    std::vector<HumanCandidate> dedup;
    for (auto& c : filtered) {
        bool dup = false;
        for (auto& k : dedup) {
            if (k.pocket == c.pocket && fabs(k.angle - c.angle) < 0.03) {
                if (c.score < k.score) k = c;
                dup = true;
                break;
            }
        }
        if (!dup) dedup.push_back(c);
    }
    ctx.candidates.swap(dedup);
}
void RankCandidates() {
    std::sort(ctx.candidates.begin(), ctx.candidates.end(),
              [](const HumanCandidate& a, const HumanCandidate& b) {
                  return a.score < b.score;
              });
    int cap = MAX_CANDIDATES;
    if ((int)ctx.candidates.size() > cap) {
        ctx.candidates.resize(cap);
    }
}

// ---------------------------------------------------------------------
// 6. SimulateCandidate — validate ranked candidates with full sims.
//    guiData must hold the just-simulated result on entry to LookAhead.
// ---------------------------------------------------------------------

// Reads gPrediction->guiData (fresh from the last determineShotResult)
// and decides whether the simulated shot is legal for the current group.
//   POTTED  -> legal ball potted (c.pocket synced to the actual pocket)
//   SAFE    -> legal first contact, cue on table, nothing potted (safe shot)
//   INVALID -> foul (scratch / bad contact / 8-ball or wrong group potted)
static SimStatus evaluateSimulation(HumanCandidate& c) {
    static double lastRejDbg = 0.0;
    auto rej = [&](const char* r) {
        double n = AutoPlay::nowSec();
        if (n - lastRejDbg > 2.0) { lastRejDbg = n; LOGI("[HumanScan] reject %s (idx=%d)", r, c.idx); }
    };

    auto& gd = gPrediction->guiData;
    if (gd.ballsCount <= 0) { rej("empty-scene"); return SimStatus::INVALID; }
    if (!gd.balls[0].onTable) { rej("scratch"); return SimStatus::INVALID; }

    auto firstHit = gd.collision.firstHitBall;
    if (!firstHit) { rej("no-contact"); return SimStatus::INVALID; }

    const bool only8 = ctx.table.only8BallLeft;
    const Ball::Classification group = ctx.table.myGroup;

    if (only8) {
        if (firstHit->index != 8) { rej("not-8"); return SimStatus::INVALID; }
    } else {
        if (group == Ball::Classification::ANY) {
            if (firstHit->classification == Ball::Classification::EIGHT_BALL) { rej("hit-8"); return SimStatus::INVALID; }
        } else {
            if (firstHit->classification != group) { rej("wrong-group-hit"); return SimStatus::INVALID; }
        }
    }
    if (firstHit->index != c.idx) { rej("wrong-target-hit"); return SimStatus::INVALID; }

    int pottedIdx = -1;
    for (int i = 1; i < gd.ballsCount; i++) {
        auto& b = gd.balls[i];
        if (!b.originalOnTable || b.onTable) continue; // not potted

        bool legal = false;
        if (only8) {
            legal = (i == 8);
        } else {
            legal = (group == Ball::Classification::ANY)
                        ? (b.classification != Ball::Classification::EIGHT_BALL)
                        : (b.classification == group);
        }
        if (!legal) { rej("foul-pot"); return SimStatus::INVALID; }
        if (pottedIdx == -1) pottedIdx = i;
    }

    if (pottedIdx == -1) {
        // First-contact legality was already enforced above; a safe (nothing
        // potted) with legal first contact is a legal shot.
        c.valid = true;
        c.potted = false;
        return SimStatus::SAFE; // legal contact, nothing potted
    }

    // Every potted ball was verified legal above (own group; never opponent;
    // never the 8 unless only8). Multi-pot combos are allowed; the planned
    // pocket just has to match one of the potted balls.
    if (c.pocket >= 0) {
        bool pocketOk = false;
        for (int i = 1; i < gd.ballsCount; i++) {
            auto& b = gd.balls[i];
            if (!b.originalOnTable || b.onTable) continue;
            if (b.pocketIndex == c.pocket) { pocketOk = true; break; }
        }
        if (!pocketOk) {
            rej("wrong-pocket");
            return SimStatus::INVALID;
        }
    }
    c.valid = true;
    c.potted = true;
    return SimStatus::POTTED;
}

// ---------------------------------------------------------------------
// 7-a. CueRestPenalty — how badly the cue ball is trapped where it stops.
//    Purely geometric (no simulation). Mirrors GameMaster::positionSafety,
//    which is the only place in the tree that already scored this; HumanScan
//    had no equivalent, which is exactly why it kept leaving the cue frozen
//    on a rail / buried in a cluster / jammed in a corner and then had to
//    play a desperation safety on the following shot.
//    Returned value is ADDED to the shape score (lower = better), and is
//    capped so it grades positions instead of vetoing them.
// ---------------------------------------------------------------------
double CueRestPenalty(const Point2D& cuePos,
                      const std::vector<BallEntry>& others) {
    double penalty = 0.0;

    // Table extents from the pocket ring (same derivation GameMaster uses).
    double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
    for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
        const Point2D& q = ctx.table.pockets[p];
        if (q.x < minX) minX = q.x;
        if (q.x > maxX) maxX = q.x;
        if (q.y < minY) minY = q.y;
        if (q.y > maxY) maxY = q.y;
    }

    // --- Cushion proximity: a cue tight to the rail kills half the cut angles.
    const double cushionMargin = 18.0;   // ~4.7 ball radii
    const double cushionPen    = 45.0;   // per edge, at full contact
    const double frozenDist    = 1.6 * BALL_RADIUS; // effectively frozen on the rail
    const double frozenPen     = 30.0;

    const double d[4] = { cuePos.x - minX, maxX - cuePos.x,
                          cuePos.y - minY, maxY - cuePos.y };
    int railsTight = 0;
    for (int i = 0; i < 4; i++) {
        if (d[i] < cushionMargin) {
            penalty += (cushionMargin - d[i]) / cushionMargin * cushionPen;
            railsTight++;
            if (d[i] < frozenDist) penalty += frozenPen;
        }
    }
    // Jammed into a corner: two perpendicular rails at once is far worse than
    // the sum of two independent rails, because no straight line escapes.
    if (railsTight >= 2) penalty += 35.0;

    // --- Ball proximity: buried inside a cluster leaves no clean stroke line.
    const double nearR  = 4.0 * BALL_RADIUS;
    const double nearSq = nearR * nearR;
    double ballPen = 0.0;
    for (const auto& o : others) {
        double d2 = (cuePos - o.pos).square();
        if (d2 < nearSq && d2 > 0.01) {
            ballPen += (1.0 - sqrt(d2) / nearR) * 40.0;
        }
    }
    if (ballPen > 70.0) ballPen = 70.0;
    penalty += ballPen;

    // Grade, never veto: SHAPE_BAD stays reserved for "no follow-up exists".
    if (penalty > 120.0) penalty = 120.0;
    return penalty;
}

// ---------------------------------------------------------------------
// 7. ComputeShape — geometric check on the sim's final positions.
//    Zero additional simulations (reads gPrediction->guiData).
//    Returns the ease (lower = better) of the best follow-up shot from the
//    cue's resting spot; 0 when no follow-up is needed (rack finished /
//    8-ball endgame); SHAPE_BAD when nothing playable exists.
// ---------------------------------------------------------------------
double ComputeShape(int& bestNextIdx) {
    bestNextIdx = -1;
    auto& gd = gPrediction->guiData;
    if (!gd.balls[0].onTable) return SHAPE_BAD;

    Point2D cueNext = gd.balls[0].predictedPosition;

    bool nextOnly8 = ctx.table.only8BallLeft;
    if (!nextOnly8 && ctx.table.myGroup != Ball::Classification::ANY) {
        bool groupLeft = false;
        for (int i = 1; i < gd.ballsCount; i++) {
            if (gd.balls[i].onTable &&
                gd.balls[i].classification == ctx.table.myGroup) {
                groupLeft = true;
                break;
            }
        }
        nextOnly8 = !groupLeft;
    }

    std::vector<BallEntry> next;
    std::vector<BallEntry> predictedBalls;
    for (int i = 1; i < gd.ballsCount; i++) {
        auto& b = gd.balls[i];
        if (!b.onTable) continue;
        BallEntry allEntry;
        allEntry.index = i;
        allEntry.pos = b.predictedPosition;
        allEntry.cls = b.classification;
        predictedBalls.push_back(allEntry);
        bool legal;
        if (nextOnly8) {
            legal = (i == 8);
        } else if (ctx.table.myGroup == Ball::Classification::ANY) {
            legal = (b.classification != Ball::Classification::EIGHT_BALL);
        } else {
            legal = (b.classification == ctx.table.myGroup);
        }
        if (!legal) continue;
        next.push_back(allEntry);
    }
    if (next.empty()) return 0.0;

    // When positioning for the 8-ball that must be banked, evaluate the
    // ease of the best bank shot from the resting cue position.
    if (nextOnly8 && ctx.table.eightBallMustBank && !next.empty()) {
        const BallEntry& eightBall = next[0];
        if (eightBall.index != 8) return SHAPE_BAD;

        // Quick geometric check for the easiest 1-rail bank from cueNext to 8-ball
        double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
        for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
            if (ctx.table.pockets[p].x < minX) minX = ctx.table.pockets[p].x;
            if (ctx.table.pockets[p].x > maxX) maxX = ctx.table.pockets[p].x;
            if (ctx.table.pockets[p].y < minY) minY = ctx.table.pockets[p].y;
            if (ctx.table.pockets[p].y > maxY) maxY = ctx.table.pockets[p].y;
        }
        const double co = 1.5;

        double best = SHAPE_BAD;
        for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
            if (PocketIsLocked(ctx.table, p)) continue;

            Point2D toPocket = ctx.table.pockets[p] - eightBall.pos;
            double distP = sqrt(toPocket.square());
            if (distP < 0.1 || distP > MAX_POCKET_DIST) continue;
            Point2D dirP = toPocket * (1.0 / distP);
            Point2D ghost = eightBall.pos - dirP * (2.0 * BALL_RADIUS);

            struct { double v; bool isX; } cushions[4] = {
                { minX + co, true }, { maxX - co, true },
                { minY + co, false }, { maxY - co, false }
            };
            for (int ci = 0; ci < 4; ci++) {
                Point2D mg = ghost;
                if (cushions[ci].isX) mg.x = 2.0 * cushions[ci].v - ghost.x;
                else mg.y = 2.0 * cushions[ci].v - ghost.y;

                double d = sqrt((mg - cueNext).square());
                if (d > MAX_CUE_DIST * 1.4 || d < TOO_CLOSE_TO_NEXT) continue;
                if (TargetPathBlocked(eightBall.pos, ctx.table.pockets[p],
                                      predictedBalls, eightBall.index)) continue;

                double score = distP + d * 1.2 + 150.0;
                if (score < best) { best = score; bestNextIdx = 8; }
            }
        }
        if (best >= SHAPE_BAD) return SHAPE_BAD;
        best += CueRestPenalty(cueNext, predictedBalls);
        return (best < LOOKAHEAD_MAX_SCORE + 100.0) ? best : SHAPE_BAD;
    }

    // Standard geometric scan of the follow-up state
    double best = SHAPE_BAD;
    for (auto& t : next) {
        for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
            if (PocketIsLocked(ctx.table, p)) continue;

            Point2D rawPocket = ctx.table.pockets[p];
            Point2D pocket = GetEffectivePocketTarget(p, rawPocket, t.pos);
            Point2D toPocket = pocket - t.pos;
            double distP = sqrt(toPocket.square());
            if (distP < 0.1 || distP > MAX_POCKET_DIST) continue;
            Point2D dirP = toPocket * (1.0 / distP);
            Point2D ghost = t.pos - dirP * (2.0 * BALL_RADIUS);
            if (!PointInsideRail(ghost, ctx.table.pockets)) continue;
            Point2D shotLine = ghost - cueNext;
            double cueDist = sqrt(shotLine.square());
            if (cueDist > MAX_CUE_DIST) continue;
            if (cueDist < TOO_CLOSE_TO_NEXT) continue;

            Point2D cueToBall = t.pos - cueNext;
            double dcb = sqrt(cueToBall.square());
            double cutDot = 1.0;
            if (dcb > 0.001) {
                Point2D dirCue = cueToBall * (1.0 / dcb);
                cutDot = dirCue.x * dirP.x + dirCue.y * dirP.y;
            }
            if (cutDot < CUT_MIN) continue;
            if (TargetPathBlocked(t.pos, ctx.table.pockets[p],
                                  predictedBalls, t.index)) continue;

            double score = distP + 0.8 * cueDist + (1.0 - cutDot) * 120.0;
            if (nextOnly8 && t.index == 8) {
                // Reward leaving an easy, straight shot on 8-ball!
                double easy8Bonus = (cutDot - 0.3) * 150.0;
                score -= easy8Bonus + 150.0;
            }
            if (score < best) { best = score; bestNextIdx = t.index; }
        }
    }
    if (best >= SHAPE_BAD) return SHAPE_BAD;

    // Grade the leave itself, not just the next pot's geometry. Without this
    // the planner happily potted a ball while welding the cue to a rail or
    // burying it in a cluster, because *some* follow-up still existed on
    // paper — that is the "stupid snooker" behaviour.
    best += CueRestPenalty(cueNext, predictedBalls);
    return best;
}

// ---------------------------------------------------------------------
// 8. LookAheadOneShot — records shape (position) + planned next ball.
// ---------------------------------------------------------------------
void LookAheadOneShot(HumanCandidate& c) {
    int nextIdx = -1;
    double shape = ComputeShape(nextIdx);
    c.shapeScore = shape;
    c.followTarget = nextIdx;
    c.hasFollowUp = (shape < LOOKAHEAD_MAX_SCORE);
}

// ---------------------------------------------------------------------
// 9. RefineWithSpin — Phase B of the decision: tries the spin/power grid
//    on an already-potted candidate and keeps the variant that still pots
//    and gives the best shape (position for the next shot). Also accepts a
//    SAFE candidate and tries to turn it into a real pot with the grid.
//    Returns true if the candidate is (or became) a potting shot.
// ---------------------------------------------------------------------
bool RefineWithSpin(HumanCandidate& c) {
    bool potted = c.potted;
    double sw = ctx.table.onLastGroupBall ? SHAPE_WEIGHT_LAST : SHAPE_WEIGHT;
    for (int v = 0; v < SPIN_VARIANTS_COUNT; v++) {
        double power = c.power * SPIN_VARIANTS[v].pMult;
        if (power > (double)AutoPlay::powerMax) power = (double)AutoPlay::powerMax;
        if (power < (double)AutoPlay::powerMin) power = (double)AutoPlay::powerMin;

        HumanCandidate trial = c;
        trial.power = power;
        SetEnglish(SPIN_VARIANTS[v].x, SPIN_VARIANTS[v].y);
        SimulateShot(trial);
        if (evaluateSimulation(trial) != SimStatus::POTTED) continue;
        potted = true;

        int nextIdx = -1;
        double shape = ComputeShape(nextIdx);
        trial.spin = Vec2d(SPIN_VARIANTS[v].x, SPIN_VARIANTS[v].y);
        trial.spinPreset = SPIN_VARIANTS[v].preset;
        trial.spinTag = SPIN_VARIANTS[v].tag;
        trial.shapeScore = shape;
        trial.followTarget = nextIdx;
        trial.hasFollowUp = (shape < LOOKAHEAD_MAX_SCORE);
        trial.finalScore = trial.score + sw * shape;

        if (trial.finalScore < c.finalScore) c = trial;
    }
    SetEnglish(0.0, 0.0); // leave the english control neutral
    return potted;
}

// ---------------------------------------------------------------------
// 6b. Run-out planner — multi-shot position play. Instead of picking a
//     single pot, it searches a full chain (group balls -> 8-ball) using
//     the simulator's predicted resting positions: every planned shot is
//     sim-validated, and the first shot played is the opening of the best
//     verified run. Bounded beam search (RUN_BEAM x RUN_BALLS x spin grid)
//     with a hard simulation budget.
// ---------------------------------------------------------------------

struct ChainGeo {
    bool   ok = false;
    double angle = 0.0;
    double power = 0.0;
    double distP = 0.0;
    double cueDist = 0.0;
    double cutDot = 1.0;
    double score = 1e18;
    Point2D ghost;
};

struct RunState {
    Point2D cue;
    std::vector<BallEntry> all;    // every on-table ball (blocking checks)
    std::vector<BallEntry> balls;  // legal-to-pot targets (group then 8 last)
};

struct RunPlan {
    std::vector<HumanCandidate> chain;
    double accum = 0.0;
    int    depth = 0;
    bool   complete = false;
};

// Geometry for potting `ball` from `cue` into `pocket` inside state `st`
// (mirrors CreateCandidates + EasyFilter rules; no simulation).
static ChainGeo chainGeometry(const Point2D& cue, const BallEntry& ball,
                              int pIdx, const Point2D& rawPocket, const RunState& st) {
    ChainGeo g;
    Point2D pocket = GetEffectivePocketTarget(pIdx, rawPocket, ball.pos);
    Point2D toPocket = pocket - ball.pos;
    g.distP = sqrt(toPocket.square());
    if (g.distP < 0.1 || g.distP > MAX_POCKET_DIST) return g;
    Point2D dirP = toPocket * (1.0 / g.distP);
    g.ghost = ball.pos - dirP * (2.0 * BALL_RADIUS);
    if (!PointInsideRail(g.ghost, ctx.table.pockets)) return g;
    Point2D shotLine = g.ghost - cue;
    g.cueDist = sqrt(shotLine.square());
    if (g.cueDist > MAX_CUE_DIST || g.cueDist < TOO_CLOSE_TO_NEXT) return g;
    g.angle = atan2(shotLine.y, shotLine.x);
    if (g.angle < 0) g.angle += 2.0 * M_PI;
    Point2D cueToBall = ball.pos - cue;
    double dcb = sqrt(cueToBall.square());
    g.cutDot = 1.0;
    if (dcb > 0.001) {
        Point2D dirCue = cueToBall * (1.0 / dcb);
        g.cutDot = dirCue.x * dirP.x + dirCue.y * dirP.y;
    }
    if (g.cutDot < CUT_MIN) return g;
    for (auto& o : st.all) {
        if (o.index == ball.index) continue;
        double d2 = DistToSegmentSq(o.pos, cue, g.ghost);
        if (d2 < BLOCK_MARGIN_SQ) return g;
    }
    if ((pIdx == 1 || pIdx == 4) && g.cutDot <= 0.70) return g; // Relaxed gate: allow angled side-pocket shots
    if (TargetPathBlocked(ball.pos, pocket, st.all, ball.index)) return g;
    g.power = CalculateRequiredPower(g.cueDist, g.distP, g.cutDot);
    if (pIdx == 1 || pIdx == 4) {
        if (g.power > 400.0) g.power = 400.0;
    }
    if (g.power > (double)AutoPlay::powerMax) g.power = (double)AutoPlay::powerMax;
    if (g.power < (double)AutoPlay::powerMin) g.power = (double)AutoPlay::powerMin;
    g.score = g.distP + 0.8 * g.cueDist + (1.0 - g.cutDot) * 120.0;
    g.ok = true;
    return g;
}

// Ease of the best follow-up pot from a chain state (no simulation).
static double shapeForState(const RunState& st, int& bestNextIdx) {
    bestNextIdx = -1;
    if (st.balls.empty()) return 0.0;
    double best = SHAPE_BAD;
    for (auto& t : st.balls) {
        for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
            if (PocketIsLocked(ctx.table, p)) continue;
            ChainGeo g = chainGeometry(st.cue, t, p, ctx.table.pockets[p], st);
            if (!g.ok) continue;
            if (g.score < best) { best = g.score; bestNextIdx = t.index; }
        }
    }
    return best;
}

// Validates the just-simulated shot (guiData) against the chain rules:
// legal first contact + a legal ball potted. Returns the potted ball index
// (or -1). `only8Now` = this step must pot the 8-ball.
static int chainShotPot(bool only8Now, int targetIdx, int targetPocket) {
    auto& gd = gPrediction->guiData;
    if (!gd.balls[0].onTable) return -1;
    auto fh = gd.collision.firstHitBall;
    if (!fh) return -1;
    if (only8Now) {
        if (fh->index != 8) return -1;
    } else {
        if (fh->classification != ctx.table.myGroup) return -1;
    }
    int pottedIdx = -1;
    for (int i = 1; i < gd.ballsCount; i++) {
        auto& b = gd.balls[i];
        if (!b.originalOnTable || b.onTable) continue;
        if (only8Now) {
            if (i != 8) return -1;
        } else {
            if (b.classification != ctx.table.myGroup) return -1;
        }
        pottedIdx = i;
    }
    if (pottedIdx != targetIdx) return -1;
    if (targetPocket >= 0 && gd.balls[pottedIdx].pocketIndex != targetPocket) return -1;
    return pottedIdx;
}

struct RunChild {
    RunState st;
    std::vector<HumanCandidate> chain;
    double accum = 0.0;
};

// Beam search over the run: try the easiest balls with the spin grid, keep
// the RUN_BEAM best branches, recurse until every ball is potted.
static void planExpand(RunState st, std::vector<HumanCandidate>& chain,
                       double accum, RunPlan& best, int& budget) {
    if (budget <= 0) return;

    if (st.balls.empty()) {
        if (!best.complete || accum < best.accum) {
            best.chain = chain;
            best.accum = accum;
            best.depth = (int)chain.size();
            best.complete = true;
        }
        return;
    }

    // Rank candidate balls by geometric pot ease from this cue. The 8-ball is
    // only ranked when it is the sole remaining target (trying to pot it early
    // is a foul, so never waste a branch on it before the group is cleared).
    std::vector<std::pair<double, int>> order;
    bool only8Now = (st.balls.size() == 1); // last ball in the run is the 8-ball
    for (int i = 0; i < (int)st.balls.size(); i++) {
        if (st.balls[i].index == 8 && !only8Now) continue;
        double sBest = 1e18;
        for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
            ChainGeo g = chainGeometry(st.cue, st.balls[i], p, ctx.table.pockets[p], st);
            if (g.ok && g.score < sBest) sBest = g.score;
        }
        order.push_back({sBest, i});
    }
    std::sort(order.begin(), order.end());
    int nTry = std::min(RUN_BALLS, (int)order.size());

    std::vector<RunChild> children;
    for (int k = 0; k < nTry; k++) {
        const BallEntry& ball = st.balls[order[k].second];
        for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
            if (PocketIsLocked(ctx.table, p)) continue;
            ChainGeo g = chainGeometry(st.cue, ball, p, ctx.table.pockets[p], st);
            if (!g.ok) continue;

            for (int v = 0; v < SPIN_VARIANTS_COUNT; v++) {
                HumanCandidate c;
                c.idx = ball.index;
                c.pocket = p;
                c.angle = g.angle;
                c.dist = g.distP;
                c.cueDist = g.cueDist;
                c.cutDot = g.cutDot;
                c.ballPos = ball.pos;
                c.ghost = g.ghost;
                c.score = g.score;
                // No hero shots: a planned step that is itself hard is exactly
                // the risky pot that can be missed and jam the run.
                if (c.score > RUN_MAX_SHOT_SCORE) continue;
                std::vector<double> powers;
                if (v == 0) {
                    powers = AutoPlay::BuildPowerGrid(g.power);
                } else {
                    double power = g.power * SPIN_VARIANTS[v].pMult;
                    powers.push_back(std::clamp(power,
                                                std::max(0.0, (double)AutoPlay::powerMin),
                                                (double)AutoPlay::powerMax));
                }

                for (double power : powers) {
                    if (budget <= 0) return;
                    if ((p == 1 || p == 4) && g.cutDot <= 0.70) continue; // Relaxed gate: allow angled side-pocket shots
                    if ((p == 1 || p == 4) && power > 400.0) continue; // Relaxed clamp: allow harder side-pocket shots
                    if (!PowerAllowedForCurrentPhase(power)) continue;
                    budget--;
                    c.power = power;
                    c.spin = Vec2d(SPIN_VARIANTS[v].x, SPIN_VARIANTS[v].y);
                    c.spinPreset = SPIN_VARIANTS[v].preset;
                    c.spinTag = SPIN_VARIANTS[v].tag;

                    SetEnglish(SPIN_VARIANTS[v].x, SPIN_VARIANTS[v].y);
                    SimulateShot(c);
                    int potIdx = chainShotPot(only8Now, ball.index, p);
                    if (potIdx < 0) continue;
                c.pocket = gPrediction->guiData.balls[potIdx].pocketIndex;

                // Count clustered balls before and after shot to reward cluster breakouts
                int clusteredBefore = 0;
                int clusteredAfter = 0;
                for (auto& b : st.balls) {
                    if (b.index != ball.index && (b.pos - ball.pos).square() < 25.0 * BALL_RADIUS * BALL_RADIUS) {
                        clusteredBefore++;
                    }
                }

                // Rebuild the child state from the sim's predicted positions.
                RunState ns;
                ns.cue = gPrediction->guiData.balls[0].predictedPosition;
                for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                    auto& b = gPrediction->guiData.balls[i];
                    if (!b.onTable) continue;
                    BallEntry e;
                    e.index = i;
                    e.pos = b.predictedPosition;
                    e.cls = b.classification;
                    ns.all.push_back(e);
                }
                for (auto& e : ns.all) {
                    if (ctx.table.myGroup != Ball::Classification::ANY && e.cls == ctx.table.myGroup) {
                        ns.balls.push_back(e);
                        // Check if ball remains trapped in a cluster in next state
                        for (auto& o : ns.all) {
                            if (o.index != e.index && (o.pos - e.pos).square() < 20.0 * BALL_RADIUS * BALL_RADIUS) {
                                clusteredAfter++;
                                break;
                            }
                        }
                    }
                }
                for (auto& e : ns.all) if (e.index == 8) { ns.balls.push_back(e); break; }

                double clusterBreakBonus = 0.0;
                if (clusteredBefore > clusteredAfter) {
                    clusterBreakBonus = (clusteredBefore - clusteredAfter) * 250.0;
                }

                int nextIdx = -1;
                double shape = 0.0;
                if (!ns.balls.empty()) shape = shapeForState(ns, nextIdx);
                c.shapeScore = shape;
                c.followTarget = nextIdx;
                c.hasFollowUp = (shape < LOOKAHEAD_MAX_SCORE);
                c.finalScore = c.score + SHAPE_WEIGHT * shape +
                               PowerPenalty(power, g.power) - clusterBreakBonus;
                c.valid = true;
                c.potted = true;

                    std::vector<HumanCandidate> childChain = chain;
                    childChain.push_back(c);
                    children.push_back({ns, childChain, accum + c.finalScore});
                }
            }
        }
    }

    if (children.empty()) {
        // Dead end: record the deepest partial run reached so far.
        if (!best.complete && (int)chain.size() > best.depth) {
            best.chain = chain;
            best.accum = accum;
            best.depth = (int)chain.size();
            best.complete = false;
        }
        return;
    }

    std::sort(children.begin(), children.end(),
              [](const RunChild& a, const RunChild& b) { return a.accum < b.accum; });
    int nb = std::min(RUN_BEAM, (int)children.size());
    for (int i = 0; i < nb; i++) {
        planExpand(children[i].st, children[i].chain, children[i].accum, best, budget);
        if (budget <= 0) break;
    }
}

// Build the run plan from the current live table state.
static RunPlan planRunOut() {
    RunPlan plan;
    if (ctx.table.myGroup == Ball::Classification::ANY) return plan;
    RunState st;
    st.cue = ctx.table.cuePos;
    st.all = ctx.table.allBalls;
    for (auto& e : st.all) {
        if (e.cls == ctx.table.myGroup) st.balls.push_back(e);
    }
    for (auto& e : st.all) if (e.index == 8) { st.balls.push_back(e); break; }
    if (st.balls.empty()) return plan;
    int budget = RUN_BUDGET;
    std::vector<HumanCandidate> chain;
    planExpand(st, chain, 0.0, plan, budget);
    LOGI("[HumanScan] RunOut balls=%d sims=%d depth=%d complete=%d accum=%.0f",
         (int)st.balls.size(), RUN_BUDGET - budget, plan.depth, plan.complete ? 1 : 0, plan.accum);
    return plan;
}

// ---------------------------------------------------------------------
// 6. SimulateCandidate — three phases:
//    Phase A: pot the ranked candidates (1 sim each), keep potting + safe pools.
//    Phase B: refine the easiest potting shots for shape, AND try to convert
//             the best safe shots into real pots via the spin/power grid.
//             Picks the best pot by finalScore (shot ease + shape).
// ---------------------------------------------------------------------
SimStatus SimulateCandidate() {
    ctx.best = HumanCandidate{};
    ctx.safeShot = HumanCandidate{};
    ctx.potCands = 0;
    ctx.safeCands = 0;
    ctx.invalidCands = 0;

    double sw = ctx.table.onLastGroupBall ? SHAPE_WEIGHT_LAST : SHAPE_WEIGHT;

    SetEnglish(0.0, 0.0);
    std::vector<HumanCandidate> potList;
    std::vector<HumanCandidate> safeList;

    for (auto& c : ctx.candidates) {
        if (ctx.simsUsed > SIM_BUDGET) break;
        const double idealPower = CalculateRequiredPower(c.cueDist, c.dist, c.cutDot);
        const auto powerGrid = AutoPlay::BuildPowerGrid(idealPower);
        for (double power : powerGrid) {
            if (ctx.simsUsed > SIM_BUDGET) break;
            if (!PowerAllowedForCurrentPhase(power)) continue;
            HumanCandidate trial = c;
            trial.power = power;
            trial.simulated = true;
            SetEnglish(0.0, 0.0);
            SimulateShot(trial);

            SimStatus st = evaluateSimulation(trial);
            if (st == SimStatus::INVALID && (!gPrediction->guiData.balls[0].onTable || c.dist < 50.0 || c.cutDot > 0.85)) {
                // Neutral spin caused a cue ball scratch or follow-in on straight/pocket-lip shot.
                // Try spin variants (draw/bottom spin) to stop cue ball and prevent the scratch!
                for (int v = 0; v < SPIN_VARIANTS_COUNT; v++) {
                    if (SPIN_VARIANTS[v].y >= 0.0 && SPIN_VARIANTS[v].x == 0.0) continue; // skip neutral/top spin
                    if (ctx.simsUsed > SIM_BUDGET) break;
                    HumanCandidate spinTrial = trial;
                    SetEnglish(SPIN_VARIANTS[v].x, SPIN_VARIANTS[v].y);
                    SimulateShot(spinTrial);
                    if (evaluateSimulation(spinTrial) == SimStatus::POTTED) {
                        spinTrial.spin = Vec2d(SPIN_VARIANTS[v].x, SPIN_VARIANTS[v].y);
                        spinTrial.spinPreset = SPIN_VARIANTS[v].preset;
                        spinTrial.spinTag = SPIN_VARIANTS[v].tag;
                        LookAheadOneShot(spinTrial);
                        spinTrial.finalScore = spinTrial.score + sw * spinTrial.shapeScore + PowerPenalty(power, idealPower);
                        potList.push_back(spinTrial);
                        ctx.potCands++;
                        st = SimStatus::POTTED;
                        break;
                    }
                }
                SetEnglish(0.0, 0.0);
            }

            if (st == SimStatus::POTTED) {
                ctx.potCands++;
                LookAheadOneShot(trial);
                trial.finalScore = trial.score + sw * trial.shapeScore +
                                   PowerPenalty(power, idealPower);
                potList.push_back(trial);
            } else if (st == SimStatus::SAFE) {
                ctx.safeCands++;
                int nextIdx = -1;
                trial.shapeScore = ComputeShape(nextIdx);
                trial.followTarget = nextIdx;
                trial.hasFollowUp = (trial.shapeScore < LOOKAHEAD_MAX_SCORE);
                trial.finalScore = trial.score + PowerPenalty(power, idealPower);
                safeList.push_back(trial);
            } else {
                ctx.invalidCands++;
            }
        }
    }

    // Phase R: full-table run planning. When we own a decided group and at
    // least one pot exists, search a multi-shot chain (group balls -> 8-ball).
    // Every planned step is sim-validated and picks the spin that keeps the
    // NEXT ball pottable, so the cue is always left in play. A complete (or
    // 2+ deep) verified run beats a single-shot pot — this is the "prepare
    // the next ball, chain to the end of the game" behaviour.
    if (!ctx.table.only8BallLeft &&
        ctx.table.myGroup != Ball::Classification::ANY &&
        !ctx.table.eightBallMustBank &&
        !potList.empty()) {
        RunPlan plan = planRunOut();
        bool usePlan = plan.complete || plan.depth >= 2;
        if (usePlan && !plan.chain.empty()) {
            HumanCandidate first = plan.chain[0];
            // Never force a drastically harder opening shot just to chase a
            // position: if the easiest plain pot is clearly easier, play that
            // one instead and let Phase B position it.
            double easyScore = 1e18;
            for (const auto& pot : potList) easyScore = std::min(easyScore, pot.score);
            bool openingOk = (first.score <= easyScore * 1.35 + 60.0);
            if (openingOk) {
                ctx.best = first;
                LOGI("[HumanScan] Run plan depth=%d %s accum=%.0f sims=%d",
                     plan.depth, plan.complete ? "COMPLETE" : "PARTIAL", plan.accum, ctx.simsUsed);
                return SimStatus::POTTED;
            }
            LOGI("[HumanScan] Run plan skipped (opening %.0f vs easy %.0f)",
                 first.score, easyScore);
        }
    }

    // Phase B: refine the easiest potting shots AND the easiest safe shots.
    std::vector<HumanCandidate> refine;
    std::sort(potList.begin(), potList.end(),
              [](const HumanCandidate& a, const HumanCandidate& b) {
                  return a.score < b.score;
              });
    std::sort(safeList.begin(), safeList.end(),
              [](const HumanCandidate& a, const HumanCandidate& b) {
                  return a.score < b.score;
              });
    int np = std::min((int)potList.size(), MAX_REFINE);
    for (int i = 0; i < np; i++) refine.push_back(potList[i]);
    int ns = std::min((int)safeList.size(), MAX_SAFE_REFINE);
    for (int i = 0; i < ns; i++) refine.push_back(safeList[i]);

    std::vector<HumanCandidate> refinedPots;
    std::vector<HumanCandidate> refinedSafes;
    for (auto& c : refine) {
        if (ctx.simsUsed > SIM_BUDGET) break;
        if (RefineWithSpin(c)) refinedPots.push_back(c);
        else refinedSafes.push_back(c);
    }

    // Best potting shot (Phase A pots + safe shots converted to pots).
    for (auto& c : potList) {
        if (!ctx.best.valid || c.finalScore < ctx.best.finalScore) ctx.best = c;
    }
    for (auto& c : refinedPots) {
        if (!ctx.best.valid || c.finalScore < ctx.best.finalScore) ctx.best = c;
    }

    // 8-Ball endgame: never pot the 8-ball with a feather stroke. If the
    // winning pot is too soft, re-sim it at a firm power and keep it only if
    // it still stays clean.
    if (ctx.best.valid && ctx.table.only8BallLeft && ctx.best.power < EIGHTBALL_MIN_POWER) {
        HumanCandidate t = ctx.best;
        t.power = EIGHTBALL_MIN_POWER;
        SetEnglish(t.spin.x, t.spin.y);
        SimulateShot(t);
        if (evaluateSimulation(t) == SimStatus::POTTED) ctx.best = t;
    }

    if (ctx.best.valid && ctx.best.potted) return SimStatus::POTTED;

    // 8-Ball endgame: if no exact ghost-ball pot was found, fine-sweep the
    // 8-ball pocket lines. A thin cut can miss by a hair in the sim and would
    // otherwise degenerate into endless "touch the 8" safes instead of the win.
    if (ctx.table.only8BallLeft) {
        const double swStep = 0.004;     // ~0.23 deg
        const int swN = 8;               // +/- 0.032 rad around the aimed angle
        int nCand = std::min((int)ctx.candidates.size(), 6);
        for (int ci = 0; ci < nCand && !ctx.best.valid; ci++) {
            if (ctx.simsUsed > SIM_BUDGET) break;
            HumanCandidate base = ctx.candidates[ci];
            for (int k = -swN; k <= swN; k++) {
                if (ctx.simsUsed > SIM_BUDGET) break;
                HumanCandidate t = base;
                t.angle = base.angle + k * swStep;
                if (t.angle < 0) t.angle += 2.0 * M_PI;
                if (t.angle >= 2.0 * M_PI) t.angle -= 2.0 * M_PI;
                t.power = std::max(t.power, EIGHTBALL_MIN_POWER);
                t.spin = Vec2d(0.0, 0.0);
                t.spinPreset = AutoPlay::SPIN_CENTER;
                SetEnglish(0.0, 0.0);
                SimulateShot(t);
                if (evaluateSimulation(t) == SimStatus::POTTED) {
                    ctx.best = t;
                    LOGI("[HumanScan] 8-ball swept pot pocket=%d angle=%.4f power=%.0f sims=%d",
                         t.pocket, t.angle, t.power, ctx.simsUsed);
                    break;
                }
            }
        }
        if (ctx.best.valid) return SimStatus::POTTED;
    }

    // 8-Ball endgame: if the 8-ball cannot be potted, still play it with a
    // controlled, sim-validated stroke instead of a feather tap that the game
    // registers as an invalid shot. A 300-power blast risks a scratch (loss),
    // so floor at VALID_FLOOR — real stroke, low scratch risk. Freeze if it fouls.
    if (ctx.table.only8BallLeft && !safeList.empty()) {
        LOGI("[HumanScan] 8-ball fallback safe (cands=%d pot=%d safe=%d bad=%d sims=%d)",
             (int)ctx.candidates.size(), ctx.potCands, ctx.safeCands, ctx.invalidCands, ctx.simsUsed);
        HumanCandidate t = safeList[0];
        t.power = std::max(t.power, EIGHTBALL_VALID_FLOOR);
        t.spin = Vec2d(0.0, 0.0);
        t.spinPreset = AutoPlay::SPIN_CENTER;
        t.spinTag = "8";
        SetEnglish(0.0, 0.0);
        SimulateShot(t);
        SimStatus st2 = evaluateSimulation(t);
        if (st2 != SimStatus::INVALID) {
            t.valid = true;
            t.potted = (st2 == SimStatus::POTTED);
            if (st2 == SimStatus::POTTED) { ctx.best = t; return SimStatus::POTTED; }
            int nextIdx = -1;
            t.shapeScore = ComputeShape(nextIdx);
            t.followTarget = nextIdx;
            t.hasFollowUp = (t.shapeScore < LOOKAHEAD_MAX_SCORE);
            // Play the controlled 8-ball contact even without a follow-up:
            // a real legal stroke beats freezing into a timeout loss.
            ctx.safeShot = t;
            return SimStatus::SAFE;
        }
        ctx.safeShot = HumanCandidate{};
        return SimStatus::INVALID;
    }

    // Best legal safety (nothing to pot): prefer the leave that keeps a next
    // shot playable (so the cue never jams itself again), then the easiest hit.
    auto betterSafe = [](const HumanCandidate& a, const HumanCandidate& b) {
        if (!b.valid) return true;
        bool aUp = a.hasFollowUp, bUp = b.hasFollowUp;
        if (aUp != bUp) return aUp;
        if (a.shapeScore != b.shapeScore) return a.shapeScore < b.shapeScore;
        return a.score < b.score;
    };
    if (!safeList.empty()) {
        ctx.safeShot = safeList[0];
        for (size_t i = 1; i < safeList.size(); i++) {
            if (betterSafe(safeList[i], ctx.safeShot)) ctx.safeShot = safeList[i];
        }
    }
    for (auto& c : refinedSafes) {
        if (betterSafe(c, ctx.safeShot)) ctx.safeShot = c;
    }

    if (ctx.safeShot.valid) return SimStatus::SAFE;
    return SimStatus::INVALID;
}

// ---------------------------------------------------------------------
// 8. CommitShot — lock the decision for the current shot.
// ---------------------------------------------------------------------
void CommitShot(const HumanCandidate& c) {
    ctx.shot = CommittedShot{};
    ctx.shot.locked = true;
    ctx.shot.idx = c.idx;
    ctx.shot.pocket = c.pocket;
    ctx.shot.angle = c.angle;
    ctx.shot.power = c.power;
    ctx.shot.spin = c.spin;
    ctx.shot.spinPreset = c.spinPreset;
    ctx.shot.spinTag = c.spinTag;
    ctx.shot.followTarget = c.followTarget;
    ctx.shot.shapeScore = c.shapeScore;
    ctx.shot.hasFollowUp = c.hasFollowUp;
    ctx.best = c;
    ctx.turnAnalyzed = true;
    ctx.awaitingSettlement = true;
    // The first shot after a break has its own power floor. Once we've locked
    // *any* committed shot, the floor no longer applies to subsequent shots.
    ctx.postBreakShot = false;
}

// ---------------------------------------------------------------------
// 9. ExecuteShot — reuse the existing human aiming/execution pipeline,
//    applying the chosen spin (english) before the shot so the simulated
//    spin matches the fired shot.
// ---------------------------------------------------------------------
void ExecuteShot() {
    const CommittedShot& s = ctx.shot;
    AutoPlay::g_CurrentCandidate = {s.idx, s.angle, 0.0, s.pocket, s.power, 0.0};
    AutoPlay::pendingShotAngle = s.angle;
    AutoPlay::pendingShotPower = s.power;
    if (s.spinPreset >= 0) {
        AutoPlay::spinPreset = (AutoPlay::SpinPreset)s.spinPreset;
    }
    AutoPlay::forcedShotSpin = s.spin;
    AutoPlay::forcedShotSpinActive = true;
    SetEnglish(s.spin.x, s.spin.y);
    AutoPlay::Shoot(s.angle, s.power);
}

// ---------------------------------------------------------------------
// BreakPhase — natural opening break with scratch rejection.
// ---------------------------------------------------------------------
bool BreakPhase() {
    Table table = sharedGameManager.mTable;
    if (!table) return false;
    auto& balls = table.mBalls();
    if (!balls) return false;

    Point2D cue = ctx.table.cuePos;

    // Apex = the racked ball closest to the cue. At break time the only balls
    // on the table are the rack + the cue, so the nearest racked ball is the
    // front ball of the rack.
    Point2D apex;
    bool haveApex = false;
    double bestDist = 1e18;
    for (int i = 1; i < (int)balls.Count; i++) {
        Ball b = balls[i];
        if (!b || !b.isOnTable()) continue;
        Point2D p = b.position();
        if (p.x < 60.0 || p.x > 125.0) continue;
        Point2D dv = p - cue;
        double dsq = dv.square();
        if (dsq < bestDist) {
            bestDist = dsq;
            apex = p;
            haveApex = true;
        }
    }
    if (!haveApex) {
        ctx.breakDone = true;
        return false;
    }

    Point2D d = Normalize2D(apex - cue);
    double base = atan2(d.y, d.x);
    if (base < 0) base += 2.0 * M_PI;

    // Search a small break-power ladder instead of assuming that maximum
    // power is always the best rack hit. The lower levels also reduce cue-ball
    // scratch risk when the rack is aligned with a corner pocket.
    const double breakPowers[] = {
        400.0, 460.0, 520.0, 580.0
    };
    const int breakPowerCount = 4;
    const double window = 0.22;   // radians each side of dead-center (~25 deg)
    const double step = 0.011;    // ~41 candidate angles across the rack face

    // Draw/backspin keeps cue ball centered and avoids scratch on break
    const double breakSpins[3][2] = {
        { 0.00, -0.50 }, // draw / backspin (ideal)
        { 0.00,  0.00 }, // center
        { 0.00, -0.70 }, // heavy draw
    };
    const int breakSpinCount = 3;

    double bestScore = -1e18;
    double bestAngle = base;
    double bestPower = breakPowers[breakPowerCount - 1];
    int bestPots = -1;
    bool bestScratch = false;
    double bestSpinX = 0.0, bestSpinY = 0.0;

    for (double off = -window; off <= window + 1e-9; off += step) {
        double angle = base + off;
        if (angle < 0) angle += 2.0 * M_PI;
        if (angle >= 2.0 * M_PI) angle -= 2.0 * M_PI;

        for (int si = 0; si < breakSpinCount; si++) {
            for (int pi = 0; pi < breakPowerCount; pi++) {
                const double power = breakPowers[pi];
                SetEnglish(breakSpins[si][0], breakSpins[si][1]);

                ctx.simsUsed++;
                gPrediction->forceFullSimulation = true;
                gPrediction->determineShotResult(true, angle, power, sharedGameManager.getShotSpin());
                gPrediction->forceFullSimulation = false;

                auto& gd = gPrediction->guiData;
                if (gd.ballsCount <= 0) continue;
                bool scratch = !gd.balls[0].onTable;

            int pots = 0;
            for (int i = 1; i < gd.ballsCount; i++) {
                if (gd.balls[i].originalOnTable && !gd.balls[i].onTable) pots++;
            }

            double cx = 0.0, cy = 0.0;
            int n = 0;
            for (int i = 1; i < gd.ballsCount; i++) {
                if (gd.balls[i].onTable) {
                    cx += gd.balls[i].predictedPosition.x;
                    cy += gd.balls[i].predictedPosition.y;
                    n++;
                }
            }
            double spread = 0.0;
            if (n > 0) {
                cx /= n;
                cy /= n;
                for (int i = 1; i < gd.ballsCount; i++) {
                    if (gd.balls[i].onTable) {
                        double qx = gd.balls[i].predictedPosition.x - cx;
                        double qy = gd.balls[i].predictedPosition.y - cy;
                        spread += sqrt(qx * qx + qy * qy);
                    }
                }
                spread /= n;
            }

                double score = pots * 3000.0 + spread * 3.0 -
                               (scratch ? 12000.0 : 0.0) - power * 0.08;
                if (score > bestScore) {
                    bestScore = score;
                    bestAngle = angle;
                    bestPower = power;
                    bestPots = pots;
                    bestScratch = scratch;
                    bestSpinX = breakSpins[si][0];
                    bestSpinY = breakSpins[si][1];
                }
            }
        }
    }
    SetEnglish(bestSpinX, bestSpinY);

    // Never commit a break that the predictor already marks as a scratch.
    // Leaving the decision pending is safer than handing the opponent an
    // avoidable foul when every tested break is unsafe.
    if (bestScratch || bestPots < 0) {
        // Fallback: execute a safe, controlled center break at 460 power with backspin
        bestAngle = base;
        bestPower = 460.0;
        bestSpinX = 0.0;
        bestSpinY = -0.50;
        bestScratch = false;
        LOGI("[HumanScan] Break fallback applied: base angle=%.4f power=%.0f", bestAngle, bestPower);
    }

    // The sweep leaves guiData at its last tested angle. Re-run the selected
    // break so prediction lines, pocket highlights, and the fired stroke all
    // describe the same angle/power/spin.
    SetEnglish(bestSpinX, bestSpinY);
    gPrediction->forceFullSimulation = true;
    gPrediction->determineShotResult(true, bestAngle, bestPower,
                                     sharedGameManager.getShotSpin());
    gPrediction->forceFullSimulation = false;

    HumanCandidate c;
    c.idx = 1;
    c.pocket = -1;
    c.angle = bestAngle;
    c.power = bestPower;
    c.score = bestScore;
    c.finalScore = bestScore;
    c.spin = Vec2d(bestSpinX, bestSpinY);
    c.spinPreset = AutoPlay::SPIN_CENTER;
    c.spinTag = "bx";

    ctx.breakBeforeBalls = ctx.table.allBalls;
    ctx.breakShotActive = true;
    ctx.breakDone = true;
    CommitShot(c);
    ExecuteShot();
    LOGI("[HumanScan] Break angle=%.3f power=%.0f pots=%d scratch=%d spin=(%.2f,%.2f) score=%.0f sims=%d",
         bestAngle, bestPower, bestPots, bestScratch, bestSpinX, bestSpinY, bestScore, ctx.simsUsed);
    return true;
}

// ---------------------------------------------------------------------
// TrySafeTap — last resort when nothing can be potted: a firm, sim-validated
// breakout tap on a legal ball (prefer clustered balls to free them).
// NEVER plays a shot the sim flagged as a foul — freezing is better than
// touching an opponent's ball. 1 sim per attempt.
// ---------------------------------------------------------------------
bool TrySafeTap() {
    static double lastDbg = 0.0;
    auto dbg = [&](const char* reason) {
        double n = AutoPlay::nowSec();
        if (n - lastDbg > 2.0) {
            lastDbg = n;
            LOGI("[HumanScan] tap %s", reason);
        }
    };

    auto& all = ctx.table.allBalls;
    if (all.empty()) { dbg("no-balls"); return false; }
    if (ctx.simsUsed > SIM_BUDGET) { dbg("sim-budget"); return false; }

    const Ball::Classification group = ctx.table.myGroup;
    const bool only8 = ctx.table.only8BallLeft;
    Point2D cue = ctx.table.cuePos;

    auto legal = [&](const BallEntry& t) {
        if (only8) return t.index == 8;
        if (group == Ball::Classification::ANY) return t.cls != Ball::Classification::EIGHT_BALL;
        return t.cls == group;
    };

    auto hasNeighbor = [&](const BallEntry& t) {
        const double nearSq = 4.0 * BALL_RADIUS * (4.0 * BALL_RADIUS);
        for (auto& o : all) {
            if (o.index == t.index) continue;
            if ((o.pos - t.pos).square() < nearSq) return true;
        }
        return false;
    };

    std::vector<BallEntry> sorted;
    for (auto& t : all) {
        if (!legal(t)) continue;
        sorted.push_back(t);
    }
    if (sorted.empty()) { dbg("no-legal-target"); return false; }

    // Prefer clustered targets (a firm hit breaks them open), then nearest.
    std::sort(sorted.begin(), sorted.end(),
              [&](const BallEntry& a, const BallEntry& b) {
                  bool ca = hasNeighbor(a), cb = hasNeighbor(b);
                  if (ca != cb) return ca > cb;
                  return (a.pos - cue).square() < (b.pos - cue).square();
              });

    // Breakout: firm power + every spin direction so the cue can curl around
    // obstacles and split clusters open. Only play a tap that actually leaves
    // a playable next shot — otherwise the tap itself becomes the "stupid
    // shot" that jams us again, so freeze instead.
    const int maxTargets = 4;
    const int maxSims = 96;
    int simCount = 0;
    HumanCandidate bestTap;
    HumanCandidate fallbackTap;
    double bestLeave = 1e18;
    double bestFallbackScore = 1e18;

    for (int si = 0; si < maxTargets && si < (int)sorted.size(); si++) {
        const BallEntry& t = sorted[si];
        Point2D d = t.pos - cue;
        double dist = sqrt(d.square());
        if (dist < 0.1) continue;
        double angle = atan2(d.y, d.x);
        if (angle < 0) angle += 2.0 * M_PI;

        bool isClustered = hasNeighbor(t);
        double basePower = CalculateRequiredPower(dist) * (isClustered ? 1.85 : 1.15);
        if (isClustered && basePower < 380.0) basePower = 380.0;
        if (basePower > (double)AutoPlay::powerMax) basePower = (double)AutoPlay::powerMax;
        if (basePower < (double)AutoPlay::powerMin) basePower = (double)AutoPlay::powerMin;

        for (int v = 0; v < SPIN_VARIANTS_COUNT && simCount < maxSims; v++) {
            std::vector<double> powers;
            if (isClustered) {
                powers = { basePower, 440.0, 560.0 };
            } else if (v == 0) {
                powers = AutoPlay::BuildPowerGrid(basePower);
            } else {
                double power = basePower * SPIN_VARIANTS[v].pMult;
                powers.push_back(std::clamp(power,
                                            std::max(0.0, (double)AutoPlay::powerMin),
                                            (double)AutoPlay::powerMax));
            }

            for (double power : powers) {
                if (simCount >= maxSims) break;
                if (!PowerAllowedForCurrentPhase(power)) continue;
                ctx.simsUsed++;
                simCount++;
                SetEnglish(SPIN_VARIANTS[v].x, SPIN_VARIANTS[v].y);
                Candidate cand{t.index, angle, 1e18, -1, power, dist};
                gPrediction->forceFullSimulation = true;
                gPrediction->determineShotResult(true, angle, power, sharedGameManager.getShotSpin(), cand);
                gPrediction->forceFullSimulation = false;

                auto& gd = gPrediction->guiData;
                bool scratch = !gd.balls[0].onTable;
                auto firstHit = gd.collision.firstHitBall;
                bool hit = false;
                if (firstHit) {
                    if (only8) hit = (firstHit->index == 8);
                    else if (group == Ball::Classification::ANY) hit = (firstHit->classification != Ball::Classification::EIGHT_BALL);
                    else hit = (firstHit->classification == group);
                }
                if (!hit) { dbg(scratch ? "miss-scratch" : "miss-no-hit"); continue; }
                if (scratch) { dbg("scratch"); continue; }

                bool foul = false;
                for (int i = 1; i < gd.ballsCount; i++) {
                    auto& b = gd.balls[i];
                    if (!b.originalOnTable || b.onTable) continue;
                    bool ballLegal = only8 ? (i == 8)
                                  : (group == Ball::Classification::ANY)
                                      ? (b.classification != Ball::Classification::EIGHT_BALL)
                                      : (b.classification == group);
                    if (!ballLegal) { foul = true; break; }
                }
                if (foul) { dbg("tap-foul"); continue; }
                // 8-ball must go into the nominated pocket; a wrong-pocket 8
                // pot is a loss, not a safe.
                if (only8 && gd.ballsCount > 8 &&
                    gd.balls[8].originalOnTable && !gd.balls[8].onTable) {
                    const int nom = ctx.table.nominatedPocket;
                    if (nom >= 0 && nom < 6 && gd.balls[8].pocketIndex != nom) {
                        dbg("8-wrong-pocket");
                        continue;
                    }
                }

                int nextIdx = -1;
                double leave = ComputeShape(nextIdx);
                if (leave < bestLeave) {
                    bestLeave = leave;
                    bestTap = HumanCandidate{};
                    bestTap.idx = t.index;
                    bestTap.angle = angle;
                    bestTap.power = power;
                    bestTap.valid = true;
                    bestTap.spin = Vec2d(SPIN_VARIANTS[v].x, SPIN_VARIANTS[v].y);
                    bestTap.spinPreset = SPIN_VARIANTS[v].preset;
                    bestTap.spinTag = SPIN_VARIANTS[v].tag;
                    bestTap.followTarget = nextIdx;
                    bestTap.shapeScore = leave;
                    bestTap.hasFollowUp = (leave < LOOKAHEAD_MAX_SCORE);
                }
                // Prefer the legal contact that opens the best next route,
                // then the least forceful stroke when routes are equivalent.
                double fallbackScore = (leave < SHAPE_BAD ? leave : 900.0) +
                                       PowerPenalty(power, basePower);
                if (!fallbackTap.valid || fallbackScore < bestFallbackScore) {
                    bestFallbackScore = fallbackScore;
                    fallbackTap = HumanCandidate{};
                    fallbackTap.idx = t.index;
                    fallbackTap.angle = angle;
                    fallbackTap.power = power;
                    fallbackTap.valid = true;
                    fallbackTap.spin = Vec2d(SPIN_VARIANTS[v].x, SPIN_VARIANTS[v].y);
                    fallbackTap.spinPreset = SPIN_VARIANTS[v].preset;
                    fallbackTap.spinTag = SPIN_VARIANTS[v].tag;
                    fallbackTap.followTarget = nextIdx;
                    fallbackTap.shapeScore = leave;
                    fallbackTap.hasFollowUp = (leave < LOOKAHEAD_MAX_SCORE);
                }
            }
        }
    }

    // Play the tap if it leaves a playable next shot. Otherwise fall back
    // to any legal contact — a timeout is worse than a bad safety.
    if (bestTap.valid && bestTap.hasFollowUp) {
        SetEnglish(bestTap.spin.x, bestTap.spin.y);
        CommitShot(bestTap);
        ExecuteShot();
        LOGI("[HumanScan] Tap idx=%d power=%.0f spin=%s leave=%.0f sims=%d",
             bestTap.idx, bestTap.power, bestTap.spinTag, bestTap.shapeScore, ctx.simsUsed);
        return true;
    }

    if (fallbackTap.valid) {
        SetEnglish(fallbackTap.spin.x, fallbackTap.spin.y);
        CommitShot(fallbackTap);
        ExecuteShot();
        LOGI("[HumanScan] LastResort idx=%d power=%.0f spin=%s sims=%d",
             fallbackTap.idx, fallbackTap.power, fallbackTap.spinTag, ctx.simsUsed);
        return true;
    }

    dbg("no-open-tap");
    return false;
}

// ---------------------------------------------------------------------
// RunIfReady — guarded entry point, called once per frame from Update
// while the human AI owns the turn. O(1) unless a decision is due.
// Returns false only for 9-Ball (out of v1 scope) so the legacy flow
// keeps running; otherwise the human AI owns the frame.
// ---------------------------------------------------------------------
bool RunIfReady() {
    static double lastDbg = 0.0;
    auto dbg = [&](const char* tag) {
        double n = AutoPlay::nowSec();
        if (n - lastDbg > 1.0) {
            lastDbg = n;
            LOGI("[HumanScan] RIF %s (sims=%d)", tag, ctx.simsUsed);
        }
    };

    if (!sharedGameManager) return true;
    if (sharedGameManager.is9BallGame()) return false;

    if (AutoPlay::AreBallsMoving()) { dbg("balls-moving"); return true; }

    if (ctx.awaitingSettlement) {
        if (ctx.breakShotActive) {
            // A break has its own result contract. Never let a stale plan run
            // before checking whether the rack actually changed.
            const bool snapshotOk = AnalyzeTable();
            const bool rackChanged = snapshotOk &&
                                     BreakStateChanged(ctx.breakBeforeBalls, ctx.table);
            const bool cueSafe = snapshotOk && ctx.table.cueOnTable;
            ctx.breakShotActive = false;
            ctx.breakBeforeBalls.clear();
            GameMaster::ResetPlan();
            ctx.awaitingSettlement = false;
            ctx.turnAnalyzed = false;

            if (!snapshotOk || !cueSafe || !rackChanged) {
                ctx.breakDone = false;
                ctx.postBreakShot = false;
                ctx.turnAnalyzed = true;
                ctx.lastFailTime = AutoPlay::nowSec();
                ctx.lastDecisionCuePos = ctx.table.cuePos;
                LOGI("[HumanScan] Break FAILED safe=%d rackChanged=%d",
                     cueSafe ? 1 : 0, rackChanged ? 1 : 0);
                return true;
            }

            LOGI("[HumanScan] Break SETTLED rackChanged=1; rebuilding from live table");
            ctx.postBreakShot = true;
        }

        // GameMaster must reconcile the previous shot before the snapshot is
        // cleared. If it cannot continue, fall back to the normal planner.
        if (!ctx.breakShotActive && GameMaster::g_plan.initialized && !GameMaster::g_plan.complete) {
            if (GameMaster::RunMasterPlan()) return true;
        }
        GameMaster::ResetPlan();
        ctx.awaitingSettlement = false;
        ctx.turnAnalyzed = false;
        dbg("settled");
    }

    if (ctx.turnAnalyzed) {
        double moved = (ctx.table.cuePos - ctx.lastDecisionCuePos).square();
        if (moved < CUE_MOVE_EPS_SQ && (AutoPlay::nowSec() - ctx.lastFailTime) < RETRY_WINDOW) {
            dbg("waiting"); return true;
        }
        ctx.turnAnalyzed = false;
    }

    if (!AnalyzeTable()) { dbg("analyze-fail"); return true; }

    if (!ctx.table.cueOnTable) {
        dbg("ball-in-hand");
        AutoPlay::PlaceCueBall();
        ctx.turnAnalyzed = false; // re-enter every frame; placement advances the drag
        return true;
    }

    if (ctx.table.isBreak && !ctx.breakDone) {
        dbg("break");
        if (!BreakPhase()) {
            ctx.turnAnalyzed = true;
            ctx.lastFailTime = AutoPlay::nowSec();
            ctx.lastDecisionCuePos = ctx.table.cuePos;
        }
        return true;
    }

    // --- GAME MASTER PATH: post-break, plan full clearance with easy direct shots ---
    ChooseGroup();

    bool groupBallRemains = false;
    if (ctx.table.myGroup != Ball::Classification::ANY &&
        ctx.table.myGroup != Ball::Classification::EIGHT_BALL) {
        for (const auto& ball : ctx.table.allBalls) {
            if (ball.cls == ctx.table.myGroup) {
                groupBallRemains = true;
                break;
            }
        }
    }
    const bool planHas8Step =
        GameMaster::g_plan.initialized &&
        GameMaster::g_plan.currentIdx < (int)GameMaster::g_plan.ballOrder.size() &&
        GameMaster::g_plan.ballOrder[GameMaster::g_plan.currentIdx] == 8 &&
        !groupBallRemains &&
        !ctx.table.eightBallMustBank;
    if (ctx.table.only8BallLeft && !planHas8Step) GameMaster::ResetPlan();

    const bool canStartGroupPlan =
        !ctx.table.only8BallLeft &&
        ctx.table.myGroup != Ball::Classification::ANY &&
        ctx.table.myGroup != Ball::Classification::EIGHT_BALL;
    if ((canStartGroupPlan || planHas8Step) && !GameMaster::g_plan.complete) {

        if (canStartGroupPlan && !GameMaster::g_plan.initialized) {
            GameMaster::SelectGroup(ctx.table);
            GameMaster::g_plan = GameMaster::BuildPlan(ctx.table);
            dbg("gm-init");
        }

        if (GameMaster::g_plan.initialized && !GameMaster::g_plan.ballOrder.empty()) {
            if (GameMaster::RunMasterPlan()) return true;
            GameMaster::ResetPlan();
        }
    }

    // --- FALLBACK: normal HumanScan pipeline ---
    const bool bankOnly = ctx.table.only8BallLeft && ctx.table.eightBallMustBank;
    if (bankOnly) {
        CreateBankCandidates();
    } else {
        CreateCandidates();
        EasyFilter();
        // Combinations and 1-rail cue banks are added AFTER EasyFilter on
        // purpose: a bank's straight cue->ghost line is blocked by design, so
        // EasyFilter would throw every one of them away. Their score offsets
        // keep them behind any available direct shot in RankCandidates.
        AppendIndirectCandidates();
    }
    RankCandidates();

    if (ctx.candidates.empty()) {
        dbg("no-candidates");
        if (!bankOnly && TrySafeTap()) return true;
        ctx.turnAnalyzed = true;
        ctx.lastFailTime = AutoPlay::nowSec();
        ctx.lastDecisionCuePos = ctx.table.cuePos;
        return true;
    }

    SimStatus st = SimulateCandidate();
    if (st == SimStatus::POTTED) {
        CommitShot(ctx.best);
        ExecuteShot();
        LOGI("[HumanScan] Shot idx=%d pocket=%d power=%.0f spin=%s shape=%.0f next=%d sims=%d",
             ctx.shot.idx, ctx.shot.pocket, ctx.shot.power, ctx.shot.spinTag,
             ctx.shot.shapeScore, ctx.shot.followTarget, ctx.simsUsed);
    } else if (st == SimStatus::SAFE) {
        CommitShot(ctx.safeShot);
        ExecuteShot();
        LOGI("[HumanScan] Safe idx=%d pocket=%d power=%.0f sims=%d",
             ctx.shot.idx, ctx.shot.pocket, ctx.shot.power, ctx.simsUsed);
    } else {
        if (!bankOnly && TrySafeTap()) return true;
        ctx.turnAnalyzed = true;
        ctx.lastFailTime = AutoPlay::nowSec();
        ctx.lastDecisionCuePos = ctx.table.cuePos;
        LOGI("[HumanScan] No legal shot (sims=%d pot=%d safe=%d bad=%d)",
             ctx.simsUsed, ctx.potCands, ctx.safeCands, ctx.invalidCands);
    }
    return true;
}

// ---------------------------------------------------------------------
// ResetTurn — fresh decision state at the start of the player's turn.
// ---------------------------------------------------------------------
void ResetTurn() {
    GameMaster::ResetPlan();
    ctx.turnAnalyzed = false;
    ctx.awaitingSettlement = false;
    ctx.breakDone = false;
    ctx.breakShotActive = false;
    ctx.postBreakShot = false;
    ctx.breakBeforeBalls.clear();
    ctx.lastFailTime = -1000.0;
    ctx.lastDecisionCuePos = Point2D(-1000.0, -1000.0);
    ctx.candidates.clear();
    ctx.best = HumanCandidate{};
    ctx.safeShot = HumanCandidate{};
    ctx.shot = CommittedShot{};
    ctx.simsUsed = 0;
    ctx.potCands = 0;
    ctx.safeCands = 0;
    ctx.invalidCands = 0;
}

} // namespace HumanScan

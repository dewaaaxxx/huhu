// =====================================================================
// GameMaster.impl.h — Human-style planner implementation
// =====================================================================

#include "game/inc/GameMaster.h"
#include "game/GameManager.h"
#include "Prediction.h"

#include <math.h>
#include <algorithm>

namespace GameMaster {

namespace {

    inline double sq(double x) { return x * x; }

    inline void SetEnglishGM(double x, double y) {
        auto vc = sharedGameManager.mVisualEnglishControl();
        if (vc) vc.mEnglish(Vec2d(x, y));
    }

    double DistToSegmentSqGM(const Point2D& p, const Point2D& a, const Point2D& b) {
        Point2D v = b - a, w = p - a;
        double c1 = w.x * v.x + w.y * v.y;
        if (c1 <= 0) return (p - a).square();
        double c2 = v.x * v.x + v.y * v.y;
        if (c2 <= c1) return (p - b).square();
        double t = c1 / c2;
        return (p - Point2D{a.x + t * v.x, a.y + t * v.y}).square();
    }

    inline bool PointInsideRailGM(const Point2D& point, const Point2D* pockets) {
        double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
        for (int i = 0; i < TABLE_POCKETS_COUNT; i++) {
            minX = std::min(minX, pockets[i].x);
            maxX = std::max(maxX, pockets[i].x);
            minY = std::min(minY, pockets[i].y);
            maxY = std::max(maxY, pockets[i].y);
        }
        minX += POCKET_RADIUS; maxX -= POCKET_RADIUS;
        minY += POCKET_RADIUS; maxY -= POCKET_RADIUS;
        const double margin = BALL_RADIUS * 0.75;
        return point.x >= minX - margin && point.x <= maxX + margin &&
               point.y >= minY - margin && point.y <= maxY + margin;
    }

    inline double RailAlignmentPenaltyGM(const Point2D& ball, const Point2D& pocket,
                                         const Point2D* pockets) {
        double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
        for (int i = 0; i < TABLE_POCKETS_COUNT; i++) {
            minX = std::min(minX, pockets[i].x);
            maxX = std::max(maxX, pockets[i].x);
            minY = std::min(minY, pockets[i].y);
            maxY = std::max(maxY, pockets[i].y);
        }
        minX += POCKET_RADIUS; maxX -= POCKET_RADIUS;
        minY += POCKET_RADIUS; maxY -= POCKET_RADIUS;
        Point2D direction = pocket - ball;
        double length = sqrt(direction.square());
        if (length < 1e-6) return 0.0;
        direction = direction * (1.0 / length);
        const double margin = 2.5 * BALL_RADIUS;
        double penalty = 0.0;
        if (std::min(fabs(ball.x - minX), fabs(ball.x - maxX)) < margin)
            penalty += (1.0 - fabs(direction.y)) * 90.0;
        if (std::min(fabs(ball.y - minY), fabs(ball.y - maxY)) < margin)
            penalty += (1.0 - fabs(direction.x)) * 90.0;
        return penalty;
    }

    constexpr double BALL_D = BALL_RADIUS * 2.0;
    constexpr double CUT_MIN = 0.28;
    constexpr double MAX_CUE = 285.0;
    constexpr double MAX_POCKET = 300.0;
    constexpr double BLOCK_SQ = (2.0 * BALL_RADIUS) * (2.0 * BALL_RADIUS);

    // --- Geometry: single shot candidate ---
    struct Geo {
        bool ok = false;
        double angle = 0.0, power = 0.0, cueDist = 0.0, distP = 0.0;
        double cutDot = 1.0, score = 1e18;
        Point2D ghost;
        int pocket = -1;
    };

    Geo computeGeo(const Point2D& cue, const Point2D& ballPos,
                   const Point2D& pocket, int pIdx,
                   const Point2D* tablePockets,
                   const std::vector<HumanScan::BallEntry>& allBalls, int selfIdx) {
        Geo g; g.pocket = pIdx;
        Point2D effP = GetEffectivePocketTarget(pIdx, pocket, ballPos);
        Point2D toP = effP - ballPos;
        g.distP = sqrt(toP.square());
        if (g.distP < 0.1 || g.distP > MAX_POCKET) return g;
        Point2D dirP = toP * (1.0 / g.distP);
        g.ghost = ballPos - dirP * BALL_D;
        Point2D sl = g.ghost - cue;
        g.cueDist = sqrt(sl.square());
        if (g.cueDist > MAX_CUE || g.cueDist < 0.1) return g;
        g.angle = atan2(sl.y, sl.x);
        if (g.angle < 0) g.angle += 2.0 * M_PI;
        Point2D c2b = ballPos - cue;
        double dcb = sqrt(c2b.square());
        g.cutDot = 1.0;
        if (dcb > 0.001) {
            Point2D dc = c2b * (1.0 / dcb);
            g.cutDot = dc.x * dirP.x + dc.y * dirP.y;
        }
        if (g.cutDot < CUT_MIN) return g;
        if (!PointInsideRailGM(g.ghost, tablePockets)) return g;
        for (auto& o : allBalls) {
            if (o.index == selfIdx) continue;
            if (DistToSegmentSqGM(o.pos, cue, g.ghost) < BLOCK_SQ) return g;
            if (DistToSegmentSqGM(o.pos, ballPos, pocket) < BLOCK_SQ) return g;
        }
        g.power = CalculateRequiredPower(g.cueDist, g.distP, g.cutDot);
        if (g.power > (double)AutoPlay::powerMax) g.power = (double)AutoPlay::powerMax;
        if (g.power < (double)AutoPlay::powerMin) g.power = (double)AutoPlay::powerMin;
        g.score = g.distP + 0.8 * g.cueDist + (1.0 - g.cutDot) * 120.0 +
                  RailAlignmentPenaltyGM(ballPos, pocket, tablePockets);
        g.ok = true;
        return g;
    }

    // --- Position safety score: lower = better (more open) ---
    //   Penalizes: near cushions, near other balls, near corners
    double positionSafety(const Point2D& cuePos,
                           const std::vector<HumanScan::BallEntry>& allBalls,
                           const Point2D* pockets, int nPockets) {
        double penalty = 0.0;

        // Cushion proximity (table ~ 254 x 127)
        double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
        for (int p = 0; p < nPockets; p++) {
            if (pockets[p].x < minX) minX = pockets[p].x;
            if (pockets[p].x > maxX) maxX = pockets[p].x;
            if (pockets[p].y < minY) minY = pockets[p].y;
            if (pockets[p].y > maxY) maxY = pockets[p].y;
        }
        const double cushionMargin = 22.0;
        const double maxPen = 100.0;

        double dxL = cuePos.x - minX, dxR = maxX - cuePos.x;
        double dyT = cuePos.y - minY, dyB = maxY - cuePos.y;
        if (dxL < cushionMargin) penalty += (cushionMargin - dxL) / cushionMargin * maxPen;
        if (dxR < cushionMargin) penalty += (cushionMargin - dxR) / cushionMargin * maxPen;
        if (dyT < cushionMargin) penalty += (cushionMargin - dyT) / cushionMargin * maxPen;
        if (dyB < cushionMargin) penalty += (cushionMargin - dyB) / cushionMargin * maxPen;

        // Ball proximity: penalize if cue rests very close to any other ball
        const double nearSq = (8.0 * BALL_RADIUS) * (8.0 * BALL_RADIUS);
        for (auto& o : allBalls) {
            double d2 = (cuePos - o.pos).square();
            if (d2 < nearSq && d2 > 0.01) {
                penalty += (1.0 - sqrt(d2) / sqrt(nearSq)) * 80.0;
            }
        }

        return penalty;
    }

    inline double PowerPenaltyGM(double power, double idealPower) {
        if (power <= idealPower) {
            return (idealPower - power) * 0.006 + power * 0.003;
        }
        return (power - idealPower) * 0.04 + power * 0.008;
    }

    inline uint32_t TableMask(const HumanScan::TableInfo& table) {
        uint32_t mask = 0;
        for (const auto& ball : table.allBalls) {
            if (ball.index >= 0 && ball.index < 32) mask |= (1u << ball.index);
        }
        return mask;
    }

} // anonymous namespace

// ---------------------------------------------------------------------
// 1. SelectGroup — pick the easier group after the break.
// ---------------------------------------------------------------------
bool SelectGroup(const HumanScan::TableInfo& table) {
    auto& ctx = HumanScan::ctx;

    Ball::Classification g = table.gameGroup;
    if (g == Ball::Classification::SOLID || g == Ball::Classification::STRIPE ||
        g == Ball::Classification::EIGHT_BALL) {
        ctx.table.myGroup = g;
        return true;
    }

    auto scoreG = [&](Ball::Classification cls) -> double {
        int open = 0, problems = 0, count = 0;
        double totDist = 0.0;
        for (auto& t : table.allBalls) {
            if (t.cls != cls) continue;
            count++;
            double best = 1e18;
            for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
                double d = sqrt((t.pos - table.pockets[p]).square());
                if (d < best) best = d;
            }
            totDist += best;
            bool blocked = false;
            for (auto& o : table.allBalls) {
                if (o.index == t.index) continue;
                Point2D bestPocket;
                double bestPocketDist = 1e18;
                for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
                    double d = sqrt((t.pos - table.pockets[p]).square());
                    if (d < bestPocketDist) {
                        bestPocketDist = d;
                        bestPocket = table.pockets[p];
                    }
                }
                Point2D dir = bestPocket - t.pos;
                double l = sqrt(dir.square());
                if (l < 0.01) continue;
                Point2D ghost = t.pos - dir * (1.0 / l) * BALL_D;
                if (DistToSegmentSqGM(o.pos, table.cuePos, ghost) < BLOCK_SQ) { blocked = true; break; }
            }
            int nearC = 0;
            double ns = (3.5 * BALL_RADIUS) * (3.5 * BALL_RADIUS);
            for (auto& o : table.allBalls) {
                if (o.index == t.index) continue;
                if ((o.pos - t.pos).square() < ns) nearC++;
            }
            bool direct = false;
            for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
                if (computeGeo(table.cuePos, t.pos, table.pockets[p], p,
                               table.pockets, table.allBalls, t.index).ok) {
                    direct = true;
                    break;
                }
            }
            if (direct && !blocked && nearC < 2) open++;
            else problems++;
        }
        if (count == 0 || open == 0) return -1e18;
        return open * 130.0 - totDist / count * 2.0 - problems * 100.0;
    };

    double ss = scoreG(Ball::Classification::SOLID);
    double st = scoreG(Ball::Classification::STRIPE);
    if (ss < -1e17 && st < -1e17) {
        ctx.table.myGroup = Ball::Classification::ANY;
        return false;
    }
    ctx.table.myGroup = (ss >= st) ? Ball::Classification::SOLID : Ball::Classification::STRIPE;
    LOGI("[GM] Select: solid=%.0f stripe=%.0f -> %d", ss, st, (int)ctx.table.myGroup);
    return true;
}

// ---------------------------------------------------------------------
// 2. BuildPlan — order group balls by difficulty (easiest first),
//    placing the 8-ball last. Plans only one shot ahead.
// ---------------------------------------------------------------------
GamePlan BuildPlan(const HumanScan::TableInfo& table) {
    GamePlan plan; plan.initialized = true;

    Ball::Classification group = table.myGroup;
    if (group == Ball::Classification::ANY || group == Ball::Classification::EIGHT_BALL)
        return plan;

    struct Entry { int idx; Point2D pos; double difficulty; };
    std::vector<Entry> entries;
    bool groupPresent = false;

    for (auto& t : table.allBalls) {
        if (t.cls != group) continue;
        groupPresent = true;
        // Score each ball by how easy it is to pot right now
        double bestScore = 1e18;
        for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
            Geo g = computeGeo(table.cuePos, t.pos, table.pockets[p], p,
                               table.pockets, table.allBalls, t.index);
            if (g.ok && g.score < bestScore) bestScore = g.score;
        }
        // Keep blocked balls in the plan. They must remain before the 8-ball
        // until they are potted or the planner explicitly replans around them.
        entries.push_back({t.index, t.pos, bestScore});
    }

    // Add 8-ball only when it can be played by the direct-shot planner. A
    // rule-required bank is handled by HumanScan's bank candidate path.
    if (groupPresent && !table.eightBallMustBank) {
        for (auto& t : table.allBalls) {
            if (t.index != 8) continue;
            double bestScore = 1e18;
            for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
                Geo g = computeGeo(table.cuePos, t.pos, table.pockets[p], p,
                                   table.pockets, table.allBalls, 8);
                if (g.ok && g.score < bestScore) bestScore = g.score;
            }
            if (bestScore < 1e17) entries.push_back({8, t.pos, bestScore});
            break;
        }
    }

    if (entries.empty()) return plan;

    // Sort by difficulty: easiest first, but 8-ball always last
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.idx == 8) return false;
        if (b.idx == 8) return true;
        return a.difficulty < b.difficulty;
    });

    for (auto& e : entries) plan.ballOrder.push_back(e.idx);

    LOGI("[GM] Plan: %d balls ordered", (int)plan.ballOrder.size());
    return plan;
}

// ---------------------------------------------------------------------
// 3. TryPlayCurrentBall — find the best shot for the current target.
//    Tests natural spin variants and a bounded 0..666 power grid, picks the one that:
//      (a) pots the ball reliably
//      (b) leaves the cue in a safe, open position
//      (c) sets up an easy follow-up on the next ball
// ---------------------------------------------------------------------
bool TryPlayCurrentBall(const HumanScan::TableInfo& table, bool isLastGroup) {
    auto& ctx = HumanScan::ctx;

    if (g_plan.currentIdx >= (int)g_plan.ballOrder.size()) return false;
    int targetIdx = g_plan.ballOrder[g_plan.currentIdx];
    Ball::Classification group = table.myGroup;
    bool isEightBall = (targetIdx == 8);
    if (isEightBall && table.eightBallMustBank) return false;
    if (isEightBall && group != Ball::Classification::ANY) {
        for (const auto& ball : table.allBalls) {
            if (ball.cls == group) return false;
        }
    }

    // Find target ball position
    Point2D ballPos; bool found = false;
    for (auto& t : table.allBalls) {
        if (t.index == targetIdx) { ballPos = t.pos; found = true; break; }
    }
    if (!found) return false;

    HumanScan::HumanCandidate best;
    double bestScore = 1e18;

    std::vector<std::pair<double, int>> pocketOrder;
    for (int p = 0; p < TABLE_POCKETS_COUNT; p++) {
        // If nomination mode requires it on 8-ball, filter by nominated pocket
        if (isEightBall && table.nominationMode == 1 &&
            table.nominatedPocket >= 0 && table.nominatedPocket < 6 &&
            p != table.nominatedPocket) continue;

        Geo geo = computeGeo(table.cuePos, ballPos, table.pockets[p], p,
                             table.pockets, table.allBalls, targetIdx);
        if (geo.ok) pocketOrder.push_back({geo.score, p});
    }
    std::sort(pocketOrder.begin(), pocketOrder.end());
    if (pocketOrder.size() > 4) pocketOrder.resize(4);

    for (const auto& pocketEntry : pocketOrder) {
        int p = pocketEntry.second;
        Geo geo = computeGeo(table.cuePos, ballPos, table.pockets[p], p,
                             table.pockets, table.allBalls, targetIdx);
        if (!geo.ok) continue;

        for (int v = 0; v < SPIN_SIMPLE_COUNT; v++) {
            const auto& sv = SPIN_SIMPLE[v];

            std::vector<double> powers;
            if (v == 0) {
                powers = AutoPlay::BuildPowerGrid(geo.power);
            } else {
                double power = geo.power * sv.pMult;
                power = std::clamp(power, std::max(0.0, (double)AutoPlay::powerMin),
                                   (double)AutoPlay::powerMax);
                powers.push_back(power);
            }

            for (double power : powers) {
                g_plan.simsUsed++;
                SetEnglishGM(sv.x, sv.y);
                Candidate cand{targetIdx, geo.angle, geo.score, p, power, geo.distP};
                gPrediction->forceFullSimulation = true;
                gPrediction->determineShotResult(true, geo.angle, power,
                                                  sharedGameManager.getShotSpin(), cand);
                gPrediction->forceFullSimulation = false;

                auto& gd = gPrediction->guiData;
                if (gd.ballsCount <= targetIdx || targetIdx <= 0) continue;

            // Must pot the target ball
            bool potted = false;
            for (int i = 1; i < gd.ballsCount; i++) {
                if (i == targetIdx && gd.balls[i].originalOnTable && !gd.balls[i].onTable) {
                    potted = true; break;
                }
            }
                if (!potted) continue;

            // Cue must stay on table
                if (!gd.balls[0].onTable) continue;

            // No wrong-group / 8-ball pot besides the target; own-group
            // multi-pots (legal combos) are allowed.
            bool foul = false;
            for (int i = 1; i < gd.ballsCount; i++) {
                if (!gd.balls[i].originalOnTable || gd.balls[i].onTable || i == targetIdx) continue;
                if (isEightBall) { foul = true; break; }
                if (gd.balls[i].classification != group) { foul = true; break; }
            }
                if (foul) continue;
                if (gd.balls[targetIdx].pocketIndex != p) continue;

                Point2D cueRest = gd.balls[0].predictedPosition;
                std::vector<HumanScan::BallEntry> postBalls;
                for (int i = 1; i < gd.ballsCount; i++) {
                    if (!gd.balls[i].onTable) continue;
                    HumanScan::BallEntry e;
                    e.index = i;
                    e.pos = gd.balls[i].predictedPosition;
                    e.cls = gd.balls[i].classification;
                    postBalls.push_back(e);
                }

            // Position quality: safety + next-ball ease
                double safetyPen = positionSafety(cueRest, postBalls, table.pockets, TABLE_POCKETS_COUNT);

                double nextEase = 0.0;
                if (g_plan.currentIdx + 1 < (int)g_plan.ballOrder.size()) {
                int nextIdx = g_plan.ballOrder[g_plan.currentIdx + 1];
                Point2D nextPos;
                bool nf = false;
                for (auto& t : postBalls) {
                    if (t.index == nextIdx) { nextPos = t.pos; nf = true; break; }
                }
                    if (nf) {
                    double bestD = 1e18;
                        for (int np = 0; np < TABLE_POCKETS_COUNT; np++) {
                            Geo ng = computeGeo(cueRest, nextPos, table.pockets[np], np,
                                                table.pockets, postBalls, nextIdx);
                            if (ng.ok && ng.score < bestD) bestD = ng.score;
                        }
                        nextEase = (bestD < 1e17) ? bestD : 1e18;
                    }
                }

            // Composite score: shot ease + safety + position quality
                double posWeight = isLastGroup ? 2.0 : 1.2;
                double total = geo.score + safetyPen + posWeight * nextEase +
                               PowerPenaltyGM(power, geo.power);

                if (total < bestScore) {
                bestScore = total;
                best.idx = targetIdx;
                best.pocket = p;
                best.angle = geo.angle;
                best.power = power;
                best.spin = Vec2d(sv.x, sv.y);
                best.spinPreset = sv.preset;
                best.spinTag = sv.tag;
                best.cueDist = geo.cueDist;
                best.dist = geo.distP;
                best.predictedCueRest = cueRest;
                best.valid = true;
                best.potted = true;
                best.score = geo.score;
                }
            }
        }
    }
    SetEnglishGM(0.0, 0.0);

    if (!best.valid) {
        LOGI("[GM] No valid shot for ball %d", targetIdx);
        return false;
    }

    g_plan.activeTarget = targetIdx;
    g_plan.activePocket = best.pocket;
    g_plan.activeBeforeMask = TableMask(table);
    g_plan.predictedCueRest = best.predictedCueRest;
    g_plan.activeStep = true;

    HumanScan::CommitShot(best);
    HumanScan::ExecuteShot();
    ctx.turnAnalyzed = true;
    ctx.awaitingSettlement = true;
    g_plan.currentIdx++;

    LOGI("[GM] Shot ball=%d pocket=%d pwr=%.0f spin=%s safety=%.0f sims=%d",
         targetIdx, best.pocket, best.power, best.spinTag, bestScore, g_plan.simsUsed);
    return true;
}

// ---------------------------------------------------------------------
// RunMasterPlan — per-frame orchestrator.
// ---------------------------------------------------------------------
bool RunMasterPlan() {
    auto& ctx = HumanScan::ctx;
    if (!sharedGameManager || sharedGameManager.is9BallGame()) return false;
    if (AutoPlay::AreBallsMoving()) return true;

    // Previous shot settled
    if (ctx.awaitingSettlement) {
        ctx.awaitingSettlement = false;
        ctx.turnAnalyzed = false;
        if (!g_plan.initialized || !g_plan.activeStep ||
            g_plan.currentIdx <= 0 ||
            g_plan.currentIdx > (int)g_plan.ballOrder.size()) return false;

        const int targetIdx = g_plan.activeTarget >= 0
                            ? g_plan.activeTarget
                            : g_plan.ballOrder[g_plan.currentIdx - 1];
        if (!HumanScan::AnalyzeTable()) return false;

        const uint32_t afterMask = TableMask(ctx.table);
        const uint32_t targetBit = (targetIdx >= 0 && targetIdx < 32)
                                 ? (1u << targetIdx) : 0u;
        const uint32_t removedMask = g_plan.activeBeforeMask & ~afterMask;
        const bool targetPotted = targetBit != 0 && (removedMask & targetBit) != 0;
        // Own-group multi-pot combos are legal progress; only flag pots of
        // opponent balls or the 8-ball (unless on the 8).
        bool unexpectedPotted = false;
        for (int i = 1; i < 32; i++) {
            if (!(removedMask & ~targetBit & (1u << i))) continue;
            // Use gameGroup (set by AnalyzeTable from the engine) not myGroup,
            // which is reset to ANY until ChooseGroup runs.
            bool own = false;
            if (ctx.table.only8BallLeft ||
                ctx.table.gameGroup == Ball::Classification::EIGHT_BALL) {
                own = (i == 8);
            } else if (ctx.table.gameGroup == Ball::Classification::SOLID) {
                own = (i >= 1 && i <= 7);
            } else if (ctx.table.gameGroup == Ball::Classification::STRIPE) {
                own = (i >= 9 && i <= 15);
            } else { // ANY (open table)
                own = (i != 8);
            }
            if (!own) { unexpectedPotted = true; break; }
        }
        const bool cueStayedOnTable = ctx.table.cueOnTable;
        const double cueDrift = (ctx.table.cuePos - g_plan.predictedCueRest).square();
        const bool cueLeaveWasPlausible =
            g_plan.predictedCueRest.square() < 1e-9 || cueDrift <= (18.0 * 18.0);
        const bool stepOk = targetPotted && !unexpectedPotted &&
                            cueStayedOnTable && cueLeaveWasPlausible;

        g_plan.activeStep = false;
        if (!stepOk) {
            LOGI("[GM] Step FAILED target=%d potted=%d unexpected=%d scratch=%d drift=%.1f",
                 targetIdx, targetPotted ? 1 : 0, unexpectedPotted ? 1 : 0,
                 cueStayedOnTable ? 0 : 1, sqrt(cueDrift));
            g_plan.replanCount++;
            if (g_plan.replanCount >= 2) return false;

            HumanScan::ChooseGroup();
            int retries = g_plan.replanCount;
            g_plan = BuildPlan(ctx.table);
            g_plan.replanCount = retries;
            if (g_plan.ballOrder.empty()) return false;
        } else {
            g_plan.replanCount = 0;
            LOGI("[GM] Step OK target=%d pocket=%d next=%d/%d",
                 targetIdx, g_plan.activePocket, g_plan.currentIdx,
                 (int)g_plan.ballOrder.size());
            if (g_plan.currentIdx >= (int)g_plan.ballOrder.size()) {
                g_plan.complete = true;
                LOGI("[GM] Full plan COMPLETE");
                return true;
            }

            const int nextTarget = g_plan.ballOrder[g_plan.currentIdx];
            if (ctx.table.eightBallMustBank && nextTarget == 8) return false;
            HumanScan::ChooseGroup();
        }
    }

    if (ctx.turnAnalyzed) return false;

    // After break, build the plan
    if (!g_plan.initialized && !ctx.table.only8BallLeft &&
        ctx.table.myGroup != Ball::Classification::ANY &&
        ctx.table.myGroup != Ball::Classification::EIGHT_BALL) {
        g_plan = BuildPlan(ctx.table);
        if (g_plan.ballOrder.empty()) return false;
    }

    // Play the current ball
    if (g_plan.initialized && !g_plan.complete &&
        g_plan.currentIdx < (int)g_plan.ballOrder.size()) {
        int groupLeft = 0;
        for (const auto& b : ctx.table.allBalls) {
            if (b.cls == ctx.table.myGroup) groupLeft++;
        }
        bool lastGroup = (groupLeft == 1);
        return TryPlayCurrentBall(ctx.table, lastGroup);
    }

    return false;
}

} // namespace GameMaster

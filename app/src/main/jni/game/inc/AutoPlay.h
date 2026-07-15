#pragma once

#include "Prediction.fast.h"
#include <imgui/imgui.h>
#include <algorithm>
#include "ScreenTable.h"
#include "mod/ButtonClicker.h"

using namespace ImGui;

constexpr double maxAngle = 360.0 / (180.0 / M_PI);

double normalizeAngle(double angle) {
    double newAngle = angle;
    if (newAngle >= maxAngle) newAngle = fmod(newAngle, maxAngle);
    else if (newAngle < 0) newAngle = maxAngle - fmod(-newAngle, maxAngle);
    return newAngle;
}

static double EaseInOutCubic(double t) {
    return t < 0.5 ? 4 * t * t * t : 1.0 - pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

Candidate g_CurrentCandidate = { -1 };

ImVec2 GetPocketScreenPos(int pocketIdx) {
    Table table = sharedGameManager.mTable;
    if (!table) return {};
    auto tableProperties = table.mTableProperties();
    if (!tableProperties) return {};
    auto& pockets = tableProperties.mPockets();
    return WorldToScreen(pockets[pocketIdx]);
}

bool IsShotValid() {
    auto& cand = g_CurrentCandidate;
    if (cand.idx == -1) return false;
    Ball::Classification myclass = sharedGameManager.getPlayerClassification();
    uint nominatedPocket = sharedGameManager.getNominatedPocket();
    if (nominatedPocket < 6 && cand.pocketIndex != nominatedPocket) return false;
    if (!gPrediction->guiData.balls[0].onTable) return false;
    if (!gPrediction->guiData.balls[cand.idx].originalOnTable) return false;
    if (gPrediction->guiData.balls[cand.idx].onTable) return false;
    if (gPrediction->guiData.balls[cand.idx].pocketIndex != cand.pocketIndex) return false;
    auto& ball8 = gPrediction->guiData.balls[8];
    if (myclass == Ball::Classification::ANY && ball8.originalOnTable && !ball8.onTable) return false;
    auto& firstHit = gPrediction->guiData.collision.firstHitBall;
    if (firstHit) {
        if (myclass == Ball::Classification::ANY) {
            if (firstHit->classification == Ball::Classification::EIGHT_BALL) return false;
        } else if (firstHit->classification != myclass) return false;
    }
    return true;
}

Point2D lastFailedCuePos = { -1000.0, -1000.0 };
Point2D lastSetCuePos = {-1000, -1000};

namespace AutoPlay {
    bool bAutoPlaying = false;

    // ── Live debug info (read by menu.h overlay) ──────
    struct ShotDebug {
        int   candidatesFound = 0;  // how many valid shots were found in Phase 1
        int   tolerancePts    = 0;  // 0/1/2 — how many of ±0.008 rad also work
        float qualityScore    = 0;  // combined quality (lower = better)
        bool  wonByTolerance  = false; // true = tolerance tiebreak decided it
    } g_dbg;

    enum State { IDLE, SCANNING, NOMINATING, EXECUTING } state = IDLE;

    double pendingShotPower = 0.f;
    double pendingShotAngle = 0.f;
    int nominationFrameCounter = 0;

    enum ScanMode { FAST, SLOW } scan = FAST;
    
    enum HumanState {
        HUM_IDLE,
        HUM_THINKING,
        HUM_OVERSHOOTING,
        HUM_CORRECTING,
        HUM_HOLDING,
        HUM_STABILIZING,
        HUM_PULLING,
        HUM_DELAY_BEFORE_SHOT,
    };
    
    // ── Variabel Human State ──
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
    
    // FIX ROOT CAUSE: Spin yang dipakai saat scan HARUS SAMA dengan spin saat tembak.
    // Kalau berbeda: simulasi scan → hasil A, simulasi display dengan spin lain → hasil B
    // → prediction line kelihatan masuk sebelum tembak, tapi setelah tembak meleset.
    // Solusi: lock spin pada saat scan dimulai, gunakan spin yang sama untuk semua
    // determineShotResult call (scan, display, tembak) sampai shot selesai.
    // FIX POWER: Power yang dipakai scan harus sama persis dengan yang ditembak.
    // confirmedPower dari scan disimpan di g_CurrentCandidate.power dan di targetPower.
    // Konversi ke mPower() via ShotPowerToPower() sudah benar — tidak perlu diubah.
    // Yang penting: power sweep di scan harus cover range yang realistis
    // berdasarkan getShotPower() (skala simulasi), bukan mPower() (skala 0-1).
    static bool g_postShotLock = false;
    static double g_postShotAngle = 0.0;
    static double g_postShotPower = 0.0;
    static int g_postShotFrames = 0;
    
    // ── Random engine ──
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<> humanDelayDist(0.15, 0.4);
    
    static double nowSec() {
        auto now = std::chrono::steady_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration<double>(duration).count();
    }

    void setAimAngle(double angle) {
        if (!sharedGameManager) return;
        auto vc = sharedGameManager.mVisualCue();
        if (!vc) return;
        auto vg = vc.mVisualGuide();
        if (!vg) return;
        lastSetCuePos = gPrediction->guiData.balls[0].initialPosition;
        vg.mAimAngle(angle);
    }

    // takeShot: the ONLY place determineShotResult(false,...) is called.
    // Order matters: simulation first → power → setAimAngle LAST → fire.
    // setAimAngle is called LAST (right before fire) so that even if the
    // physics simulation takes time, the angle cannot be overridden between
    // setAimAngle and the actual fire command. spin=0 = straight physics.
    void takeShot(double angle, double power) {
        targetAngle = angle;
        targetPower = power;
        startAngle = sharedGameManager.mVisualCue().mVisualGuide().mAimAngle();
        stateStartTime = nowSec();
        humanState = HUM_THINKING;
    }
    
    void triggerShot() {
        g_postShotLock = true;
        g_postShotAngle = targetAngle;
        // BUG FIX: pendingShotPower tidak selalu sync dengan targetPower.
        // takeShot() set targetPower, tapi triggerShot() baca pendingShotPower → power salah/lebih pelan.
        // Pakai targetPower yang di-set oleh takeShot() dan di-hold sepanjang human state machine.
        g_postShotPower = targetPower;
        g_postShotFrames = 15;
        M(void, libmain + 0x2dc0c58, void*)(F(void*, sharedGameManager + 0x3b0));
    }

    void ClearState() {
        g_CurrentCandidate.idx = -1;
        lastFailedCuePos = { -1000.0, -1000.0 };
        state = IDLE;
        humanState = HUM_IDLE;
    }

    // ── Scene Snapshot: capture ball positions once per turn, reuse for all
    // simulation calls in ScanFast/ScanSlow. Eliminates repeated game-memory
    // reads that are the primary cause of FPS stutter during scanning.
    void TakeSnapshot() {
        g_useSnapshot = false;
        g_sceneSnapshot.valid = false;
        Table table = sharedGameManager.mTable;
        if (!table) return;
        auto& balls = table.mBalls();
        if (!balls) return;
        g_sceneSnapshot.tableBounds = table.mTableCollisionBounds();
        g_sceneSnapshot.ballsCount  = balls.Count;
        for (int i = 0; i < g_sceneSnapshot.ballsCount && i < MAX_BALLS_COUNT; i++) {
            auto& dst         = g_sceneSnapshot.balls[i];
            dst.index         = i;
            dst.state         = balls[i].state();
            dst.originalOnTable = balls[i].isOnTable();
            dst.classification  = balls[i].classification();
            dst.position      = balls[i].position();
        }
        auto tableProperties = table.mTableProperties();
        if (tableProperties) {
            auto& pockets = tableProperties.mPockets();
            for (int i = 0; i < TABLE_POCKETS_COUNT; i++)
                g_sceneSnapshot.pockets[i] = pockets[i];
            g_sceneSnapshot.hasPockets = true;
        }
        g_sceneSnapshot.valid = true;
        g_useSnapshot         = true;
    }

    // ── Auto-power: reduce for close shots (prevent overshoot),
    // boost slightly for long rail shots (ensure ball reaches pocket).
    //
    //  distCG    = cue-ball to ghost-ball distance
    //  totalDist = distCG + target-to-pocket distance
    //
    // Close shot  (distCG < 25):  0.62× at contact → 1.0× at 25 units
    // Long shot   (totalDist > 160): linear +0–18% boost up to 260 units
    static double calcAdaptivePower(double distCG, double totalDist) {
        double rawPow = sqrt(2.0 * 196.0 * totalDist);
        double mult   = 1.0;
        if (distCG < 25.0) {
            // Linear: short distance → lower power, prevents overshoot
            mult = 0.62 + (distCG / 25.0) * 0.38;
        } else if (totalDist > 160.0) {
            // Long / over-rail shot: gentle power boost
            double boost = ((totalDist - 160.0) / 100.0) * 0.18;
            if (boost > 0.18) boost = 0.18;
            mult = 1.0 + boost;
        }
        double power = rawPow * mult;
        if (power > 480.0) power = 480.0;
        if (power < 80.0)  power = 80.0;
        return power;
    }

    // Shoot: sets angle for UI, checks nomination, then fires.
    // determineShotResult(false) is NOT called here — takeShot handles it once.
    // (Calling it twice was the source of physics double-apply and miss shots.)
    void Shoot(double angle, double power = 0.f) {
        //setAimAngle(angle);

        bool nominating = false;
        int nominationMode = sharedGameManager.getPocketNominationMode();
        auto myclass = sharedGameManager.getPlayerClassification();
        if ((nominationMode == 1 && myclass == Ball::Classification::EIGHT_BALL) ||
            (nominationMode == 2 && myclass != Ball::Classification::ANY)) {
            if (g_CurrentCandidate.idx != -1 &&
                sharedGameManager.getNominatedPocket() != g_CurrentCandidate.pocketIndex) {
                nominating = true;
            }
        }

        if (nominating) {
            pendingShotPower = power;
            pendingShotAngle = angle;
            state = NOMINATING;
            nominationFrameCounter = 0;
        } else {
            takeShot(angle, power);
            ClearState();
            state = IDLE;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // ScanSlow — quality-aware fallback when all geometric paths are blocked.
    //
    // IMPROVEMENTS vs old version:
    //  - Power calculated dynamically from angle geometry (not hardcoded)
    //  - Collects up to 3 valid shots per frame, picks best by quality score
    //  - Quality = cue-center distance + damage penalty to own balls
    //  - Damage threshold: 0.5 units (catches even small nudges)
    //  - 16 steps/frame at 0.005 rad = sweep 360° in ~78 frames (~1.3s at 60fps)
    // ═══════════════════════════════════════════════════════════════════════════
    void ScanSlow(double angleStep = 0.005f) {
        static double currentScanAngle = 0.0;
        static bool   isScanning       = false;
        static Point2D lastScanCuePos  = { -1000.0, -1000.0 };

        if (g_CurrentCandidate.idx != -1) return;

        auto& cueBall = gPrediction->guiData.balls[0];
        if (!isScanning || cueBall.initialPosition != lastScanCuePos) {
            currentScanAngle = 0.0;
            isScanning       = true;
            lastScanCuePos   = cueBall.initialPosition;
        }

        Ball::Classification myclass   = sharedGameManager.getPlayerClassification();
        uint  nominatedPocket          = sharedGameManager.getNominatedPocket();
        bool  isNineBallGame           = (myclass == Ball::Classification::NINE_BALL_RULE);
        auto  pockets                  = getPockets();

        // Best-shot accumulator for this frame (up to 3 valid shots collected)
        struct SlowShot { int targetIdx; int pocketIdx; double angle; double power; double quality; };
        SlowShot best = { -1, -1, 0, 0, 1e18 };

        int  steps     = 0;
        int  collected = 0;

        while (steps < 20 && currentScanAngle < maxAngle) {
            double angle = currentScanAngle;
            currentScanAngle += angleStep;
            steps++;

            // Try 3 powers per angle: derived from shot distance where possible,
            // otherwise use fixed medium values for safety.
            const double POWERS[] = { 150.0, 220.0, 300.0, 380.0, 460.0, 540.0, 620.0, 666.0 };

            for (double power : POWERS) {
                Candidate dummy = { -1 };
                gPrediction->determineShotResult(true, angle, power, 0.0, dummy);

                // Cue ball must stay on table (no scratch)
                if (!gPrediction->guiData.balls[0].onTable) continue;
                auto* firstHit = gPrediction->guiData.collision.firstHitBall;
                if (!firstHit) continue;

                int targetIdx  = -1;
                int pocketIdx  = -1;

                if (isNineBallGame) {
                    int lowestIdx = -1;
                    for (int i = 1; i < gPrediction->guiData.ballsCount; i++)
                        if (gPrediction->guiData.balls[i].originalOnTable) { lowestIdx = i; break; }
                    if (firstHit->index != lowestIdx) continue;

                    for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                        auto& b = gPrediction->guiData.balls[i];
                        if (!b.originalOnTable || b.onTable) continue;
                        if (nominatedPocket < 6 && b.pocketIndex != (int)nominatedPocket) continue;
                        if (i == 9) { targetIdx = 9; pocketIdx = b.pocketIndex; break; }
                        if (targetIdx == -1 || i == firstHit->index) { targetIdx = i; pocketIdx = b.pocketIndex; }
                    }
                } else {
                    // 8-ball classification checks
                    if (myclass == Ball::Classification::ANY) {
                        if (firstHit->classification == Ball::Classification::EIGHT_BALL) continue;
                    } else if (firstHit->classification != myclass) continue;
                    if (gPrediction->guiData.balls[8].originalOnTable &&
                        !gPrediction->guiData.balls[8].onTable &&
                        myclass != Ball::Classification::EIGHT_BALL) continue;

                    for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                        auto& b = gPrediction->guiData.balls[i];
                        if (!b.originalOnTable || b.onTable) continue;
                        bool isTarget = (myclass == Ball::Classification::ANY)
                            ? (b.classification != Ball::Classification::CUE_BALL &&
                               b.classification != Ball::Classification::EIGHT_BALL)
                            : (b.classification == myclass);
                        if (!isTarget) continue;
                        if (nominatedPocket < 6 && b.pocketIndex != (int)nominatedPocket) continue;
                        targetIdx = i; pocketIdx = b.pocketIndex; break;
                    }
                }
                if (targetIdx == -1) continue;

                // ── Quality scoring (same as ScanFast Phase 2) ──────────────
                Point2D cueFinal = gPrediction->guiData.balls[0].predictedPosition;
                double  cueDist  = sqrt(cueFinal.x * cueFinal.x + cueFinal.y * cueFinal.y);
                double  damage   = 0.0;

                if (!isNineBallGame) {
                    for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                        auto& b = gPrediction->guiData.balls[i];
                        if (!b.originalOnTable || !b.onTable) continue;
                        if (i == targetIdx) continue;
                        bool isMine = (myclass == Ball::Classification::ANY)
                            ? (b.classification != Ball::Classification::CUE_BALL &&
                               b.classification != Ball::Classification::EIGHT_BALL)
                            : (b.classification == myclass);
                        if (!isMine) continue;
                        double dx = b.predictedPosition.x - b.initialPosition.x;
                        double dy = b.predictedPosition.y - b.initialPosition.y;
                        double moved = sqrt(dx * dx + dy * dy);
                        if (moved > 0.5) damage += moved * 10.0;
                    }
                }

                double quality = cueDist * 0.4 + damage + (double)(power) * 0.05;
                if (quality < best.quality) {
                    best = { targetIdx, pocketIdx, angle, power, quality };
                }
                collected++;
                break;  // found valid shot at this angle — try next angle
            }
            if (collected >= 3) break;  // collected enough to compare
        }

        if (best.targetIdx != -1) {
            // Found the best slow-scan shot this frame — shoot it
            g_CurrentCandidate.idx        = best.targetIdx;
            g_CurrentCandidate.angle      = best.angle;
            g_CurrentCandidate.power      = best.power;
            g_CurrentCandidate.pocketIndex= best.pocketIdx;
            g_dbg.candidatesFound         = collected;
            g_dbg.qualityScore            = (float)best.quality;
            g_dbg.tolerancePts            = 0;  // no tolerance test in slow scan
            g_dbg.wonByTolerance          = false;
            Shoot(best.angle, best.power);
            isScanning = false;
            return;
        }

        if (currentScanAngle >= maxAngle) {
            isScanning       = false;
            currentScanAngle = 0.0;
            state            = IDLE;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // ScanFast — pure geometry + single simulation verify.
    //
    // HOW IT WORKS (zero frame lag, maximum accuracy):
    //  1. Compute ghost-ball angle analytically — O(1), FREE, perfect geometry
    //  2. ONE determineShotResult(true,...) call to verify (obstruction check)
    //  3. If valid: optionally try lower power (1 more call) for cleaner physics
    //  4. SHOOT IMMEDIATELY — no sweeping, no boundary walking
    //  5. If blocked: skip to next candidate (sweeping won't unblock an obstacle)
    //
    // Total calls per frame: typically 1-4 (stops at first valid candidate)
    // Worst case (all blocked): N_candidates × 1 = max ~36 calls/frame — still fast
    // ═══════════════════════════════════════════════════════════════════════════
    // ═══════════════════════════════════════════════════════════════════════════
    // ScanFast — GENIUS 4-PHASE STRATEGY
    //
    // PHASE 0: Build all geometry candidates (no simulation).
    //   Score by cut-angle penalty: full-hit (0° cut) ranked first.
    //   Shorter + fuller = easiest shot geometrically.
    //
    // PHASE 1: Collect up to 5 valid candidates via simulation.
    //   Per candidate: 1 verify call + up to 3 softer-power calls = max 4 calls.
    //   Stops collecting after 5 — enough to differentiate quality.
    //
    // PHASE 2: Score each valid candidate by shot QUALITY (not just geometry).
    //   1 extra simulation call per candidate (re-run with bestPower).
    //   Quality metrics (lower = better choice):
    //     a) Cue ball center distance  → land near table center = reach all balls
    //     b) Damage to MY balls        → penalize disturbing own balls on table
    //     c) Next-shot distance        → cue lands close to next target = easy run-out
    //
    // PHASE 3: Tolerance window test (±0.008 rad = ±0.46°).
    //   2 simulation calls per candidate.
    //   A shot that still pots at ±0.008 rad has inherent forgiveness.
    //   Tolerant shots rank first — they survive tiny physics differences.
    //
    // PHASE 4: Sort (tolerance DESC, quality ASC) → shoot best.
    //
    // Total calls: max ~5×4 + 5×1 + 5×2 = 35 per turn. Still single-frame fast.
    // ═══════════════════════════════════════════════════════════════════════════
    void ScanFast() {
        if (g_CurrentCandidate.idx != -1) return;
        if (gPrediction->guiData.balls[0].initialPosition == lastFailedCuePos) return;

        Ball::Classification myclass = sharedGameManager.getPlayerClassification();
        uint nominatedPocket         = sharedGameManager.getNominatedPocket();
        auto pockets                 = getPockets();
        auto& cueBall                = gPrediction->guiData.balls[0];
        bool isNineBall              = (myclass == Ball::Classification::NINE_BALL_RULE);

        // ── PHASE 0: Geometry candidates ──────────────────────────────────────
        std::vector<Candidate> candidates;
        candidates.reserve(36);

        bool foundLowest = false;
        int  lowestIdx   = -1;

        for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
            auto& ball = gPrediction->guiData.balls[i];
            if (!ball.originalOnTable) continue;
            if (!foundLowest) { foundLowest = true; lowestIdx = i; }
            if (isNineBall && i != lowestIdx) break;

            if (!isNineBall) {
                bool isTarget = (myclass == Ball::Classification::ANY)
                    ? ball.classification != Ball::Classification::EIGHT_BALL
                    : ball.classification == myclass;
                if (!isTarget) continue;
            }

            for (int pIdx = 0; pIdx < (int)pockets.size(); pIdx++) {
                if (nominatedPocket < 6 && pIdx != (int)nominatedPocket) continue;

                Point2D pocket  = pockets[pIdx];
                Point2D toPock  = pocket - ball.initialPosition;
                double  distTP  = sqrt(toPock.square());
                if (distTP < 0.1) continue;

                Point2D dir   = toPock * (1.0 / distTP);
                Point2D ghost = ball.initialPosition - dir * (2.0 * BALL_RADIUS);
                Point2D sVec  = ghost - cueBall.initialPosition;
                double  distCG = sqrt(sVec.square());
                if (distCG < 0.01) continue;

                double angle = atan2(sVec.y, sVec.x);
                if (angle < 0) angle += 2.0 * M_PI;

                // Cut-angle penalty: cosTheta=1 (full hit) = no penalty
                //                    cosTheta=0 (90° cut) = +2×distCG penalty
                double cosTheta = (sVec.x * dir.x + sVec.y * dir.y) / distCG;
                double score    = distCG * (2.0 - cosTheta) + distTP;

                // Auto-adaptive power: reduces near-ball overshoot, boosts long shots
                double power = calcAdaptivePower(distCG, distCG + distTP);

                candidates.push_back({i, angle, score, pIdx, power});
            }
        }

        if (candidates.empty()) {
            lastFailedCuePos = cueBall.initialPosition;
            scan = SLOW;
            return;
        }
        std::sort(candidates.begin(), candidates.end());

        // ── Shared validation lambda (runs after any determineShotResult) ──────
        // Returns true if result is a legal pot for this game mode.
        auto isLegalPot = [&](const Candidate& c) -> bool {
            if (!gPrediction->firstHitIsTarget)           return false;
            if (!gPrediction->guiData.balls[0].onTable)  return false;

            if (isNineBall) {
                auto* fh = gPrediction->guiData.collision.firstHitBall;
                if (!fh || fh->index != c.idx) return false;
                for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                    auto& b = gPrediction->guiData.balls[i];
                    if (!b.originalOnTable || b.onTable) continue;
                    if (nominatedPocket < 6 && b.pocketIndex != (int)nominatedPocket) continue;
                    if (i == 9 || i == c.idx) return true;
                }
                return false;
            }

            // 8-ball / solid-stripe
            if (gPrediction->guiData.balls[c.idx].onTable) return false;
            if (gPrediction->guiData.balls[c.idx].pocketIndex != c.pocketIndex) return false;
            if (gPrediction->guiData.balls[8].originalOnTable &&
                !gPrediction->guiData.balls[8].onTable &&
                myclass != Ball::Classification::EIGHT_BALL) return false;

            bool anyPotted = false;
            for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                auto& b = gPrediction->guiData.balls[i];
                bool mine = (myclass == Ball::Classification::ANY)
                    ? (b.classification != Ball::Classification::CUE_BALL &&
                       b.classification != Ball::Classification::EIGHT_BALL)
                    : (b.classification == myclass);
                if (mine && b.originalOnTable && !b.onTable) { anyPotted = true; break; }
            }
            if (!anyPotted) return false;

            auto* fh = gPrediction->guiData.collision.firstHitBall;
            if (fh) {
                if (myclass != Ball::Classification::ANY && fh->classification != myclass) return false;
                if (myclass == Ball::Classification::ANY &&
                    fh->classification == Ball::Classification::EIGHT_BALL) return false;
            }
            return true;
        };

        // ── PHASE 1: Collect up to 5 valid candidates ─────────────────────────
        struct ValidShot {
            Candidate cand;
            double    angle;
            double    bestPower;
            double    qualityScore;   // lower = better
            int       tolerancePts;   // 0..2 from ±0.008 rad test
        };
        std::vector<ValidShot> validShots;
        validShots.reserve(5);

        for (const auto& cand : candidates) {
            if ((int)validShots.size() >= 5) break;

            // ── Angle refinement: coba beberapa offset kecil sekitar angle kandidat ──
            constexpr double angleOffsets[] = {0.0, -0.003, +0.003, -0.0175, +0.0175, -0.035, +0.035};
            double bestAngle = cand.angle;
            double bestPower = cand.power;
            bool   foundValid = false;

            for (double dA : angleOffsets) {
                double tryAngle = NumberUtils::normalizeDoublePrecision(
                    normalizeAngle(cand.angle + dA));

                // ── Power sweep: coba beberapa level power per angle ──────────────
                constexpr double powerFactors[] = {1.0, 1.2, 0.85, 1.4, 0.7, 1.5, 0.65};
                for (double pf : powerFactors) {
                    double tryPower = std::min(std::max(cand.power * pf, 80.0), 666.0);

                    gPrediction->determineShotResult(true, tryAngle, tryPower, 0.0, cand);
                    if (!isLegalPot(cand)) continue;

                    // Valid — simpan kombinasi terbaik (prioritas power paling rendah)
                    if (!foundValid || tryPower < bestPower) {
                        bestAngle  = tryAngle;
                        bestPower  = tryPower;
                        foundValid = true;
                    }
                    // Kalau sudah ketemu di power 1.0 dan angle 0.0, langsung pakai
                    if (dA == 0.0 && pf == 1.0) goto useFound;
                    break; // ketemu di power ini, skip power berikutnya untuk angle ini
                }
                if (foundValid && dA == 0.0) break; // angle pas, tidak perlu coba offset
            }
            useFound:
            if (!foundValid) continue;

            // Verify final dengan angle+power terbaik
            gPrediction->determineShotResult(true, bestAngle, bestPower, 0.0, cand);
            if (!isLegalPot(cand)) continue;

            if (isNineBall) {
                int bestPotted = -1;
                for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                    auto& b = gPrediction->guiData.balls[i];
                    if (!b.originalOnTable || b.onTable) continue;
                    if (nominatedPocket < 6 && b.pocketIndex != (int)nominatedPocket) continue;
                    if (i == 9) { bestPotted = 9; break; }
                    if (bestPotted == -1 || i == cand.idx) bestPotted = i;
                }
                if (bestPotted == -1) continue;
                Candidate c9 = cand;
                c9.idx = bestPotted;
                c9.pocketIndex = gPrediction->guiData.balls[bestPotted].pocketIndex;
                validShots.push_back({c9, bestAngle, bestPower, cand.score, 0});
                continue;
            }

            // 8-ball: try softer powers
            const double pTry[3] = { bestPower * 0.70, bestPower * 0.82, bestPower * 0.92 };
            for (int pi = 0; pi < 3; pi++) {
                double rp = pTry[pi];
                if (rp < 80.0) rp = 80.0;
                if (rp >= bestPower) continue;
                gPrediction->determineShotResult(true, bestAngle, rp, 0.0, cand);
                if (gPrediction->firstHitIsTarget &&
                    gPrediction->guiData.balls[0].onTable &&
                    !gPrediction->guiData.balls[cand.idx].onTable &&
                    gPrediction->guiData.balls[cand.idx].pocketIndex == cand.pocketIndex) {
                    bestPower = rp;
                    break;
                }
            }

            validShots.push_back({cand, bestAngle, bestPower, cand.score, 0});
        }

        if (validShots.empty()) {
            lastFailedCuePos = cueBall.initialPosition;
            scan = SLOW;
            return;
        }

        // ── PHASE 2 + 3 (combined, per-candidate): Quality + tolerance ──────────
        //
        // Processed one candidate at a time — NOT two separate passes.
        // EARLY-EXIT RULE: if raw qualityScore < 100 AND tolerancePts == 2
        //   → lock that shot immediately, return without inspecting further candidates.
        //   → guarantees a "perfect" shot can NEVER be displaced by a quality 200+ shot
        //     that follows it in the sorted list.
        //
        // Tolerance test is skipped for shots with raw quality >= 150 (not worth
        // spending 2 simulation calls on a shot that won't win anyway).
        if (!isNineBall) {

            for (auto& vs : validShots) {
                // ── Phase 2: quality scoring (1 simulation call) ──────────────
                gPrediction->determineShotResult(true, vs.angle, vs.bestPower, 0.0, vs.cand);

                Point2D cueFinal = gPrediction->guiData.balls[0].predictedPosition;
                double  cueDist  = sqrt(cueFinal.x * cueFinal.x + cueFinal.y * cueFinal.y);

                double damage      = 0.0;
                double nearestNext = 9999.0;
                int    myBallsLeft = 0;

                for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                    auto& b = gPrediction->guiData.balls[i];
                    if (!b.originalOnTable) continue;
                    if (i == vs.cand.idx) continue;

                    bool isMine = (myclass == Ball::Classification::ANY)
                        ? (b.classification != Ball::Classification::CUE_BALL &&
                           b.classification != Ball::Classification::EIGHT_BALL)
                        : (b.classification == myclass);
                    if (!isMine) continue;

                    if (b.onTable) {
                        myBallsLeft++;
                        double dx    = b.predictedPosition.x - b.initialPosition.x;
                        double dy    = b.predictedPosition.y - b.initialPosition.y;
                        double moved = sqrt(dx * dx + dy * dy);
                        if (moved > 0.5) damage += moved * 10.0;
                        double nx = b.predictedPosition.x - cueFinal.x;
                        double ny = b.predictedPosition.y - cueFinal.y;
                        double nd = sqrt(nx * nx + ny * ny);
                        if (nd < nearestNext) nearestNext = nd;
                    }
                }
                if (myBallsLeft == 0) nearestNext = 0.0;

                constexpr double WALL_X = 127.0, WALL_Y = 63.5, WALL_THRESH = 18.0;
                double wallPenalty  = 0.0;
                double distToWallX  = WALL_X - fabs(cueFinal.x);
                double distToWallY  = WALL_Y - fabs(cueFinal.y);
                if (distToWallX < WALL_THRESH) wallPenalty += (WALL_THRESH - distToWallX) * 6.0;
                if (distToWallY < WALL_THRESH) wallPenalty += (WALL_THRESH - distToWallY) * 6.0;

                vs.qualityScore = vs.cand.score
                                + cueDist * 2.0
                                + damage
                                + nearestNext * 0.7
                                + wallPenalty;

                // ── Phase 3: tolerance (2 simulation calls — skip if quality ≥ 150) ──
                if (vs.qualityScore < 150.0) {
                    for (double dA : {-0.008, 0.008}) {
                        double testA = NumberUtils::normalizeDoublePrecision(vs.angle + dA);
                        gPrediction->determineShotResult(true, testA, vs.bestPower, 0.0, vs.cand);
                        bool ok = gPrediction->firstHitIsTarget &&
                                  gPrediction->guiData.balls[0].onTable &&
                                  !gPrediction->guiData.balls[vs.cand.idx].onTable &&
                                  gPrediction->guiData.balls[vs.cand.idx].pocketIndex == vs.cand.pocketIndex;
                        if (ok) vs.tolerancePts++;
                    }
                }

                // ── PERFECT SHOT LOCK: quality < 100 AND full tolerance 2/2 ──────────
                // Shoot immediately — do NOT allow further candidates to override.
                // Without this, a perfect shot (final score ~20) could be displaced
                // by a subsequent candidate that re-scores at 200+ due to snapshot drift.
                if (vs.qualityScore < 100.0 && vs.tolerancePts == 2) {
                    vs.qualityScore      -= 80.0;   // apply tolerance bonus
                    g_CurrentCandidate    = vs.cand;
                    g_dbg.candidatesFound = (int)validShots.size();
                    g_dbg.tolerancePts    = 2;
                    g_dbg.qualityScore    = (float)vs.qualityScore;
                    g_dbg.wonByTolerance  = false;
                    Shoot(vs.angle, vs.bestPower);
                    return;
                }
            }

            // Normal path: apply tolerance bonus and sort all remaining candidates
            for (auto& vs : validShots)
                vs.qualityScore -= vs.tolerancePts * 40.0f;

            std::stable_sort(validShots.begin(), validShots.end(),
                [](const ValidShot& a, const ValidShot& b) {
                    return a.qualityScore < b.qualityScore;
                });
        }

        // ── PHASE 4: Shoot the best candidate ─────────────────────────────────
        auto& best = validShots[0];
        g_CurrentCandidate = best.cand;

        g_dbg.candidatesFound = (int)validShots.size();
        g_dbg.tolerancePts    = best.tolerancePts;
        g_dbg.qualityScore    = (float)best.qualityScore;
        g_dbg.wonByTolerance  = !isNineBall && (validShots.size() > 1)
                                 && (best.tolerancePts > validShots[1].tolerancePts);

        Shoot(best.angle, best.bestPower);
    }

    bool isAnimationActive() {
        auto visualCue = sharedGameManager.mVisualCue();
        if (!visualCue) return true;
        auto _powerBarView = F(ptr, visualCue + 0x510);
        if (!_powerBarView) return true;
        uintptr_t activeAction = M(uintptr_t, libmain + 0x2de6f30, ptr)(_powerBarView);
        return (activeAction != 0);
    }

    void Update() {
        buttonClicker.Update();
        if (isAnimationActive()) return;

        if (!bAutoPlaying || !sharedGameManager.mStateManager().isPlayerTurn()) {
            if (humanState != HUM_IDLE) return;
            // Kalau sedang EXECUTING (nomination → shot), jangan reset
            g_CurrentCandidate.idx = -1;
            if (state == EXECUTING) return;
            NativeTouchesEnd(5, 0, 0);   // Joystick
            if (state != IDLE) {
                state = IDLE;
                scan = FAST;
                lastFailedCuePos = { -1000.0, -1000.0 };
                // Release snapshot — turn is over, game memory is stale
                g_useSnapshot        = false;
                g_sceneSnapshot.valid = false;
                
            }
            return;
        }

        if (state == IDLE) {
            state = SCANNING;
            scan = FAST;
            // Capture ball positions once per turn — all simulation calls in
            // ScanFast / ScanSlow reuse this snapshot instead of reading game
            // memory repeatedly, eliminating the per-frame FPS stutter.
            TakeSnapshot();
            ScanFast();  // Immediate — find shot in SAME frame, no 1-frame gap
        } else if (state == SCANNING) {
            // Safety net: reached only if ScanFast above failed (scan = SLOW)
            if (scan == SLOW) ScanSlow(0.005f);
        } else if (state == NOMINATING) {
            nominationFrameCounter++;
            if (nominationFrameCounter == 10) {
                buttonClicker.Click(GetPocketScreenPos(g_CurrentCandidate.pocketIndex));
            }
            if (nominationFrameCounter > 20 && !buttonClicker.Active) {
                uint nominatedPocket = sharedGameManager.getNominatedPocket();
                if (nominatedPocket == (uint)g_CurrentCandidate.pocketIndex) {
                    // Nominasi confirmed — re-validasi shot dengan full simulation
                    
                    gPrediction->determineShotResult(true, pendingShotAngle, pendingShotPower,
                                                     0.0, g_CurrentCandidate);
                    

                    // Scratch check setelah nominasi
                    if (!gPrediction->guiData.balls[0].onTable) {
                        LOGI("[AUTOPLAY] Post-nomination scratch detected, cancelling");
                        ClearState();
                        return;
                    }

                    // Update pocketIndex dari simulasi fresh
                    if (g_CurrentCandidate.idx >= 0 && g_CurrentCandidate.idx < gPrediction->guiData.ballsCount) {
                        int freshPocket = gPrediction->guiData.balls[g_CurrentCandidate.idx].pocketIndex;
                        if (freshPocket >= 0 && freshPocket < 6) {
                            g_CurrentCandidate.pocketIndex = freshPocket;
                        }
                    }

                    // Cek bola target masih valid ke pocket yang dinominasi
                    if (g_CurrentCandidate.pocketIndex != (int)nominatedPocket) {
                        LOGI("[AUTOPLAY] Target pocket mismatch after nomination, cancelling");
                        ClearState();
                        return;
                    }

                    // Start human state machine
                    
                    startAngle = sharedGameManager.mVisualCue().mVisualGuide().mAimAngle();
                    targetAngle = pendingShotAngle;
                    targetPower = pendingShotPower;
                    humanShotLocked = true;
                    humanState = HUM_THINKING;
                    stateStartTime = nowSec() + 0.3;
                    state = EXECUTING; // EXECUTING biar tidak di-reset oleh isPlayerTurn check
                } else {
                    // Retry setiap 30 frame, timeout 150 frame
                    if (nominationFrameCounter % 30 == 0) {
                        if (nominationFrameCounter > 150) {
                            LOGI("[AUTOPLAY] Nomination timeout, resetting");
                            ClearState();
                            lastFailedCuePos = gPrediction->guiData.balls[0].initialPosition;
                        } else {
                            LOGI("[AUTOPLAY] Nomination retry #%d", nominationFrameCounter / 30);
                            buttonClicker.Click(GetPocketScreenPos(g_CurrentCandidate.pocketIndex));
                        }
                    }
                }
            }
        }
            // ─── HUMAN STATE MACHINE ────────────────────────────────────────────
        // ─── HIDE PREDICTION LINES DURING HUMAN STATE ──────────────────────────
        if (humanState != HUM_IDLE) {
        double now = nowSec();

        auto UpdateJoystickVisuals = [&](double angle) {
            float jX = Width * 0.83f;
            float jY = Height * 0.82f;
            float jR = 65.0f;
            float tX = jX + cos(angle) * jR;
            float tY = jY + sin(angle) * jR;
            NativeTouchesMove(5, tX, tY);

            ImDrawList* fg = ImGui::GetForegroundDrawList();
    if (fg) {
        fg->AddCircleFilled(ImVec2(tX, tY), 10.0f, IM_COL32(255, 255, 255, 100));
        fg->AddCircle(ImVec2(tX, tY), 10.0f, IM_COL32(255, 255, 255, 200), 0.0f, 2.0f);
    }
        };
    
                // 1. HUM_THINKING (0.5s pause)
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
            gPrediction->determineShotResult(true, targetAngle, pendingShotPower, 0.0, g_CurrentCandidate);
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
            gPrediction->determineShotResult(true, targetAngle, pendingShotPower, 0.0, g_CurrentCandidate);
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
            gPrediction->determineShotResult(true, targetAngle, pendingShotPower, 0.0, g_CurrentCandidate);
            return;
        }

        // 4. STABILIZE & LOCK (0.4s) - joystick still held from HUM_HOLDING
        if (humanState == HUM_STABILIZING) {
            NativeTouchesMove(5, Width * 0.83f + cos(targetAngle) * 65.0f,
                                 Height * 0.82f + sin(targetAngle) * 65.0f);
            setAimAngle(targetAngle);
    
            if (now - stateStartTime >= 0.4) {
                NativeTouchesEnd(5, Width * 0.83f + cos(targetAngle) * 65.0f,
                                    Height * 0.82f + sin(targetAngle) * 65.0f);    
                stateStartTime = now;
                humanState = HUM_PULLING;
            }
            return;
        }

        // 5. POWER PULL (0.85s smooth) via simulated slider touch
        if (humanState == HUM_PULLING) {
            setAimAngle(targetAngle);

            gPrediction->determineShotResult(true, targetAngle, targetPower,
                                             0.0, g_CurrentCandidate);
                                            
            sharedGameManager.mVisualCue().mPower(ShotPowerToPower(targetPower));

            stateStartTime = now;
            humanState = HUM_DELAY_BEFORE_SHOT;
            return;
        }

        // 6. FINAL HUMAN PAUSE (0.4s) then FIRE!
        if (humanState == HUM_DELAY_BEFORE_SHOT) {
            setAimAngle(targetAngle);
            if (now - stateStartTime >= 0.4) {
                setAimAngle(targetAngle);
                sharedGameManager.mVisualCue().mPower(ShotPowerToPower(targetPower));
                // FIRE SHOT
                triggerShot();
                humanShotLocked = false;
                ClearState();
                state = IDLE; humanState = HUM_IDLE;
            }
            return;
        }
    }
    bool isPlayerTurn = sharedGameManager.mStateManager().isPlayerTurn();

    // PREDICTION LINES:
    // Di Prediction ini, lines digambar berdasarkan parameter `isAuto` di determineShotResult:
    //   isAuto=false → fastCalc=false → positions di-track → lines TAMPIL
    //   isAuto=true  → fastCalc=true  → positions tidak di-track → lines HILANG
    //
    // - Saat humanState aktif (lagi aiming/pulling): panggil isAuto=true → lines hilang
    // - Saat humanState HUM_IDLE (setelah shot selesai): panggil isAuto=false → lines tampil
    if (humanState != HUM_IDLE) {
        // Lines hilang selama human state machine jalan.
        // Pakai 0.0 supaya simulasi konsisten dengan yang dipilih saat scan.
        gPrediction->determineShotResult(true, targetAngle, targetPower, 0.0);
    } else if (isPlayerTurn && g_CurrentCandidate.idx == -1) {
        // Setelah shot selesai: unlock spin supaya scan berikutnya bisa lock fresh.
        
        // Lines tampil lagi, ikuti aim angle real-time.
        if (gPrediction && sharedGameManager) {
            double curAngle = sharedGameManager.mVisualCue().mVisualGuide().mAimAngle();
            // FIX POWER: mPower() return skala game (0.0–1.0), sedangkan
            // determineShotResult expect skala simulasi (0–666 = langsung velocity).
            // getShotPower() sudah return skala simulasi yang benar.
            // mPower() mentah menyebabkan display lines pakai power jauh lebih kecil
            // dari yang dipakai saat scan → trajectory berbeda → lines meleset.
            double curPower = sharedGameManager.mVisualCue().getShotPower();
            if (curPower < 10.0) curPower = 400.0; // fallback kalau belum ada power
            gPrediction->determineShotResult(false, curAngle, curPower, 0.0);
        }
    }
    }
};

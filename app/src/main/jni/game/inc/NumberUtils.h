#pragma once

#include <cmath>

#include <Vector/Vectors.h>

#include "GameConstants.h"

#define NAN std::isnan

static constexpr double DAT_04c8b998 = 0.0; // F(double, libmain + 0x4c8b998) // v56.13.0
static constexpr double DAT_04c8bc98 = 4.71238898038; // F(double, libmain + 0x4c8bc98) // v56.13.0
static constexpr double DAT_04c8ba00 = 1.57079632679; // F(double, libmain + 0x4c8ba00) // v56.13.0
static constexpr double DAT_04c8b9f8 = 3.14159265359; // F(double, libmain + 0x4c8b9f8) // v56.13.0

void FUN_02b1bfc0(double *param_1,const Vector2D *param_2) {
    double *pdVar1;
    double dVar2;
    
    dVar2 = param_2->y;
    if (param_2->x == DAT_04c8b998) {
        pdVar1 = (double *)&DAT_04c8bc98;
        if (dVar2 < DAT_04c8b998 == (NAN(dVar2) || NAN(DAT_04c8b998))) {
            pdVar1 = (double *)&DAT_04c8ba00;
        }
        dVar2 = *pdVar1;
    }
    else {
        dVar2 = atan(dVar2 / param_2->x);
        dVar2 = round(dVar2 * 10000.0);
        dVar2 = dVar2 / 10000.0;
        if (param_2->x < DAT_04c8b998) {
            dVar2 = dVar2 + DAT_04c8b9f8;
        }
    }
    *param_1 = dVar2;
    return;
}


namespace NumberUtils {
    // sub_1C2B0D8 5.8.0 angle, power, spin, ball positions??? should be truncated
    inline double normalizeDoublePrecision(double value, double negativeThreshold = 0.0, double negativeExtraLen = 0.0, size_t maxLen = 7) {
        return std::round(value * 10000.0) / 10000.0; // AIMX Style: 4 decimal places precision
    }

    double calcAngle(const Vec2d& delta) {
        double angle;
        FUN_02b1bfc0(&angle, &delta); // USE ENGINE MATH: Fixes bounce mismatch
        return angle;
    }

    inline double calcAngle(Vec2d source, Vec2d Destination) { return calcAngle(source - Destination); }
}

double ShotPowerToPower(double shotPower) {
    auto maxPower = CUE_PROPERTIES_MAX_POWER;
    double ratio = 1.0 - (shotPower / maxPower);
    double power = 1.0 - ratio * ratio;
    return NumberUtils::normalizeDoublePrecision(power);
}

// The engine never stores the shot speed we ask for. setPower(s) writes
// ShotPowerToPower(s) into VisualCue::mPower, and that is rounded to 4 decimals;
// the cue then reads it back through the inverse (VisualCue::getShotPower).
// The round trip is therefore LOSSY, and the loss grows sharply toward full
// power because the inverse carries a sqrt:
//     s' = (1 - sqrt(1 - p)) * M     =>     ds = M * dp / (2 * sqrt(1 - p))
// With M = 666 and dp = 5e-5 (half a quantum) that is 0.02 speed units at half
// power but ~0.5 near the top of the bar. Simulating with the raw `s` while the
// game fires `s'` makes the cue ball stop slightly PAST the drawn point, and
// only at high power - which is where the Beast AI lives (its grid tops out at
// powerMax and it prefers the hardest power that pots the most balls).
//
// Rounding `s` itself, which is what Shoot() used to do, cannot fix this: `s` is
// in the hundreds, so snapping it to 1e-4 is a no-op. The quantisation is in `p`.
// Quantise FIRST, then simulate and fire the exact same number.
inline double QuantizeShotPower(double shotPower) {
    double maxPower = CUE_PROPERTIES_MAX_POWER;
    if (maxPower <= 0.0) return shotPower;
    double p = ShotPowerToPower(shotPower); // what setPower() will actually store
    if (p <= 0.0) return 0.0;
    if (p > 1.0) p = 1.0;
    return (1.0 - sqrt(1.0 - p)) * maxPower; // what the cue will read back out
}
#pragma once

#include "Types.h"
#include "Foundation.h"
#include "CCNode.h"
#include "Ball.h"
#include <cmath>
#include "inc/NumberUtils.h"

struct VisualGuide : Instance { // objCType
    Field<0x28, double> mAimAngle; // VisualCue::aimAngle()
    Field<0xa0, ptr> mClassification; // isVisualGuidePointingToWrongBallClassification

    VisualGuide(ptr instance = 0) : Instance(instance), mAimAngle(instance), mClassification(instance) {}
};

struct VisualCue : CCNode {
    Field<0x3a8, VisualGuide> mVisualGuide;
    Field<0x3b0, double> mPower;  // 🎱 Internal power variable (0.0-1.0 normalized)

    VisualCue(ptr instance = 0) : CCNode(instance), mVisualGuide(instance), mPower(instance) {}

    double getShotAngle() {
        auto angle = mVisualGuide().mAimAngle();
        // angle = round(angle * 10000.0) / 10000.0;
        return NumberUtils::normalizeDoublePrecision(angle);
    }
    
    double getShotPower(bool strict = false) {
        auto power = mPower();
        if (strict && power <= 0.0) return 0.0;

        if (power <= 0.0 || power > 1.0) power = 1.f;
        else power = NumberUtils::normalizeDoublePrecision(power);

        auto maxPower = CUE_PROPERTIES_MAX_POWER;
        return (1.0 - sqrt(1.0 - power * maxPower / maxPower)) * maxPower;
    }
    
    // 🎱 NEW: Set shot power with proper conversion
    // Converts from actual power value (0-666) to internal normalized value (0.0-1.0)
    void setShotPower(double actualPower) {
        // Clamp to valid range
        if (actualPower < 0.0) actualPower = 0.0;
        if (actualPower > CUE_PROPERTIES_MAX_POWER) actualPower = CUE_PROPERTIES_MAX_POWER;
        
        // Inverse of getShotPower formula: actualPower = (1.0 - sqrt(1.0 - normalizedPower)) * maxPower
        // Solving for normalizedPower:
        // actualPower / maxPower = 1.0 - sqrt(1.0 - normalizedPower)
        // sqrt(1.0 - normalizedPower) = 1.0 - (actualPower / maxPower)
        // 1.0 - normalizedPower = [1.0 - (actualPower / maxPower)]²
        // normalizedPower = 1.0 - [1.0 - (actualPower / maxPower)]²
        
        auto maxPower = CUE_PROPERTIES_MAX_POWER;
        double ratio = actualPower / maxPower;
        double oneMinus = 1.0 - ratio;
        double normalizedPower = 1.0 - (oneMinus * oneMinus);
        
        // Apply normalization and bounds checking
        normalizedPower = NumberUtils::normalizeDoublePrecision(normalizedPower);
        if (normalizedPower < 0.0) normalizedPower = 0.0;
        if (normalizedPower > 1.0) normalizedPower = 1.0;
        
        // Set the internal field
        mPower(normalizedPower);
    }
    
    // 🎱 Alternative: Set power using ShotPowerToPower conversion
    // Use this if you already have a converted power value
    void setPowerDirect(double normalizedPower) {
        // Direct assignment for pre-converted values
        if (normalizedPower < 0.0) normalizedPower = 0.0;
        if (normalizedPower > 1.0) normalizedPower = 1.0;
        
        normalizedPower = NumberUtils::normalizeDoublePrecision(normalizedPower);
        mPower(normalizedPower);
    }
    
    // 🎱 Utility: Get the internal normalized power value (0.0-1.0)
    double getInternalPower() {
        auto power = mPower();
        if (power < 0.0) return 0.0;
        if (power > 1.0) return 1.0;
        return power;
    }
    
    operator bool() { return instance && this->isInstanceOf("VisualCue"); }
};


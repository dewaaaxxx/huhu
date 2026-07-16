#pragma once

#include <cmath>
#include <algorithm>

// 🎱 Enhanced Physical Model for 8 Ball Pool
namespace Physics {
    // Ball physics constants
    constexpr double BALL_RADIUS = 0.028575; // Standard 8-ball radius (57.15mm diameter)
    constexpr double TABLE_FRICTION = 0.1; // Sliding friction coefficient
    constexpr double ROLLING_FRICTION = 0.01; // Rolling friction
    constexpr double RESTITUTION = 0.95; // Coefficient of restitution (bounciness)
    constexpr double SPIN_FACTOR = 0.5; // How much spin affects ball trajectory
    
    // Table boundaries (in game units)
    struct TableBounds {
        double minX, maxX;
        double minY, maxY;
        
        bool isInBounds(double x, double y) const {
            return x >= minX && x <= maxX && y >= minY && y <= maxY;
        }
    };
    
    // Ball collision response
    struct CollisionResponse {
        double velocityX, velocityY;
        double spinX, spinY;
        bool isColliding;
    };
    
    // Calculate velocity after friction
    inline double applyFriction(double velocity, double deltaTime, double frictionCoeff) {
        double deceleration = 9.81 * frictionCoeff; // gravity-based friction
        velocity -= deceleration * deltaTime;
        return std::max(0.0, velocity);
    }
    
    // Calculate ball-to-ball collision response
    inline CollisionResponse calculateBallCollision(
        double ball1VelX, double ball1VelY,
        double ball2VelX, double ball2VelY,
        double ball1SpinX = 0.0, double ball1SpinY = 0.0
    ) {
        // Simplified elastic collision for equal mass balls
        double vxDiff = ball1VelX - ball2VelX;
        double vyDiff = ball1VelY - ball2VelY;
        
        // After collision, balls exchange velocities partially
        double transferFactor = (1.0 + RESTITUTION) * 0.5;
        
        return {
            ball1VelX - vxDiff * transferFactor,
            ball1VelY - vyDiff * transferFactor,
            ball1SpinX * 0.8,  // Spin reduces after collision
            ball1SpinY * 0.8,
            true
        };
    }
    
    // Calculate wall collision
    inline CollisionResponse calculateWallCollision(
        double velX, double velY,
        bool hitXWall = false, bool hitYWall = false
    ) {
        return {
            hitXWall ? -velX * RESTITUTION : velX,
            hitYWall ? -velY * RESTITUTION : velY,
            0.0, 0.0,
            true
        };
    }
    
    // Apply spin to velocity
    inline void applySpin(double& velX, double& velY, double spinX, double spinY) {
        velX += spinX * SPIN_FACTOR * 0.1;
        velY += spinY * SPIN_FACTOR * 0.1;
    }
    
    // Calculate required power for distance
    inline double calculatePowerForDistance(double distance) {
        // Physics: v² = 2 * a * s
        // a = friction deceleration
        double deceleration = 9.81 * TABLE_FRICTION;
        double requiredVelocity = std::sqrt(2.0 * deceleration * distance);
        return requiredVelocity;
    }
    
    // Calculate optimal angle considering deflection
    inline double calculateDeflectionAngle(
        double distance,
        double cueBallX, double cueBallY,
        double targetX, double targetY
    ) {
        double dx = targetX - cueBallX;
        double dy = targetY - cueBallY;
        
        double angle = std::atan2(dy, dx);
        
        // Account for deflection due to spin/friction at extreme distances
        if (distance > 2.0) {
            double deflectionFactor = std::tanh(distance / 5.0) * 0.1; // Small deflection for long shots
            angle += deflectionFactor;
        }
        
        return angle;
    }
    
    // Validate shot based on physics
    inline bool isValidShot(double power, double maxPower = 666.0) {
        if (power > maxPower) return false;
        return true;
    }
}

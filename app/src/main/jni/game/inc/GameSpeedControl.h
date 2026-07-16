#pragma once

#include <imgui/imgui.h>
#include <map>
#include <string>

using namespace ImGui;

namespace GameSpeed {
    enum SpeedMode {
        SLOW = 0,
        NORMAL = 1,
        FAST = 2,
        VERY_FAST = 3
    };
    
    struct SpeedSettings {
        const char* name;
        float animationMultiplier;
        float physicsTimeStep;
        float shotExecutionDelay; // milliseconds
    };
    
    static const SpeedSettings SPEED_CONFIGS[] = {
        { "🐢 Slow",      0.5f,  0.016f, 800.0f },   // 50% speed
        { "▶️ Normal",     1.0f,  0.016f, 500.0f },   // 100% speed
        { "⏩ Fast",       1.5f,  0.012f, 300.0f },   // 150% speed
        { "⚡ Very Fast",  2.0f,  0.008f, 150.0f }    // 200% speed
    };
    
    static SpeedMode currentSpeed = NORMAL;
    static float speedAnimTime = 0.0f;
    
    inline float getAnimationMultiplier() {
        return SPEED_CONFIGS[currentSpeed].animationMultiplier;
    }
    
    inline float getPhysicsTimeStep() {
        return SPEED_CONFIGS[currentSpeed].physicsTimeStep;
    }
    
    inline float getShotExecutionDelay() {
        return SPEED_CONFIGS[currentSpeed].shotExecutionDelay;
    }
    
    inline const char* getCurrentSpeedName() {
        return SPEED_CONFIGS[currentSpeed].name;
    }
    
    inline void setSpeed(SpeedMode mode) {
        currentSpeed = mode;
        speedAnimTime = ImGui::GetTime();
        LOGI("Game speed changed to: %s", getCurrentSpeedName());
    }
    
    inline void Draw() {
        ImGuiIO& io = GetIO();
        float padding = 20.0f;
        float buttonWidth = 70.0f;
        float buttonHeight = 40.0f;
        
        SetNextWindowPos(ImVec2(io.DisplaySize.x - padding - (buttonWidth * 4 + 15), padding), ImGuiCond_Always);
        SetNextWindowSize(ImVec2(buttonWidth * 4 + 15, buttonHeight + 20), ImGuiCond_Always);
        
        PushStyleColor(ImGuiCol_WindowBg, IM_COL32(20, 20, 30, 220));
        PushStyleColor(ImGuiCol_Border, IM_COL32(100, 200, 255, 180));
        PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
        PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
        PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
        
        if (Begin("##SpeedControl", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
            
            for (int i = 0; i < 4; i++) {
                if (i > 0) SameLine(0, 5);
                
                bool isSelected = (currentSpeed == i);
                
                if (isSelected) {
                    PushStyleColor(ImGuiCol_Button, IM_COL32(0, 150, 255, 255));
                    PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(50, 180, 255, 255));
                } else {
                    PushStyleColor(ImGuiCol_Button, IM_COL32(50, 50, 70, 200));
                    PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 80, 100, 220));
                }
                
                PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
                
                if (Button(SPEED_CONFIGS[i].name, ImVec2(buttonWidth - 5, buttonHeight))) {
                    setSpeed((SpeedMode)i);
                }
                
                PopStyleVar();
                PopStyleColor(2);
            }
        }
        End();
        
        PopStyleVar(3);
        PopStyleColor(2);
    }
}

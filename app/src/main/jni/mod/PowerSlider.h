#pragma once

#include "include/input.h"

extern struct Candidate;

// g_CurrentCandidate / lastFailedCuePos / IsShotValid() are defined inside
// namespace AutoPlay (game/inc/AutoPlay.h, included before this file via
// AutoPlay.impl.h). The old `extern ...;` declarations here pointed at
// separate, never-defined GLOBAL symbols with the same names — that's what
// produced "ld: error: undefined symbol: g_CurrentCandidate" etc. at link
// time. Use the real, namespaced ones (AutoPlay::...) below instead.

#define ifl(cond) if ([&](){ bool b = (cond); if (b) LOGI(#cond); return b; }())
// #define ifln(cond) if ([&](){ bool b = (cond); if (!b) LOGI("!("#cond")"); return b; }())

struct PowerSlider {
    bool Active = false;
    float ElapsedTime = 0.f, Duration = 0.f;
    float HoldTime = 0.f, HoldDuration = 0.f;
    ImVec2 StartPos;
    ImVec2 EndPos; // Max drag position
    ImVec2 TargetPos; // Actual target based on power
    ImVec2 CurrentPos;

    float ShotPower = 666.0f;
    int TouchIndex = 10; // High touch index

    enum State {
        IDLE,
        STARTING,
        MOVING,
        ENDING,
        RETURNING,
    } state = IDLE;
    
    void Start(ImVec2 start, ImVec2 end) {
        this->StartPos = start;
        this->EndPos = end;
        this->CurrentPos = this->StartPos;
        this->ElapsedTime = 0.f;
        this->HoldTime = 0.f;

        this->Active = true;
        this->state = STARTING;
    }
    
    void Start(ImVec4 Rect) {
        float center = Rect.x + Rect.z / 2.0f;
        Start(ImVec2(center, Rect.y), ImVec2(center, Rect.y + Rect.w));
    }

    void End() {
        LOGI("ending at power %f", sharedGameManager.mVisualCue().getShotPower(true));
        NativeTouchesEnd(this->TouchIndex, this->CurrentPos.x, this->CurrentPos.y);
        this->Active = false;
        this->state = IDLE;
        AutoPlay::g_CurrentCandidate.idx = -1;
    }

    void Cancel() {
        LOGI("Canceling power slider at power %f", sharedGameManager.mVisualCue().getShotPower(true));
        // NativeTouchesMove(this->TouchIndex, this->StartPos.x, this->StartPos.y);
        // this->End();
        this->EndPos = this->CurrentPos; // Use EndPos as start of return animation
        this->TargetPos = this->StartPos; // Return to origin
        this->ElapsedTime = 0.f;
        this->HoldTime = 0.f;
        this->Duration = 0.3f; // Fast return
        this->state = RETURNING;

        AutoPlay::g_CurrentCandidate.idx = -1;
        AutoPlay::lastFailedCuePos = { -1000.0, -1000.0 };

    }
    
    // DragTime is time to reach MAX power (666.0f)
    void SimulateDrag(ImVec4 Rect, float ShotPower = 0.f, float DragTime = .7f, float HoldTime = 0.35f) {
        if (this->Active) return;

        // BUG (found by comparing against a known-working reference source):
        // this line forced every shot to full power regardless of the
        // requested ShotPower argument, and — combined with the missing
        // mPower() calls below — meant the ONLY thing driving the shot was
        // a simulated touch drag the game's own gesture handler had to
        // interpret correctly on its own, with nothing setting the cue's
        // actual power value directly. Removed: honor the requested power.
        this->ShotPower = ShotPower > 0.f ? ShotPower : 666.0f;
        if (this->ShotPower > 666.0f) this->ShotPower = 666.0f;
        float powerRatio = (float)ShotPowerToPower((double)this->ShotPower);
        
        Start(Rect);
        
        // Calculate exact target position for the requested power
        this->TargetPos = ImVec2(
            this->StartPos.x + (this->EndPos.x - this->StartPos.x) * powerRatio,
            this->StartPos.y + (this->EndPos.y - this->StartPos.y) * powerRatio
        );
        
        this->Duration = DragTime * powerRatio;
        this->HoldDuration = HoldTime;
    }

    void Update() {
        if (!this->Active) return;
        
        float dt = ImGui::GetIO().DeltaTime;
        
        if (this->state == STARTING) {
            this->HoldTime += dt;
            if (this->HoldTime >= this->HoldDuration * 2.f) {
                NativeTouchesBegin(this->TouchIndex, this->StartPos.x, this->StartPos.y);
                this->state = MOVING;
                this->HoldTime = 0.f;
            }
        }
        
        if (this->state == MOVING) {
            this->ElapsedTime += dt;
            
            if (this->ElapsedTime < this->Duration) {
                // Calculate interpolated position
                float t = this->ElapsedTime / this->Duration;
                this->CurrentPos = ImVec2(
                    this->StartPos.x + (this->TargetPos.x - this->StartPos.x) * t,
                    this->StartPos.y + (this->TargetPos.y - this->StartPos.y) * t
                );

                NativeTouchesMove(this->TouchIndex, this->CurrentPos.x, this->CurrentPos.y);
                // Drive the cue's actual power value directly, in step with
                // the drag - this was missing entirely before. Without it,
                // firing the shot depended 100% on the game's own gesture
                // handler correctly reading the simulated touch positions as
                // a power drag; if it didn't (e.g. it wants continuous real
                // touch events, not just begin/move/end at these coordinates),
                // the release fired at whatever power the cue already had -
                // typically none, which reads as "the stick wasn't pulled".
                if (sharedGameManager && sharedGameManager.mVisualCue()) {
                    sharedGameManager.mVisualCue().mPower(ShotPowerToPower((double)(this->ShotPower * t)));
                }
            } else {
                // Ensure we hit the target exactly
                this->CurrentPos = this->TargetPos;
                NativeTouchesMove(this->TouchIndex, this->CurrentPos.x, this->CurrentPos.y);
                if (sharedGameManager && sharedGameManager.mVisualCue()) {
                    sharedGameManager.mVisualCue().mPower(ShotPowerToPower((double)this->ShotPower));
                }
                this->state = ENDING;
            }

            if (dynamic_bool["DebugTouch"]) {
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                fg->AddCircleFilled(this->CurrentPos, 15.0f, IM_COL32(255, 255, 255, 100)); // Semi-transparent white circle
                fg->AddCircle(this->CurrentPos, 15.0f, IM_COL32(255, 255, 255, 200), 0.0f, 2.0f); // White outline
            }
        }

        if (this->state == ENDING) {
            this->HoldTime += dt;
            // Keep power locked at target while holding, same as MOVING's
            // final frame - a stray frame between phases shouldn't let it
            // drift back to whatever the game had before we started.
            if (sharedGameManager && sharedGameManager.mVisualCue()) {
                sharedGameManager.mVisualCue().mPower(ShotPowerToPower((double)this->ShotPower));
            }
            if (this->HoldTime >= this->HoldDuration) {
                if (AutoPlay::IsShotValid()) {
                    this->End();
                } else {
                    LOGI("Shot invalid before release. Canceling.");
                    this->Cancel();
                }
            }
        }

        if (this->state == RETURNING) {
            this->ElapsedTime += dt;

            if (this->ElapsedTime < this->Duration) {
                float t = this->ElapsedTime / this->Duration;
                // Interpolate from EndPos (captured CurrentPos) to TargetPos (StartPos)
                this->CurrentPos = ImVec2(
                    this->EndPos.x + (this->TargetPos.x - this->EndPos.x) * t,
                    this->EndPos.y + (this->TargetPos.y - this->EndPos.y) * t
                );
                NativeTouchesMove(this->TouchIndex, this->CurrentPos.x, this->CurrentPos.y);
            } else {
                // Finished returning
                this->CurrentPos = this->TargetPos;
                NativeTouchesMove(this->TouchIndex, this->CurrentPos.x, this->CurrentPos.y);
                End();
            }

            if (dynamic_bool["DebugTouch"]) {
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                fg->AddCircleFilled(this->CurrentPos, 15.0f, IM_COL32(255, 255, 255, 100));
                fg->AddCircle(this->CurrentPos, 15.0f, IM_COL32(255, 255, 255, 200), 0.0f, 2.0f);
            }
        }

    }
};

PowerSlider powerSlider;

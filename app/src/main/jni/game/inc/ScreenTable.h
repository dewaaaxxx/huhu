#pragma once

// Reference resolution from the game (1280x640)
inline constexpr double REF_WIDTH = 1280.0;
inline constexpr double REF_HEIGHT = 640.0;

// Table anchor points in reference coordinates
inline constexpr double REF_TABLE_LEFT = 207.0;
inline constexpr double REF_TABLE_RIGHT = 1072.0;
inline constexpr double REF_TABLE_TOP = 171.0;
inline constexpr double REF_TABLE_BOTTOM = 584.0;

// Calculated table dimensions in reference space
inline constexpr double REF_TABLE_WIDTH = REF_TABLE_RIGHT - REF_TABLE_LEFT;  // 865.0

inline double TABLE_LEFT = 0.0;
inline double TABLE_TOP = 0.0;
inline double TABLE_RIGHT = 0.0;
inline double TABLE_BOTTOM = 0.0;
inline double TABLE_SCALE = 1.0;   // X scale (kept for compatibility)
inline double TABLE_SCALE_X = 1.0; // Pixels per world-unit, horizontal
inline double TABLE_SCALE_Y = 1.0; // Pixels per world-unit, vertical (separate: screen is NOT exactly 2:1)

ImVec2 WorldToScreen(Vec2d worldPos) {
    double positionX = worldPos.x + TABLE_HALF_WIDTH;
    double positionY = -(worldPos.y + TABLE_HALF_HEIGHT);
    double scrX = TABLE_LEFT + positionX * TABLE_SCALE_X;
    double scrY = TABLE_BOTTOM + positionY * TABLE_SCALE_Y;
    return ImVec2(scrX, scrY);
}

void UpdateScreenTable() {
    // Calculate scale based on screen height (matches Java implementation)
    double heightScale = Height / REF_HEIGHT;
    
    // Calculate horizontal offset for centering
    double scaledRefWidth = heightScale * REF_WIDTH;
    double offsetX = (Width - scaledRefWidth) / 2.0;
    
    // Apply scaling and centering to anchor points
    TABLE_LEFT = offsetX + (heightScale * REF_TABLE_LEFT);
    TABLE_RIGHT = offsetX + (heightScale * REF_TABLE_RIGHT);
    TABLE_TOP = heightScale * REF_TABLE_TOP;
    TABLE_BOTTOM = heightScale * REF_TABLE_BOTTOM;
    
    // Single UNIFORM scale for both axes: the physics world is square, so one
    // world unit must span the same pixels horizontally and vertically.
    //
    // Deriving Y separately as (TABLE_BOTTOM - TABLE_TOP) / TABLE_HEIGHT gives
    // 413/127 = 3.2520 px/unit against X's 865/254 = 3.4055 -- a 4.7% vertical
    // squash. TABLE_HEIGHT (127) is the cushion-face-to-cushion-face span, but
    // the REF_TABLE_TOP/BOTTOM anchors bracket the visible cloth, whose true
    // vertical span is 413/3.4055 = 121.3 units, not 127. Dividing by the wrong
    // span shrank every Y toward the table centre, and the error grows with
    // distance from it: at the middle-pocket mouth (y = 76.7) the drawn point
    // landed ~11.8 px low, so the whole top of the overlay -- rails, jaws,
    // pocket rings, prediction line -- sagged downward.
    //
    // The pixel-space aspect difference is already baked into the REF anchors,
    // so X's ratio is the correct one for both axes. All five reference
    // projects use this single uniform scale.
    TABLE_SCALE = (TABLE_RIGHT - TABLE_LEFT) / TABLE_WIDTH;
    TABLE_SCALE_X = TABLE_SCALE;
    TABLE_SCALE_Y = TABLE_SCALE;
}
void DrawEightBallLoading(ImDrawList* draw_list) {
    // Animation removed — rotation is now handled directly in AutoPlay.h (state == SCANNING/SLOW)
    // via g_toggleRotAngle, which rotates the DrawToggleButton icon in place.
    (void)draw_list;
}

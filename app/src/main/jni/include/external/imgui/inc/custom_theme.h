#pragma once

#include "../src/imgui.h"

void StyleColorsCustom(ImGuiStyle* _style = nullptr);

static int current_theme = 0;

static const char* themes[] = { "Custom", "Dark", "Light", "Classic" };

static void switch_theme(int current_theme) {
    switch (current_theme) {
        case 0: StyleColorsCustom(); break;
        case 1: ImGui::StyleColorsDark(); break;
        case 2: ImGui::StyleColorsLight(); break;
        case 3: ImGui::StyleColorsClassic(); break;
    }
    
    ImGuiStyle& style = ImGui::GetStyle();
    style.TouchExtraPadding = ImVec2(10.0f, 10.0f);
    style.ItemSpacing = ImVec2(12, 8);
    style.ItemInnerSpacing = ImVec2(10, 6);
    style.FramePadding = ImVec2(12, 6);
}

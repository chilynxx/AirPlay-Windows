#pragma once

#include "imgui.h"
#include <algorithm>
#include <cmath>

// Apple-inspired minimalist vector UI kit (100% trademark-safe custom geometric monochrome icons).
// Supports both Obsidian Dark Mode and Crisp Alabaster Light Mode.
namespace ap::video::receiver_ui {

struct ThemePalette {
    ImU32 bg_window;
    ImU32 bg_stage;
    ImU32 bg_sidebar;
    ImU32 bg_card;
    ImU32 bg_card_hover;
    ImU32 bg_glass_dock;
    ImU32 bg_pill_active;
    ImU32 bg_pill_idle;
    ImU32 border_line;
    ImU32 border_glass;
    ImU32 text_ink;
    ImU32 text_muted;
    ImU32 text_subtle;
    ImU32 accent_blue;
    ImU32 accent_blue_soft;
    ImU32 status_green;
    ImU32 status_green_bg;
    ImU32 status_amber;
    ImU32 status_amber_bg;
    ImU32 status_red;
};

inline ThemePalette get_palette(bool dark_mode) noexcept {
    if (dark_mode) {
        return ThemePalette{
            IM_COL32(13, 15, 19, 255),     // bg_window (Obsidian)
            IM_COL32(17, 20, 25, 255),     // bg_stage
            IM_COL32(14, 17, 22, 255),     // bg_sidebar
            IM_COL32(25, 29, 37, 242),     // bg_card
            IM_COL32(34, 39, 50, 250),     // bg_card_hover
            IM_COL32(20, 24, 31, 228),     // bg_glass_dock (Frosted translucent)
            IM_COL32(59, 130, 246, 255),   // bg_pill_active
            IM_COL32(32, 37, 47, 235),     // bg_pill_idle
            IM_COL32(40, 46, 58, 255),     // border_line
            IM_COL32(255, 255, 255, 30),   // border_glass (Specular highlight)
            IM_COL32(243, 245, 249, 255),  // text_ink
            IM_COL32(148, 158, 176, 255),  // text_muted
            IM_COL32(104, 114, 132, 255),  // text_subtle
            IM_COL32(59, 130, 246, 255),   // accent_blue
            IM_COL32(30, 58, 110, 140),    // accent_blue_soft
            IM_COL32(16, 185, 129, 255),   // status_green
            IM_COL32(16, 185, 129, 32),    // status_green_bg
            IM_COL32(245, 158, 11, 255),   // status_amber
            IM_COL32(245, 158, 11, 34),    // status_amber_bg
            IM_COL32(239, 68, 68, 255)     // status_red
        };
    }
    return ThemePalette{
        IM_COL32(250, 251, 253, 255),      // bg_window (Crisp Alabaster)
        IM_COL32(243, 245, 248, 255),      // bg_stage
        IM_COL32(250, 251, 253, 255),      // bg_sidebar
        IM_COL32(255, 255, 255, 250),      // bg_card
        IM_COL32(238, 242, 248, 255),      // bg_card_hover
        IM_COL32(255, 255, 255, 235),      // bg_glass_dock
        IM_COL32(37, 99, 235, 255),        // bg_pill_active
        IM_COL32(236, 240, 246, 255),      // bg_pill_idle
        IM_COL32(224, 228, 236, 255),      // border_line
        IM_COL32(205, 212, 224, 180),      // border_glass
        IM_COL32(24, 28, 35, 255),         // text_ink
        IM_COL32(104, 114, 128, 255),      // text_muted
        IM_COL32(142, 151, 164, 255),      // text_subtle
        IM_COL32(37, 99, 235, 255),        // accent_blue
        IM_COL32(226, 236, 255, 255),      // accent_blue_soft
        IM_COL32(16, 163, 112, 255),       // status_green
        IM_COL32(225, 248, 238, 255),      // status_green_bg
        IM_COL32(194, 120, 14, 255),       // status_amber
        IM_COL32(255, 246, 225, 255),      // status_amber_bg
        IM_COL32(220, 53, 69, 255)         // status_red
    };
}

// Legacy color aliases for compatibility
constexpr ImU32 ink   = IM_COL32(32, 38, 41, 255);
constexpr ImU32 muted = IM_COL32(115, 123, 128, 255);
constexpr ImU32 blue  = IM_COL32(59, 130, 246, 255);
constexpr ImU32 line  = IM_COL32(230, 232, 231, 255);
constexpr ImU32 paper = IM_COL32(245, 246, 244, 255);

enum class Icon {
    Cast,
    Home,
    Music,
    Screen,
    Settings,
    Phone,
    Previous,
    Next,
    Play,
    Pause,
    Close,
    Minimize,
    Maximize,
    Fullscreen,
    Record,
    Camera,
    Refresh,
    Rotate,
    Volume,
    VolumeMute,
    Wifi,
    Usb,
    Android,
    Globe,
    Theme
};

inline void icon(ImDrawList* d, Icon kind, ImVec2 p, float size, ImU32 color) {
    auto v = [&](float x, float y) { return ImVec2(p.x + x * size / 24.0f, p.y + y * size / 24.0f); };
    auto stroke = [&](float x, float y, float xx, float yy, float thickness = 1.65f) {
        d->AddLine(v(x, y), v(xx, yy), color, thickness);
    };
    switch (kind) {
    case Icon::Cast:
        d->AddRect(v(3, 4), v(21, 16), color, 2.5f, 0, 1.65f);
        d->AddTriangleFilled(v(12, 13), v(7, 20), v(17, 20), color);
        break;
    case Icon::Home:
        stroke(3, 10.5f, 12, 3.5f); stroke(12, 3.5f, 21, 10.5f);
        stroke(5.5f, 9.5f, 5.5f, 20.5f); stroke(5.5f, 20.5f, 18.5f, 20.5f); stroke(18.5f, 20.5f, 18.5f, 9.5f);
        d->AddRect(v(9.8f, 14), v(14.2f, 20.5f), color, 1.0f, 0, 1.5f);
        break;
    case Icon::Music:
        stroke(9, 18, 9, 5); stroke(9, 5, 20, 3); stroke(20, 3, 20, 16); stroke(9, 8, 20, 6);
        d->AddCircleFilled(v(6.5f, 18), size * 0.11f, color, 16);
        d->AddCircleFilled(v(17.5f, 16), size * 0.11f, color, 16);
        break;
    case Icon::Screen:
        d->AddRect(v(2.5f, 3.5f), v(21.5f, 16.5f), color, 2.5f, 0, 1.65f);
        stroke(12, 16.5f, 12, 20.5f); stroke(7.5f, 20.5f, 16.5f, 20.5f);
        break;
    case Icon::Settings: {
        ImVec2 outline[40];
        constexpr float angles[] = {-22.5f, -15.f, -11.f, 11.f, 15.f};
        constexpr float radii[]  = {7.5f, 7.5f, 10.f, 10.f, 7.5f};
        for (int i = 0; i < 8; ++i) {
            for (int j = 0; j < 5; ++j) {
                const float a = (i * 45 + angles[j]) * 3.14159265f / 180.0f;
                outline[i * 5 + j] = v(12 + radii[j] * std::cos(a), 12 + radii[j] * std::sin(a));
            }
        }
        d->AddPolyline(outline, 40, color, ImDrawFlags_Closed, 1.5f);
        d->AddCircle(v(12, 12), size * 0.13f, color, 24, 1.5f);
        break;
    }
    case Icon::Phone:
        d->AddRect(v(6.5f, 2.5f), v(17.5f, 21.5f), color, 3.0f, 0, 1.65f);
        stroke(10.5f, 5.0f, 13.5f, 5.0f);
        d->AddCircleFilled(v(12.0f, 18.8f), size * 0.045f, color, 12);
        break;
    case Icon::Previous:
        d->AddTriangleFilled(v(18, 5), v(8, 12), v(18, 19), color);
        stroke(5.5f, 5.5f, 5.5f, 18.5f);
        break;
    case Icon::Next:
        d->AddTriangleFilled(v(6, 5), v(16, 12), v(6, 19), color);
        stroke(18.5f, 5.5f, 18.5f, 18.5f);
        break;
    case Icon::Play:
        d->AddTriangleFilled(v(8.5f, 5), v(19.5f, 12), v(8.5f, 19), color);
        break;
    case Icon::Pause:
        d->AddRectFilled(v(7, 5), v(10.2f, 19), color, 1.2f);
        d->AddRectFilled(v(13.8f, 5), v(17, 19), color, 1.2f);
        break;
    case Icon::Close:
        stroke(7, 7, 17, 17); stroke(17, 7, 7, 17);
        break;
    case Icon::Minimize:
        stroke(7, 12, 17, 12);
        break;
    case Icon::Maximize:
        d->AddRect(v(7, 7), v(17, 17), color, 1.5f, 0, 1.4f);
        break;
    case Icon::Fullscreen:
        stroke(3.5f, 9, 3.5f, 3.5f); stroke(3.5f, 3.5f, 9, 3.5f);
        stroke(15, 3.5f, 20.5f, 3.5f); stroke(20.5f, 3.5f, 20.5f, 9);
        stroke(3.5f, 15, 3.5f, 20.5f); stroke(3.5f, 20.5f, 9, 20.5f);
        stroke(15, 20.5f, 20.5f, 20.5f); stroke(20.5f, 20.5f, 20.5f, 15);
        break;
    case Icon::Record:
        d->AddCircle(v(12, 12), size * 0.34f, color, 24, 1.5f);
        d->AddCircleFilled(v(12, 12), size * 0.19f, IM_COL32(239, 68, 68, 255), 24);
        break;
    case Icon::Camera:
        d->AddRect(v(3, 6.5f), v(21, 19.5f), color, 2.5f, 0, 1.6f);
        stroke(8.5f, 6.5f, 10, 4.2f); stroke(10, 4.2f, 14, 4.2f); stroke(14, 4.2f, 15.5f, 6.5f);
        d->AddCircle(v(12, 13), size * 0.15f, color, 20, 1.6f);
        break;
    case Icon::Refresh:
        d->PathArcTo(v(12, 12), size * 0.31f, -2.4f, 2.2f, 24);
        d->PathStroke(color, 0, 1.7f);
        d->AddTriangleFilled(v(16.5f, 4.5f), v(20.2f, 8.5f), v(14.5f, 9.2f), color);
        break;
    case Icon::Rotate:
        d->AddRect(v(5, 7), v(15, 19), color, 2.0f, 0, 1.5f);
        d->PathArcTo(v(14, 9), size * 0.22f, -1.6f, 0.4f, 16);
        d->PathStroke(color, 0, 1.5f);
        d->AddTriangleFilled(v(19, 8.5f), v(21.5f, 11.5f), v(17.2f, 11.5f), color);
        break;
    case Icon::Volume:
        d->AddTriangleFilled(v(5, 9), v(11, 5), v(11, 19), color);
        d->AddRectFilled(v(3, 9), v(7, 15), color, 1.0f);
        d->PathArcTo(v(12, 12), size * 0.16f, -0.8f, 0.8f, 12);
        d->PathStroke(color, 0, 1.5f);
        d->PathArcTo(v(12, 12), size * 0.29f, -0.9f, 0.9f, 16);
        d->PathStroke(color, 0, 1.5f);
        break;
    case Icon::VolumeMute:
        d->AddTriangleFilled(v(5, 9), v(11, 5), v(11, 19), color);
        d->AddRectFilled(v(3, 9), v(7, 15), color, 1.0f);
        stroke(14.5f, 9, 20, 15); stroke(20, 9, 14.5f, 15);
        break;
    case Icon::Wifi:
        d->PathArcTo(v(12, 17), size * 0.38f, -2.35f, -0.79f, 20);
        d->PathStroke(color, 0, 1.65f);
        d->PathArcTo(v(12, 17), size * 0.23f, -2.35f, -0.79f, 16);
        d->PathStroke(color, 0, 1.65f);
        d->AddCircleFilled(v(12, 17.5f), size * 0.065f, color, 12);
        break;
    case Icon::Usb:
        stroke(12, 4, 12, 19);
        d->AddCircleFilled(v(12, 19.5f), size * 0.08f, color, 12);
        d->AddTriangleFilled(v(12, 3), v(9.5f, 6.5f), v(14.5f, 6.5f), color);
        stroke(12, 14, 7.5f, 10.5f); stroke(7.5f, 10.5f, 7.5f, 8.5f);
        d->AddCircleFilled(v(7.5f, 8.0f), size * 0.06f, color, 12);
        stroke(12, 12, 16.5f, 9.0f); stroke(16.5f, 9.0f, 16.5f, 7.0f);
        d->AddRectFilled(v(15.2f, 5.5f), v(17.8f, 7.8f), color, 0.5f);
        break;
    case Icon::Android:
        // Geometric robot antenna visor (distinct monochrome icon)
        d->PathArcTo(v(12, 14), size * 0.28f, -3.14159f, 0.0f, 20);
        d->PathStroke(color, 0, 1.65f);
        d->AddRect(v(5.3f, 13.5f), v(18.7f, 19.5f), color, 2.0f, 0, 1.65f);
        stroke(8.2f, 8.5f, 6.2f, 5.2f);
        stroke(15.8f, 8.5f, 17.8f, 5.2f);
        d->AddCircleFilled(v(9.5f, 11.5f), size * 0.045f, color, 10);
        d->AddCircleFilled(v(14.5f, 11.5f), size * 0.045f, color, 10);
        break;
    case Icon::Globe:
        d->AddCircle(v(12, 12), size * 0.35f, color, 24, 1.55f);
        stroke(3.8f, 12, 20.2f, 12, 1.35f);
        d->AddEllipse(v(12, 12), ImVec2(size * 0.17f, size * 0.35f), color, 0, 24, 1.35f);
        break;
    case Icon::Theme:
        d->AddCircle(v(12, 12), size * 0.30f, color, 24, 1.6f);
        d->PathArcTo(v(12, 12), size * 0.28f, -1.57f, 1.57f, 16);
        d->PathFillConvex(color);
        break;
    }
}

inline bool surface(const char* id, ImVec2 size) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    const bool clicked = ImGui::Button(id, size);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    return clicked;
}

inline bool icon_button(const char* id, Icon kind, const char* tooltip,
                        float size = 35.0f, bool primary = false,
                        bool dark_mode = true) {
    const auto pal = get_palette(dark_mode);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = surface(id, ImVec2(size, size));
    auto* d = ImGui::GetWindowDrawList();
    const bool hover = ImGui::IsItemHovered();
    const bool disabled = ImGui::GetStyle().Alpha < 0.9f;
    ImU32 color = disabled ? pal.text_subtle : pal.text_ink;
    if (primary) {
        const ImU32 fill = disabled ? pal.bg_pill_idle
                                    : (hover ? IM_COL32(79, 146, 255, 255) : pal.accent_blue);
        d->AddCircleFilled(ImVec2(p.x + size * 0.5f, p.y + size * 0.5f), size * 0.5f, fill, 48);
        color = IM_COL32(255, 255, 255, 255);
    } else if (hover) {
        d->AddRectFilled(p, ImVec2(p.x + size, p.y + size), pal.bg_card_hover, 9.0f);
    }
    icon(d, kind, ImVec2(p.x + (size - 20.0f) * 0.5f, p.y + (size - 20.0f) * 0.5f), 20.0f, color);
    if (hover && tooltip && tooltip[0]) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

inline bool pill_button(const char* id, const char* label, Icon kind,
                        bool active, const char* tooltip,
                        ImFont* font, bool dark_mode = true, float height = 34.0f) {
    const auto pal = get_palette(dark_mode);
    const ImVec2 text_sz = font->CalcTextSizeA(font->FontSize, 1000.0f, 0.0f, label);
    const float width = std::max(64.0f, text_sz.x + 38.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = surface(id, ImVec2(width, height));
    auto* d = ImGui::GetWindowDrawList();
    const bool hover = ImGui::IsItemHovered();

    const ImU32 bg = active ? pal.bg_pill_active
                            : (hover ? pal.bg_card_hover : pal.bg_pill_idle);
    const ImU32 fg = active ? IM_COL32(255, 255, 255, 255) : pal.text_ink;
    d->AddRectFilled(p, ImVec2(p.x + width, p.y + height), bg, height * 0.5f);
    d->AddRect(p, ImVec2(p.x + width, p.y + height),
               active ? pal.accent_blue : pal.border_glass, height * 0.5f);
    icon(d, kind, ImVec2(p.x + 10.0f, p.y + (height - 17.0f) * 0.5f), 17.0f, fg);
    d->AddText(font, font->FontSize,
               ImVec2(p.x + 30.0f, p.y + (height - text_sz.y) * 0.5f), fg, label);
    if (hover && tooltip && tooltip[0]) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

inline bool nav(const char* id, const char* label, Icon kind, bool selected,
                float width, ImFont* font, bool dark_mode = true) {
    const auto pal = get_palette(dark_mode);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = surface(id, ImVec2(width, 42.0f));
    auto* d = ImGui::GetWindowDrawList();
    const bool hover = ImGui::IsItemHovered();
    if (selected || hover) {
        d->AddRectFilled(p, ImVec2(p.x + width, p.y + 42.0f),
                         selected ? pal.accent_blue_soft : pal.bg_card_hover, 10.0f);
        if (selected) {
            d->AddRectFilled(ImVec2(p.x + 2.0f, p.y + 10.0f),
                             ImVec2(p.x + 5.5f, p.y + 32.0f),
                             pal.accent_blue, 2.0f);
        }
    }
    const ImU32 color = selected ? (dark_mode ? IM_COL32(255, 255, 255, 255) : pal.accent_blue)
                                 : pal.text_muted;
    icon(d, kind, ImVec2(p.x + 14.0f, p.y + 11.0f), 20.0f,
         selected ? pal.accent_blue : color);
    d->AddText(font, font->FontSize,
               ImVec2(p.x + 44.0f, p.y + (42.0f - font->FontSize) * 0.5f),
               color, label);
    return clicked;
}

inline bool text_button(const char* label, ImVec2 size = ImVec2(0, 34), bool dark_mode = true) {
    const auto pal = get_palette(dark_mode);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(pal.bg_card));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(pal.bg_card_hover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(pal.accent_blue_soft));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(pal.border_line));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.text_ink));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);
    return pressed;
}

} // namespace ap::video::receiver_ui

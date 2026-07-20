#pragma once
#include <raylib.h>

namespace gui::util {
struct UITheme {
    static constexpr Color PanelBg       { 15, 15, 20, 235 };
    static constexpr Color SubCardBg     { 24, 24, 34, 255 };
    static constexpr Color PanelBorder   { 65, 65, 85, 255 };
    static constexpr Color HeaderBg      { 32, 32, 48, 255 };
    static constexpr Color TextNormal    { 225, 225, 235, 255 };
    static constexpr Color TextMuted     { 135, 135, 155, 255 };
    static constexpr Color AccentActive  { 0, 190, 255, 255 };
    static constexpr Color ButtonHover   { 52, 52, 74, 255 };
};
}
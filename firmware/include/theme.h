// Dark theme palette — same accent colors as the Pi build's ui.py, so the
// gauge cluster's visual identity carries over, just rendered with LVGL's
// anti-aliased arcs/gradients instead of hand-drawn pygame rects.
#pragma once
#include <lvgl.h>

namespace theme {
inline lv_color_t bg()      { return lv_color_hex(0x0a0c0c); }
inline lv_color_t panel()   { return lv_color_hex(0x1a1e20); }
inline lv_color_t card()    { return lv_color_hex(0x262a2c); }
inline lv_color_t border()  { return lv_color_hex(0x3a3e40); }
inline lv_color_t text()    { return lv_color_hex(0xffffff); }
inline lv_color_t subtext() { return lv_color_hex(0xa0a6a8); }

inline lv_color_t boost()   { return lv_color_hex(0xff4444); }
inline lv_color_t rpm()     { return lv_color_hex(0x44aaff); }
inline lv_color_t coolant() { return lv_color_hex(0x44ff88); }
inline lv_color_t oil()     { return lv_color_hex(0xffaa44); }
inline lv_color_t danger()  { return lv_color_hex(0xff4444); }
inline lv_color_t success() { return lv_color_hex(0x44dc64); }
}  // namespace theme

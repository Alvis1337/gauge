// A single arc-style gauge: colored value arc + big numeric readout +
// small label underneath. Four of these make up the main screen (boost,
// ethanol, coolant, oil), replacing the Pi build's flat pygame rectangles
// with real anti-aliased arcs and a proper dark theme.
#pragma once
#include <lvgl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

class GaugeWidget {
public:
    void create(lv_obj_t *parent, const char *label, float min_v, float max_v,
                lv_color_t color, const char *unit_fmt) {
        _min = min_v;
        _max = max_v;
        _unit_fmt = unit_fmt;

        _root = lv_obj_create(parent);
        lv_obj_set_size(_root, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(_root, lv_color_hex(0x1e1e1e), 0);
        lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(_root, 14, 0);
        lv_obj_set_style_border_width(_root, 1, 0);
        lv_obj_set_style_border_color(_root, lv_color_hex(0x3c3c3c), 0);
        lv_obj_set_style_shadow_width(_root, 12, 0);
        lv_obj_set_style_shadow_opa(_root, LV_OPA_30, 0);
        lv_obj_set_style_shadow_color(_root, color, 0);
        lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);

        _arc = lv_arc_create(_root);
        lv_obj_set_size(_arc, 128, 128);
        lv_arc_set_rotation(_arc, 135);
        lv_arc_set_bg_angles(_arc, 0, 270);
        lv_arc_set_range(_arc, 0, 1000);  // internal 0-1000 fixed range; setValue() maps float -> this
        lv_obj_remove_style(_arc, NULL, LV_PART_KNOB);
        lv_obj_clear_flag(_arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(_arc, 10, LV_PART_MAIN);
        lv_obj_set_style_arc_color(_arc, lv_color_hex(0x333333), LV_PART_MAIN);
        lv_obj_set_style_arc_width(_arc, 10, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(_arc, color, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(_arc, true, LV_PART_INDICATOR);
        lv_obj_align(_arc, LV_ALIGN_CENTER, 0, -6);

        _value_label = lv_label_create(_arc);
        lv_obj_set_style_text_font(_value_label, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(_value_label, lv_color_white(), 0);
        lv_obj_center(_value_label);
        lv_label_set_text(_value_label, "--");

        _name_label = lv_label_create(_root);
        lv_obj_set_style_text_font(_name_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(_name_label, lv_color_hex(0xa0a0a0), 0);
        lv_label_set_text(_name_label, label);
        lv_obj_align(_name_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    }

    // NAN renders as "--", same as the Python GaugeData.*_display() methods.
    void setValue(float v) {
        if (isnan(v)) {
            lv_label_set_text(_value_label, "--");
            lv_arc_set_value(_arc, 0);
            return;
        }
        char buf[16];
        snprintf(buf, sizeof(buf), _unit_fmt, v);
        lv_label_set_text(_value_label, buf);

        float clamped = std::max(_min, std::min(_max, v));
        int mapped = (int)((clamped - _min) / (_max - _min) * 1000.0f);
        lv_arc_set_value(_arc, mapped);
    }

private:
    lv_obj_t *_root = nullptr;
    lv_obj_t *_arc = nullptr;
    lv_obj_t *_value_label = nullptr;
    lv_obj_t *_name_label = nullptr;
    float _min = 0, _max = 100;
    const char *_unit_fmt = "%.0f";
};

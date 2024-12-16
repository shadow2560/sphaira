#pragma once

#include "ui/widget.hpp"

namespace sphaira::ui {

// todo: remove fixed values from appstore
struct ScrollableText final : Widget {
    ScrollableText(const std::string& text, float x, float y, float y_clip, float w, float font_size);
    void Draw(NVGcontext* vg, Theme* theme) override;

    std::string m_text;
    // static constexpr float m_y_off_base = 374;
    // float m_y_off = m_y_off_base;
    // static constexpr float m_clip_y = 250.0F;

    const float m_y_off_base;
    float m_y_off;
    const float m_clip_y;
    const float m_end_w;
    static constexpr float m_step = 30;

    int m_index = 0;
    const float m_font_size;
    float m_bounds[4];
};

} // namespace sphaira::ui

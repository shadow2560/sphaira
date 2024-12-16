#pragma once

#include "ui/widget.hpp"

namespace sphaira::ui {

class ScrollBar final : public Widget {
public:
    enum class Direction { DOWN, UP };

public:
    ScrollBar() = default;
    ScrollBar(Vec4 bounds, float entry_height, std::size_t entries);

    auto Update(Controller* controller, TouchInfo* touch) -> void override {}
    auto OnLayoutChange() -> void override;
    auto Draw(NVGcontext* vg, Theme* theme) -> void override;

    auto Setup(Vec4 bounds, float entry_height, std::size_t entries) -> void;
    auto Move(Direction direction) -> void;

private:
    auto Setup() -> void;

private:
    Vec4 m_bounds{};
    std::size_t m_entries{};
    std::size_t m_index{};
    float m_entry_height{};
    float m_step_size{};
    bool m_should_draw{false};
};

} // namespace sphaira::ui

#include "ui/scrollbar.hpp"
#include "ui/nvg_util.hpp"

namespace sphaira::ui {

ScrollBar::ScrollBar(Vec4 bounds, float entry_height, std::size_t entries)
    : m_bounds{bounds}
    , m_entries{entries}
    , m_entry_height{entry_height} {
    Setup();
}

auto ScrollBar::OnLayoutChange() -> void {

}

auto ScrollBar::Draw(NVGcontext* vg, Theme* theme) -> void {
    if (m_should_draw) {
        gfx::drawRect(vg, m_pos, gfx::Colour::RED);
    }
}

auto ScrollBar::Setup(Vec4 bounds, float entry_height, std::size_t entries) -> void {
    m_bounds = bounds;
    m_entry_height = entry_height;
    m_entries = entries;
    Setup();
}

auto ScrollBar::Setup() -> void {
    m_bounds.y += 5.f;
    m_bounds.h -= 10.f;

    const float total_size = (m_entry_height) * static_cast<float>(m_entries);
    if (total_size > m_bounds.h) {
        m_step_size = total_size / m_entries;
        m_pos.x = m_bounds.x;
        m_pos.y = m_bounds.y;
        m_pos.w = 2.f;
        m_pos.h = total_size - m_bounds.h;
        m_should_draw = true;
        // LOG("total size: %.2f\n", total_size);
        // LOG("step size: %.2f\n", m_step_size);
        // LOG("pos y: %.2f\n", m_pos.y);
        // LOG("pos h: %.2f\n", m_pos.h);
    } else {
        // LOG("not big enough for scroll total: %.2f bounds: %.2f\n", total_size, bounds.h);
    }
}

auto ScrollBar::Move(Direction direction) -> void {
    switch (direction) {
        case Direction::DOWN:
            if (m_index < (m_entries - 1)) {
                m_index++;
                m_pos.y += m_step_size;
            }
            break;
        case Direction::UP:
            if (m_index != 0) {
                m_index--;
                m_pos.y -= m_step_size;
            }
            break;
    }
}

} // namespace sphaira::ui

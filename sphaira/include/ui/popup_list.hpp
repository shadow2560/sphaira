#pragma once

#include "ui/widget.hpp"
#include "ui/scrollbar.hpp"
#include <optional>

namespace sphaira::ui {

class PopupList final : public Widget {
public:
    using Items = std::vector<std::string>;
    using Callback = std::function<void(std::optional<std::size_t>)>;

public:
    explicit PopupList(std::string title, Items items, Callback cb, std::size_t index = 0);
    PopupList(std::string title, Items items, Callback cb, std::string index);
    PopupList(std::string title, Items items, std::string& index_str_ref, std::size_t& index);
    PopupList(std::string title, Items items, std::string& index_ref);
    PopupList(std::string title, Items items, std::size_t& index_ref);

    auto Update(Controller* controller, TouchInfo* touch) -> void override;
    auto OnLayoutChange() -> void override;
    auto Draw(NVGcontext* vg, Theme* theme) -> void override;
    auto OnFocusGained() noexcept -> void override;
    auto OnFocusLost() noexcept -> void override;

private:
    static constexpr Vec2 m_title_pos{70.f, 28.f};
    static constexpr Vec4 m_block{280.f, 110.f, 720.f, 60.f};
    static constexpr float m_text_xoffset{15.f};
    static constexpr float m_line_width{1220.f};

    std::string m_title;
    Items m_items;
    Callback m_callback;
    std::size_t m_index; // index in list array
    std::size_t m_index_offset{}; // drawing from array start

    // std::size_t& index_ref;
    // std::string& index_str_ref;

    float m_selected_y{};
    float m_yoff{};
    float m_line_top{};
    float m_line_bottom{};
    ScrollBar m_scrollbar;
};

} // namespace sphaira::ui

#include "ui/popup_list.hpp"
#include "ui/nvg_util.hpp"
#include "app.hpp"
#include "i18n.hpp"

namespace sphaira::ui {

PopupList::PopupList(std::string title, Items items, std::string& index_str_ref, s64& index_ref)
: PopupList{std::move(title), std::move(items), Callback{}, index_ref}  {

    m_callback = [&index_str_ref, &index_ref, this](auto op_idx) {
        if (op_idx) {
            index_ref = *op_idx;
            index_str_ref = m_items[index_ref];
        }
    };
}

PopupList::PopupList(std::string title, Items items, std::string& index_ref)
: PopupList{std::move(title), std::move(items), Callback{}}  {

    const auto it = std::find(m_items.cbegin(), m_items.cend(), index_ref);
    if (it != m_items.cend()) {
        m_index = std::distance(m_items.cbegin(), it);
        if (m_index >= 7) {
            m_index_offset = m_index - 6;
        }
    }

    m_callback = [&index_ref, this](auto op_idx) {
        if (op_idx) {
            index_ref = m_items[*op_idx];
        }
    };
}

PopupList::PopupList(std::string title, Items items, s64& index_ref)
: PopupList{std::move(title), std::move(items), Callback{}, index_ref}  {

    m_callback = [&index_ref, this](auto op_idx) {
        if (op_idx) {
            index_ref = *op_idx;
        }
    };
}

PopupList::PopupList(std::string title, Items items, Callback cb, std::string index)
: PopupList{std::move(title), std::move(items), cb, 0} {

    const auto it = std::find(m_items.cbegin(), m_items.cend(), index);
    if (it != m_items.cend()) {
        SetIndex(std::distance(m_items.cbegin(), it));
        if (m_index >= 7) {
            m_index_offset = m_index - 6;
        }
    }
}

PopupList::PopupList(std::string title, Items items, Callback cb, s64 index)
: m_title{std::move(title)}
, m_items{std::move(items)}
, m_callback{cb}
, m_index{index} {
    this->SetActions(
        std::make_pair(Button::DOWN, Action{[this](){
            if (m_list->ScrollDown(m_index, 1, m_items.size())) {
                SetIndex(m_index);
            }
        }}),
        std::make_pair(Button::UP, Action{[this](){
            if (m_list->ScrollUp(m_index, 1, m_items.size())) {
                SetIndex(m_index);
            }
        }}),
        std::make_pair(Button::A, Action{"Select"_i18n, [this](){
            if (m_callback) {
                m_callback(m_index);
            }
            SetPop();
        }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }})
    );

    m_pos.w = 1280.f;
    const float a = std::min(405.f, (60.f * static_cast<float>(m_items.size())));
    m_pos.h = 80.f + 140.f + a;
    m_pos.y = 720.f - m_pos.h;
    m_line_top = m_pos.y + 70.f;
    m_line_bottom = 720.f - 73.f;
    if (m_index >= 7) {
        m_index_offset = m_index - 6;
    }

    Vec4 v{m_block};
    v.y = m_line_top + 1.f + 42.f;
    const Vec4 pos{0, m_line_top, 1280.f, m_line_bottom - m_line_top};
    m_list = std::make_unique<List>(1, 7, pos, v);
    m_list->SetScrollBarPos(1250, m_line_top + 20, m_line_bottom - m_line_top - 40);
}

auto PopupList::Update(Controller* controller, TouchInfo* touch) -> void {
    Widget::Update(controller, touch);
    m_list->OnUpdate(controller, touch, m_items.size(), [this](auto i) {
        SetIndex(i);
        FireAction(Button::A);
    });
}

auto PopupList::Draw(NVGcontext* vg, Theme* theme) -> void {
    gfx::dimBackground(vg);
    gfx::drawRect(vg, m_pos, theme->elements[ThemeEntryID_SELECTED].colour);
    gfx::drawText(vg, m_pos + m_title_pos, 24.f, theme->elements[ThemeEntryID_TEXT].colour, m_title.c_str());
    gfx::drawRect(vg, 30.f, m_line_top, m_line_width, 1.f, theme->elements[ThemeEntryID_TEXT].colour);
    gfx::drawRect(vg, 30.f, m_line_bottom, m_line_width, 1.f, theme->elements[ThemeEntryID_TEXT].colour);

    m_list->Draw(vg, theme, m_items.size(), [this](auto* vg, auto* theme, auto v, auto i) {
        const auto& [x, y, w, h] = v;
        if (m_index == i) {
            gfx::drawRect(vg, x - 4.f, y - 4.f, w + 8.f, h + 8.f, theme->elements[ThemeEntryID_SELECTED_OVERLAY].colour);
            gfx::drawRect(vg, x, y, w, h, theme->elements[ThemeEntryID_SELECTED].colour);
            gfx::drawText(vg, x + m_text_xoffset, y + (h / 2.f), 20.f, m_items[i].c_str(), NULL, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->elements[ThemeEntryID_TEXT_SELECTED].colour);
        } else {
            gfx::drawRect(vg, x, y, w, 1.f, theme->elements[ThemeEntryID_TEXT].colour);
            gfx::drawRect(vg, x, y + h, w, 1.f, theme->elements[ThemeEntryID_TEXT].colour);
            gfx::drawText(vg, x + m_text_xoffset, y + (h / 2.f), 20.f, m_items[i].c_str(), NULL, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->elements[ThemeEntryID_TEXT].colour);
        }
    });

    Widget::Draw(vg, theme);
}

auto PopupList::OnFocusGained() noexcept -> void {
    Widget::OnFocusGained();
    SetHidden(false);
}

auto PopupList::OnFocusLost() noexcept -> void {
    Widget::OnFocusLost();
    SetHidden(true);
}

void PopupList::SetIndex(s64 index) {
    m_index = index;

    if (m_index > m_index_offset && m_index - m_index_offset >= 6) {
        m_index_offset = m_index - 6;
    }
}

} // namespace sphaira::ui

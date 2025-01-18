#include "ui/widget.hpp"
#include "ui/nvg_util.hpp"
#include "app.hpp"
#include "log.hpp"

namespace sphaira::ui {

auto uiButton::Draw(NVGcontext* vg, Theme* theme) -> void {
    // enable to see button region
    // gfx::drawRect(vg, m_pos, gfx::Colour::RED);

    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
    nvgFillColor(vg, theme->GetColour(ThemeEntryID_TEXT));
    nvgFontSize(vg, 20);
    nvgText(vg, m_hint_pos.x, m_hint_pos.y, m_action.m_hint.c_str(), nullptr);
    nvgFontSize(vg, 26);
    nvgText(vg, m_button_pos.x, m_button_pos.y, gfx::getButton(m_button), nullptr);
}

void Widget::Update(Controller* controller, TouchInfo* touch) {
    for (const auto& [button, action] : m_actions) {
        if ((action.m_type & ActionType::DOWN) && controller->GotDown(button)) {
            if (static_cast<u64>(button) & static_cast<u64>(Button::ANY_BUTTON)) {
                App::PlaySoundEffect(SoundEffect_Focus);
            }
            action.Invoke(true);
            break;
        }
        else if ((action.m_type & ActionType::UP) && controller->GotUp(button)) {
            action.Invoke(false);
            break;
        }
        else if ((action.m_type & ActionType::HELD) && controller->GotHeld(button)) {
            action.Invoke(true);
            break;
        }
    }

    auto draw_actions = GetUiButtons();
    for (auto& e : draw_actions) {
        if (touch->is_clicked && touch->in_range(e.GetPos())) {
            log_write("got click: %s\n", e.m_action.m_hint.c_str());
            FireAction(e.m_button);
            break;
        }
    }
}

void Widget::Draw(NVGcontext* vg, Theme* theme) {
    auto draw_actions = GetUiButtons();

    for (auto& e : draw_actions) {
        e.Draw(vg, theme);
    }
}

auto Widget::HasAction(Button button) const -> bool {
    return m_actions.contains(button);
}

void Widget::SetAction(Button button, Action action) {
    m_actions.insert_or_assign(button, action);
}

void Widget::RemoveAction(Button button) {
    if (auto it = m_actions.find(button); it != m_actions.end()) {
        m_actions.erase(it);
    }
}

auto Widget::FireAction(Button b, u8 type) -> bool {
    for (const auto& [button, action] : m_actions) {
        if (button == b && (action.m_type & type)) {
            App::PlaySoundEffect(SoundEffect_Focus);
            action.Invoke(true);
            return true;
        }
    }
    return false;
}

auto Widget::GetUiButtons() const -> uiButtons {
    auto vg = App::GetVg();
    auto [x, y] = m_button_pos;

    uiButtons draw_actions;
    draw_actions.reserve(m_actions.size());

    // build array
    for (const auto& [button, action] : m_actions) {
        if (action.IsHidden() || action.m_hint.empty()) {
            continue;
        }

        uiButton ui_button{button, action};

        // swap
        if (button == Button::R && draw_actions.size() && draw_actions.back().m_button == Button::L) {
            const auto s = draw_actions.back();
            draw_actions.back().m_button = button;
            draw_actions.back().m_action = action;
            draw_actions.emplace_back(s);
        } else {
            draw_actions.emplace_back(ui_button);
        }
    }

    float bounds[4]{};
    for (auto& e : draw_actions) {
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);

        nvgFontSize(vg, 20.f);
        nvgTextBounds(vg, x, y, e.m_action.m_hint.c_str(), nullptr, bounds);
        auto len = bounds[2] - bounds[0];
        e.m_hint_pos = {x, 675, len, 20};

        x -= len + 8.f;
        nvgFontSize(vg, 26.f);
        nvgTextBounds(vg, x, y - 7.f, gfx::getButton(e.m_button), nullptr, bounds);
        len = bounds[2] - bounds[0];
        e.m_button_pos = {x, 675 - 4.f, len, 26};
        x -= len + 34.f;

        e.SetPos(e.m_button_pos);
        e.SetX(e.GetX() - 40);
        e.SetW(e.m_hint_pos.x - e.m_button_pos.x + len + 25);
        e.SetY(e.GetY() - 18);
        e.SetH(26 + 18 * 2);
    }

    return draw_actions;
}

} // namespace sphaira::ui

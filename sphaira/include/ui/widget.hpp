#pragma once

#include "ui/object.hpp"
#include <vector>
#include <memory>
#include <map>
#include <unordered_map>

namespace sphaira::ui {

struct uiButton final : Object {
    uiButton(Button button, Action action) : m_button{button}, m_action{action} {}
    auto Draw(NVGcontext* vg, Theme* theme) -> void override;

    Button m_button;
    Action m_action;
    Vec4 m_button_pos{};
    Vec4 m_hint_pos{};
};

struct Widget : public Object {
    using Actions = std::map<Button, Action>;
    using uiButtons = std::vector<uiButton>;

    virtual ~Widget() = default;

    virtual void Update(Controller* controller, TouchInfo* touch);
    virtual void Draw(NVGcontext* vg, Theme* theme);

    virtual void OnFocusGained() {
        m_focus = true;
    }

    virtual void OnFocusLost() {
        m_focus = false;
    }

    virtual auto HasFocus() const -> bool {
        return m_focus;
    }

    virtual auto IsMenu() const -> bool {
        return false;
    }

    auto HasAction(Button button) const -> bool;
    void SetAction(Button button, Action action);
    void SetActions(std::same_as<std::pair<Button, Action>> auto ...args) {
        const std::array list = {args...};
        for (const auto& [button, action] : list) {
            SetAction(button, action);
        }
    }

    auto GetActions() const {
        return m_actions;
    }

    void RemoveAction(Button button);

    void RemoveActions() {
        m_actions.clear();
    }

    auto FireAction(Button button, u8 type = ActionType::DOWN) -> bool;

    void SetPop(bool pop = true) {
        m_pop = pop;
    }

    auto ShouldPop() const -> bool {
        return m_pop;
    }

    auto SetUiButtonPos(Vec2 pos) {
        m_button_pos = pos;
    }

    auto GetUiButtons() const -> uiButtons;

    Actions m_actions{};
    Vec2 m_button_pos{1220, 675};
    bool m_focus{false};
    bool m_pop{false};
};

} // namespace sphaira::ui

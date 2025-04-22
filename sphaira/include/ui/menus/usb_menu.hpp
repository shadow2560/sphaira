#pragma once

#include "ui/menus/menu_base.hpp"
#include "yati/source/usb.hpp"

namespace sphaira::ui::menu::usb {

enum class State {
    // not connected.
    None,
    // just connected, starts the transfer.
    Connected,
    // set whilst transfer is in progress.
    Progress,
    // set when the transfer is finished.
    Done,
    // failed to connect.
    Failed,
};

struct Menu final : MenuBase {
    Menu();
    ~Menu();

    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    void OnFocusGained() override;

// this should be private
// private:
    std::shared_ptr<yati::source::Usb> m_usb_source{};
    bool m_was_mtp_enabled{};

    Thread m_thread{};
    Mutex m_mutex{};
    // the below are shared across threads, lock with the above mutex!
    State m_state{State::None};
    bool m_usb_has_connection{};
    u32 m_usb_speed{};
    u32 m_usb_count{};
};

} // namespace sphaira::ui::menu::usb

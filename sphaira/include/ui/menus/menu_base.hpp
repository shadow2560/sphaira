#pragma once

#include "ui/widget.hpp"
#include "ui/scrolling_text.hpp"
#include <string>

namespace sphaira::ui::menu {

struct PolledData {
    struct tm tm{};
    u32 battery_percetange{};
    PsmChargerType charger_type{};
    NifmInternetConnectionType type{};
    NifmInternetConnectionStatus status{};
    u32 strength{};
    u32 ip{};
};

struct MenuBase : Widget {
    MenuBase(std::string title);
    virtual ~MenuBase();

    virtual auto GetShortTitle() const -> const char* = 0;
    virtual void Update(Controller* controller, TouchInfo* touch);
    virtual void Draw(NVGcontext* vg, Theme* theme);

    auto IsMenu() const -> bool override {
        return true;
    }

    void SetTitle(std::string title);
    void SetTitleSubHeading(std::string sub_heading);
    void SetSubHeading(std::string sub_heading);

    auto GetTitle() const {
        return m_title;
    }

    static auto GetPolledData(bool force_refresh = false) -> PolledData;

private:
    std::string m_title{};
    std::string m_title_sub_heading{};
    std::string m_sub_heading{};

    ScrollingText m_scroll_title_sub_heading{};
    ScrollingText m_scroll_sub_heading{};
};

} // namespace sphaira::ui::menu

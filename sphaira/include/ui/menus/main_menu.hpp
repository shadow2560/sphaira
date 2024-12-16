#pragma once

#include "ui/widget.hpp"
#include "ui/menus/homebrew.hpp"
#include "ui/menus/filebrowser.hpp"
#include "ui/menus/appstore.hpp"

namespace sphaira::ui::menu::main {

// this holds 2 menus and allows for switching between them
struct MainMenu final : Widget {
    MainMenu();
    ~MainMenu();

    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    void OnFocusGained() override;
    void OnFocusLost() override;

private:
    void OnLRPress(std::shared_ptr<MenuBase> menu, Button b);
    void AddOnLPress();
    void AddOnRPress();

private:
    std::shared_ptr<homebrew::Menu> m_homebrew_menu{};
    std::shared_ptr<filebrowser::Menu> m_filebrowser_menu{};
    std::shared_ptr<appstore::Menu> m_app_store_menu{};
    std::shared_ptr<MenuBase> m_current_menu{};

    std::string m_update_url{};
    std::string m_update_version{};
    std::string m_update_description{};
    bool m_update_avaliable{};
};

} // namespace sphaira::ui::menu::main

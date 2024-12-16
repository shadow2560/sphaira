#include "ui/menus/main_menu.hpp"
#include "ui/sidebar.hpp"
#include "ui/popup_list.hpp"
#include "ui/option_box.hpp"
#include "app.hpp"
#include "log.hpp"
#include "download.hpp"
#include "defines.hpp"
#include "ui/menus/irs_menu.hpp"
#include "ui/menus/themezer.hpp"
#include "web.hpp"
#include "i18n.hpp"

#include <cstring>

namespace sphaira::ui::menu::main {
namespace {

#if 0
bool parseSearch(const char *parse_string, const char *filter, char* new_string) {
    char c;
    u32 offset = 0;
    const u32 filter_len = std::strlen(filter) - 1;

    while ((c = parse_string[offset++]) != '\0') {
        if (c == *filter) {
            for (u32 i = 0; c == filter[i]; i++) {
                c = parse_string[offset++];
                if (i == filter_len) {
                    for (u32 j = 0; c != '\"'; j++) {
                        new_string[j] = c;
                        new_string[j+1] = '\0';
                        c = parse_string[offset++];
                    }
                    return true;
                }
            }
        }
    }

    return false;
}
#endif

} // namespace

MainMenu::MainMenu() {
    #if 0
    DownloadMemoryAsync("https://api.github.com/repos/ITotalJustice/sys-patch/releases/latest", [this](std::vector<u8>& data, bool success){
        data.push_back('\0');
        auto raw_str = (const char*)data.data();
        char out_str[0x301];

        if (parseSearch(raw_str, "tag_name\":\"", out_str)) {
            m_update_version = out_str;
            if (strcasecmp("v1.5.0", m_update_version.c_str())) {
                m_update_avaliable = true;
            }
            log_write("FOUND IT : %s\n", out_str);
        }

        if (parseSearch(raw_str, "browser_download_url\":\"", out_str)) {
            m_update_url = out_str;
            log_write("FOUND IT : %s\n", out_str);
        }

        if (parseSearch(raw_str, "body\":\"", out_str)) {
            m_update_description = out_str;
            // m_update_description.replace("\r\n\r\n", "\n");
            log_write("FOUND IT : %s\n", out_str);
        }
    });
    #endif

    AddOnLPress();
    AddOnRPress();

    this->SetActions(
        std::make_pair(Button::START, Action{App::Exit}),
        std::make_pair(Button::Y, Action{"Menu"_i18n, [this](){
            auto options = std::make_shared<Sidebar>("Menu Options"_i18n, "v" APP_VERSION_HASH, Sidebar::Side::LEFT);
            ON_SCOPE_EXIT(App::Push(options));


            SidebarEntryArray::Items language_items;
            language_items.push_back("Auto"_i18n);
            language_items.push_back("English");
            language_items.push_back("Japanese");
            language_items.push_back("French");
            language_items.push_back("German");
            language_items.push_back("Italian");
            language_items.push_back("Spanish");
            language_items.push_back("Chinese");
            language_items.push_back("Korean");
            language_items.push_back("Dutch");
            language_items.push_back("Portuguese");
            language_items.push_back("Russian");

            options->AddHeader("Header"_i18n);
            options->AddSpacer();
            options->Add(std::make_shared<SidebarEntryCallback>("Theme"_i18n, [this](){
                SidebarEntryArray::Items theme_items{};
                const auto theme_meta = App::GetThemeMetaList();
                for (auto& p : theme_meta) {
                    theme_items.emplace_back(p.name);
                }

                auto options = std::make_shared<Sidebar>("Theme Options"_i18n, Sidebar::Side::LEFT);
                ON_SCOPE_EXIT(App::Push(options));

                options->Add(std::make_shared<SidebarEntryArray>("Select Theme"_i18n, theme_items, [this, theme_items](std::size_t& index_out){
                    App::SetTheme(index_out);
                }, App::GetThemeIndex()));

                options->Add(std::make_shared<SidebarEntryBool>("Shuffle"_i18n, App::GetThemeShuffleEnable(), [this](bool& enable){
                    App::SetThemeShuffleEnable(enable);
                }, "Enabled"_i18n, "Disabled"_i18n));

                options->Add(std::make_shared<SidebarEntryBool>("Music"_i18n, App::GetThemeMusicEnable(), [this](bool& enable){
                    App::SetThemeMusicEnable(enable);
                }, "Enabled"_i18n, "Disabled"_i18n));
            }));

            options->Add(std::make_shared<SidebarEntryCallback>("Network"_i18n, [this](){
                auto options = std::make_shared<Sidebar>("Network Options"_i18n, Sidebar::Side::LEFT);
                ON_SCOPE_EXIT(App::Push(options));

                options->Add(std::make_shared<SidebarEntryBool>("Nxlink"_i18n, App::GetNxlinkEnable(), [this](bool& enable){
                    App::SetNxlinkEnable(enable);
                }, "Enabled"_i18n, "Disabled"_i18n));
                options->Add(std::make_shared<SidebarEntryCallback>("Check for update"_i18n, [this](){
                    App::Notify("Not Implemented"_i18n);
                }));
            }));

            options->Add(std::make_shared<SidebarEntryArray>("Language"_i18n, language_items, [this, language_items](std::size_t& index_out){
                App::SetLanguage(index_out);
            }, (std::size_t)App::GetLanguage()));

            if (m_update_avaliable) {
                std::string str = "Update avaliable: "_i18n + m_update_version;
                options->Add(std::make_shared<SidebarEntryCallback>(str, [this](){
                    App::Notify("Not Implemented"_i18n);
                }));
            }

            options->Add(std::make_shared<SidebarEntryBool>("Logging"_i18n, App::GetLogEnable(), [this](bool& enable){
                App::SetLogEnable(enable);
            }, "Enabled"_i18n, "Disabled"_i18n));
            options->Add(std::make_shared<SidebarEntryBool>("Replace hbmenu on exit"_i18n, App::GetReplaceHbmenuEnable(), [this](bool& enable){
                App::SetReplaceHbmenuEnable(enable);
            }, "Enabled"_i18n, "Disabled"_i18n));

            options->Add(std::make_shared<SidebarEntryCallback>("Misc"_i18n, [this](){
                auto options = std::make_shared<Sidebar>("Misc Options"_i18n, Sidebar::Side::LEFT);
                ON_SCOPE_EXIT(App::Push(options));

                options->Add(std::make_shared<SidebarEntryCallback>("Themezer"_i18n, [](){
                    App::Push(std::make_shared<menu::themezer::Menu>());
                }));
                options->Add(std::make_shared<SidebarEntryCallback>("Irs"_i18n, [](){
                    App::Push(std::make_shared<menu::irs::Menu>());
                }));
                options->Add(std::make_shared<SidebarEntryCallback>("Web"_i18n, [](){
                    WebShow("https://lite.duckduckgo.com/lite");
                }));
            }));
        }})
    );

    m_homebrew_menu = std::make_shared<homebrew::Menu>();
    m_filebrowser_menu = std::make_shared<filebrowser::Menu>(m_homebrew_menu->GetHomebrewList());
    m_app_store_menu = std::make_shared<appstore::Menu>(m_homebrew_menu->GetHomebrewList());
    m_current_menu = m_homebrew_menu;

    for (auto [button, action] : m_actions) {
        m_current_menu->SetAction(button, action);
    }
}

MainMenu::~MainMenu() {

}

void MainMenu::Update(Controller* controller, TouchInfo* touch) {
    m_current_menu->Update(controller, touch);
}

void MainMenu::Draw(NVGcontext* vg, Theme* theme) {
    m_current_menu->Draw(vg, theme);
}

void MainMenu::OnFocusGained() {
    Widget::OnFocusGained();
    this->SetHidden(false);
    m_current_menu->OnFocusGained();
}

void MainMenu::OnFocusLost() {
    m_current_menu->OnFocusLost();
}

void MainMenu::OnLRPress(std::shared_ptr<MenuBase> menu, Button b) {
    m_current_menu->OnFocusLost();
    if (m_current_menu == m_homebrew_menu) {
        m_current_menu = menu;
        RemoveAction(b);
    } else {
        m_current_menu = m_homebrew_menu;
    }

    m_current_menu->OnFocusGained();

    for (auto [button, action] : m_actions) {
        m_current_menu->SetAction(button, action);
    }

    if (b == Button::L) {
        AddOnRPress();
    } else {
        AddOnLPress();
    }
}

void MainMenu::AddOnLPress() {
    SetAction(Button::L, Action{"Fs"_i18n, [this]{
        OnLRPress(m_filebrowser_menu, Button::L);
    }});
}

void MainMenu::AddOnRPress() {
    SetAction(Button::R, Action{"App"_i18n, [this]{
        OnLRPress(m_app_store_menu, Button::R);
    }});
}

} // namespace sphaira::ui::menu::main

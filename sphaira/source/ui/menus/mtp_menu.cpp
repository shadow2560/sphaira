#include "ui/menus/mtp_menu.hpp"
#include "yati/yati.hpp"
#include "usb/usbds.hpp"
#include "app.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "ui/nvg_util.hpp"
#include "i18n.hpp"
#include "haze_helper.hpp"
#include <cstring>
#include <algorithm>

namespace sphaira::ui::menu::mtp {
namespace {

constexpr u64 MAX_BUFFER_SIZE = 1024*1024*32;
constexpr u64 SLEEPNS = 1000;
volatile bool IN_PUSH_THREAD{};

auto GetUsbStateStr(UsbState state) -> const char* {
    switch (state) {
        case UsbState_Detached: return "Detached";
        case UsbState_Attached: return "Attached";
        case UsbState_Powered: return "Powered";
        case UsbState_Default: return "Default";
        case UsbState_Address: return "Address";
        case UsbState_Configured: return "Configured";
        case UsbState_Suspended: return "Suspended";
    }

    return "Unknown";
}

auto GetUsbSpeedStr(UsbDeviceSpeed speed) -> const char* {
    // todo: remove this cast when libnx pr is merged.
    switch ((u32)speed) {
        case UsbDeviceSpeed_None: return "None";
        case UsbDeviceSpeed_Low: return "USB 1.0 Low Speed";
        case UsbDeviceSpeed_Full: return "USB 1.1 Full Speed";
        case UsbDeviceSpeed_High: return "USB 2.0 High Speed";
        case UsbDeviceSpeed_Super: return "USB 3.0 Super Speed";
    }

    return "Unknown";
}

bool OnInstallStart(void* user, const char* path) {
    auto menu = (Menu*)user;
    log_write("[INSTALL] inside OnInstallStart()\n");

    for (;;) {
        mutexLock(&menu->m_mutex);
        ON_SCOPE_EXIT(mutexUnlock(&menu->m_mutex));

        if (menu->m_state != State::Progress) {
            break;
        }

        if (menu->GetToken().stop_requested()) {
            return false;
        }

        svcSleepThread(1e+6);
    }

    log_write("[INSTALL] OnInstallStart() got state: %u\n", (u8)menu->m_state);

    if (menu->m_source) {
        log_write("[INSTALL] OnInstallStart() we have source\n");
        for (;;) {
            mutexLock(&menu->m_source->m_mutex);
            ON_SCOPE_EXIT(mutexUnlock(&menu->m_source->m_mutex));

            if (!IN_PUSH_THREAD) {
                break;
            }

            if (menu->GetToken().stop_requested()) {
                return false;
            }

            svcSleepThread(1e+6);
        }

        log_write("[INSTALL] OnInstallStart() stopped polling source\n");
    }

    log_write("[INSTALL] OnInstallStart() doing make_shared\n");
    menu->m_source = std::make_shared<StreamFtp>(path, menu->GetToken());

    mutexLock(&menu->m_mutex);
    ON_SCOPE_EXIT(mutexUnlock(&menu->m_mutex));
    menu->m_state = State::Connected;
    log_write("[INSTALL] OnInstallStart() done make shared\n");

    return true;
}

bool OnInstallWrite(void* user, const void* buf, size_t size) {
    auto menu = (Menu*)user;

    return menu->m_source->Push(buf, size);
}

void OnInstallClose(void* user) {
    auto menu = (Menu*)user;
    menu->m_source->Disable();
}

} // namespace

StreamFtp::StreamFtp(const fs::FsPath& path, std::stop_token token) {
    m_path = path;
    m_token = token;
    m_buffer.reserve(MAX_BUFFER_SIZE);
    m_active = true;
}

Result StreamFtp::ReadChunk(void* buf, s64 size, u64* bytes_read) {
    while (!m_token.stop_requested()) {
        mutexLock(&m_mutex);
        ON_SCOPE_EXIT(mutexUnlock(&m_mutex));

        if (m_buffer.empty()) {
            if (!m_active) {
                break;
            }

            svcSleepThread(SLEEPNS);
        } else {
            size = std::min<s64>(size, m_buffer.size());
            std::memcpy(buf, m_buffer.data(), size);
            m_buffer.erase(m_buffer.begin(), m_buffer.begin() + size);
            *bytes_read = size;
            R_SUCCEED();
        }
    }

    return 0x1;
}

bool StreamFtp::Push(const void* buf, s64 size) {
    IN_PUSH_THREAD = true;
    ON_SCOPE_EXIT(IN_PUSH_THREAD = false);

    while (!m_token.stop_requested()) {
        mutexLock(&m_mutex);
        ON_SCOPE_EXIT(mutexUnlock(&m_mutex));

        if (!m_active) {
            break;
        }

        if (m_buffer.size() + size >= MAX_BUFFER_SIZE) {
            svcSleepThread(SLEEPNS);
        } else {
            const auto offset = m_buffer.size();
            m_buffer.resize(offset + size);
            std::memcpy(m_buffer.data() + offset, buf, size);
            return true;
        }
    }

    return false;
}

void StreamFtp::Disable() {
    mutexLock(&m_mutex);
    ON_SCOPE_EXIT(mutexUnlock(&m_mutex));
    m_active = false;
}

Menu::Menu(u32 flags) : MenuBase{"MTP Install"_i18n, flags} {
    SetAction(Button::B, Action{"Back"_i18n, [this](){
        SetPop();
    }});

    SetAction(Button::X, Action{"Options"_i18n, [this](){
        App::DisplayInstallOptions(false);
    }});

    App::SetAutoSleepDisabled(true);

    mutexInit(&m_mutex);
    m_was_mtp_enabled = App::GetMtpEnable();
    if (!m_was_mtp_enabled) {
        log_write("[MTP] wasn't enabled, forcefully enabling\n");
        App::SetMtpEnable(true);
    }

    haze::InitInstallMode(this, OnInstallStart, OnInstallWrite, OnInstallClose);
}

Menu::~Menu() {
    // signal for thread to exit and wait.
    haze::DisableInstallMode();
    m_stop_source.request_stop();

    if (m_source) {
        m_source->Disable();
    }

    if (!m_was_mtp_enabled) {
        log_write("[MTP] disabling on exit\n");
        App::SetMtpEnable(false);
    }

    App::SetAutoSleepDisabled(false);
    log_write("closing data!!!!\n");
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    static TimeStamp poll_ts;
    if (poll_ts.GetSeconds() >= 1) {
        poll_ts.Update();

        UsbState state{UsbState_Detached};
        usbDsGetState(&state);

        UsbDeviceSpeed speed{(UsbDeviceSpeed)UsbDeviceSpeed_None};
        usbDsGetSpeed(&speed);

        char buf[128];
        std::snprintf(buf, sizeof(buf), "State: %s | Speed: %s", i18n::get(GetUsbStateStr(state)).c_str(), i18n::get(GetUsbSpeedStr(speed)).c_str());
        SetSubHeading(buf);
    }

    mutexLock(&m_mutex);
    ON_SCOPE_EXIT(mutexUnlock(&m_mutex));

    if (m_state == State::Connected) {
        log_write("set to progress\n");
        m_state = State::Progress;
        log_write("got connection\n");
        App::Push(std::make_shared<ui::ProgressBox>(0, "Installing "_i18n, "", [this](auto pbox) -> Result {
            log_write("inside progress box\n");
            const auto rc = yati::InstallFromSource(pbox, m_source, m_source->m_path);
            if (R_FAILED(rc)) {
                m_source->Disable();
                R_THROW(rc);
            }

            R_SUCCEED();
        }, [this](Result rc){
            App::PushErrorBox(rc, "MTP install failed!"_i18n);


            mutexLock(&m_mutex);
            ON_SCOPE_EXIT(mutexUnlock(&m_mutex));

            if (R_SUCCEEDED(rc)) {
                App::Notify("MTP install success!"_i18n);
                m_state = State::Done;
            } else {
                m_state = State::Failed;
                haze::DisableInstallMode();
            }
        }));
    }
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    mutexLock(&m_mutex);
    ON_SCOPE_EXIT(mutexUnlock(&m_mutex));

    switch (m_state) {
        case State::None:
        case State::Done:
            gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Drag'n'Drop (NSP, XCI, NSZ, XCZ) to the install folder on PC"_i18n.c_str());
            break;

        case State::Connected:
        case State::Progress:
            break;

        case State::Failed:
            gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Failed to install via MTP, press B to exit..."_i18n.c_str());
            break;
    }
}

void Menu::OnFocusGained() {
    MenuBase::OnFocusGained();
}

} // namespace sphaira::ui::menu::mtp

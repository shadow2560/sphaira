#pragma once

#include "widget.hpp"
#include "fs.hpp"
#include <functional>

namespace sphaira::ui {

struct ProgressBox;
using ProgressBoxCallback = std::function<bool(ProgressBox*)>;
using ProgressBoxDoneCallback = std::function<void(bool success)>;

struct ProgressBox final : Widget {
    ProgressBox(
        const std::string& title,
        ProgressBoxCallback callback, ProgressBoxDoneCallback done = [](bool success){},
        int cpuid = 1, int prio = 0x2C, int stack_size = 1024*1024
    );
    ~ProgressBox();

    auto Update(Controller* controller, TouchInfo* touch) -> void override;
    auto Draw(NVGcontext* vg, Theme* theme) -> void override;

    auto NewTransfer(const std::string& transfer) -> ProgressBox&;
    auto UpdateTransfer(s64 offset, s64 size) -> ProgressBox&;
    void RequestExit();
    auto ShouldExit() -> bool;

    // helper functions
    auto CopyFile(const fs::FsPath& src, const fs::FsPath& dst) -> Result;
    void Yield();

    auto OnDownloadProgressCallback() {
        return [this](u32 dltotal, u32 dlnow, u32 ultotal, u32 ulnow){
            if (this->ShouldExit()) {
                return false;
            }
            this->UpdateTransfer(dlnow, dltotal);
            return true;
        };
    }

public:
    struct ThreadData {
        ProgressBox* pbox;
        ProgressBoxCallback callback;
        bool result;
    };

private:
    Mutex m_mutex{};
    Thread m_thread{};
    ThreadData m_thread_data{};

    ProgressBoxDoneCallback m_done{};
    std::string m_title{};
    std::string m_transfer{};
    s64 m_size{};
    s64 m_offset{};
    bool m_exit_requested{};
};

// this is a helper function that does many things.
// 1. creates a progress box, pushes that box to app
// 2. creates a thread and passes the pbox and callback to that thread
// 3. that thread calls the callback.

// this allows for blocking processes to run on a seperate thread whilst
// updating the ui with the progress of the operation.
// the callback should poll ShouldExit() whether to keep running

} // namespace sphaira::ui {

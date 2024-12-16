#pragma once

#include <vector>
#include <string>
#include <functional>
#include <switch.h>

namespace sphaira {

using DownloadCallback = std::function<void(std::vector<u8>& data, bool success)>;
using ProgressCallback = std::function<bool(u32 dltotal, u32 dlnow, u32 ultotal, u32 ulnow)>;

enum class DownloadPriority {
    Normal, // gets pushed to the back of the queue
    High, // gets pushed to the front of the queue
};

struct DownloadEventData {
    DownloadCallback callback;
    std::vector<u8> data;
    bool result;
};

auto DownloadInit() -> bool;
void DownloadExit();

// sync functions
auto DownloadMemory(const std::string& url, const std::string& post, ProgressCallback pcallback = nullptr) -> std::vector<u8>;
auto DownloadFile(const std::string& url, const std::string& out, const std::string& post, ProgressCallback pcallback = nullptr) -> bool;
// async functions
// starts the downloads in a new thread, pushes an event when complete
// then, the callback will be called on the main thread.
// auto DownloadMemoryAsync(const std::string& url, DownloadCallback callback, DownloadPriority prio = DownloadPriority::Normal) -> bool;
// auto DownloadFileAsync(const std::string& url, const std::string& out, DownloadCallback callback, DownloadPriority prio = DownloadPriority::Normal) -> bool;

auto DownloadMemoryAsync(const std::string& url, const std::string& post, DownloadCallback callback, ProgressCallback pcallback = nullptr, DownloadPriority prio = DownloadPriority::Normal) -> bool;
auto DownloadFileAsync(const std::string& url, const std::string& out, const std::string& post, DownloadCallback callback, ProgressCallback pcallback = nullptr, DownloadPriority prio = DownloadPriority::Normal) -> bool;

void DownloadClearCache(const std::string& url);

} // namespace sphaira

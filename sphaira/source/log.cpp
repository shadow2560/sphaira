#include "log.hpp"
#include <cstdio>
#include <cstdarg>
#include <unistd.h>
#include <mutex>
#include <switch.h>

#if sphaira_USE_LOG
namespace {

constexpr const char* logpath = "/config/sphaira/log.txt";

std::FILE* file{};
int nxlink_socket{};
std::mutex mutex{};

void log_write_arg_internal(const char* s, std::va_list* v) {
    if (file) {
        std::vfprintf(file, s, *v);
        std::fflush(file);
    }
    if (nxlink_socket) {
        std::vprintf(s, *v);
    }
}

} // namespace

extern "C" {

auto log_file_init() -> bool {
    std::scoped_lock lock{mutex};
    if (file) {
        return false;
    }

    file = std::fopen(logpath, "w");
    return file != nullptr;
}

auto log_nxlink_init() -> bool {
    std::scoped_lock lock{mutex};
    if (nxlink_socket) {
        return false;
    }

    nxlink_socket = nxlinkConnectToHost(true, false);
    return nxlink_socket != 0;
}

void log_file_exit() {
    std::scoped_lock lock{mutex};
    if (file) {
        std::fclose(file);
        file = nullptr;
    }
}

void log_nxlink_exit() {
    std::scoped_lock lock{mutex};
    if (nxlink_socket) {
        close(nxlink_socket);
        nxlink_socket = 0;
    }
}

void log_write(const char* s, ...) {
    std::scoped_lock lock{mutex};
    if (!file && !nxlink_socket) {
        return;
    }

    std::va_list v{};
    va_start(v, s);
    log_write_arg_internal(s, &v);
    va_end(v);
}

void log_write_arg(const char* s, va_list* v) {
    std::scoped_lock lock{mutex};
    if (!file && !nxlink_socket) {
        return;
    }

    log_write_arg_internal(s, v);
}

} // extern "C"

#endif

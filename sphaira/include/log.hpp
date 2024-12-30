#pragma once

#define sphaira_USE_LOG 1

#include <cstdarg>

#if sphaira_USE_LOG
auto log_file_init() -> bool;
auto log_nxlink_init() -> bool;
void log_file_exit();
void log_nxlink_exit();
void log_write(const char* s, ...) __attribute__ ((format (printf, 1, 2)));
void log_write_arg(const char* s, std::va_list& v);
#else
inline auto log_file_init() -> bool {
    return true;
}
inline auto log_nxlink_init() -> bool {
    return true;
}
#define log_file_exit()
#define log_nxlink_exit()
#define log_write(...)
#endif

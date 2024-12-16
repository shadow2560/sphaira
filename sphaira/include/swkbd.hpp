#pragma once

#include <switch.h>
#include <string>

namespace sphaira::swkbd {

Result ShowText(std::string& out, const char* guide = nullptr, s64 len_min = -1, s64 len_max = -1);
Result ShowNumPad(s64& out, const char* guide = nullptr, s64 len_min = -1, s64 len_max = -1);

} // namespace sphaira::swkbd

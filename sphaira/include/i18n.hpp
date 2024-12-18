#pragma once

#include <string>

namespace sphaira::i18n {

bool init(long index);
void exit();

std::string get(const char* str);

} // namespace sphaira::i18n

inline namespace literals {

std::string operator"" _i18n(const char* str, size_t len);

} // namespace literals

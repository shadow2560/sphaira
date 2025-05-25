#include <minIni.h>
#include <type_traits>
#include "option.hpp"
#include "app.hpp"

#include <cctype>
#include <cstring>
#include <cstdlib>

namespace sphaira::option {
namespace {

// these are taken from minini in order to parse a value already loaded in memory.
long getl(const char* LocalBuffer, long def) {
    const auto len = strlen(LocalBuffer);
    return (len == 0) ? def
                    : ((len >= 2 && toupper((int)LocalBuffer[1]) == 'X') ? strtol(LocalBuffer, NULL, 16)
                                                                           : strtol(LocalBuffer, NULL, 10));
}

bool getbool(const char* LocalBuffer, bool def) {
    const auto c = toupper(LocalBuffer[0]);

    if (c == 'Y' || c == '1' || c == 'T')
    return true;
        else if (c == 'N' || c == '0' || c == 'F')
    return false;
        else
    return def;
}

} // namespace

template<typename T>
auto OptionBase<T>::GetInternal(const char* name) -> T {
    if (!m_value.has_value()) {
        if constexpr(std::is_same_v<T, bool>) {
            m_value = ini_getbool(m_section.c_str(), name, m_default_value, App::CONFIG_PATH);
        } else if constexpr(std::is_same_v<T, long>) {
            m_value = ini_getl(m_section.c_str(), name, m_default_value, App::CONFIG_PATH);
        } else if constexpr(std::is_same_v<T, std::string>) {
            char buf[FS_MAX_PATH];
            ini_gets(m_section.c_str(), name, m_default_value.c_str(), buf, sizeof(buf), App::CONFIG_PATH);
            m_value = buf;
        }
    }
    return m_value.value();
}

template<typename T>
auto OptionBase<T>::Get() -> T {
    return GetInternal(m_name.c_str());
}

template<typename T>
auto OptionBase<T>::GetOr(const char* name) -> T {
    if (ini_haskey(m_section.c_str(), m_name.c_str(), App::CONFIG_PATH)) {
        return Get();
    } else {
        return GetInternal(name);
    }
}

template<typename T>
void OptionBase<T>::Set(T value) {
    m_value = value;
    if constexpr(std::is_same_v<T, bool>) {
        ini_putl(m_section.c_str(), m_name.c_str(), value, App::CONFIG_PATH);
    } else if constexpr(std::is_same_v<T, long>) {
        ini_putl(m_section.c_str(), m_name.c_str(), value, App::CONFIG_PATH);
    } else if constexpr(std::is_same_v<T, std::string>) {
        ini_puts(m_section.c_str(), m_name.c_str(), value.c_str(), App::CONFIG_PATH);
    }
}

template<typename T>
auto OptionBase<T>::LoadFrom(const char* section, const char* name, const char* value) -> bool {
    return m_section == section && LoadFrom(name, value);
}

template<typename T>
auto OptionBase<T>::LoadFrom(const char* name, const char* value) -> bool {
    if (m_name == name) {
        if constexpr(std::is_same_v<T, bool>) {
            m_value = getbool(value, m_default_value);
        } else if constexpr(std::is_same_v<T, long>) {
            m_value = getl(value, m_default_value);
        } else if constexpr(std::is_same_v<T, std::string>) {
            m_value = value;
        }

        return true;
    }

    return false;
}

template struct OptionBase<bool>;
template struct OptionBase<long>;
template struct OptionBase<std::string>;

} //  namespace sphaira::option

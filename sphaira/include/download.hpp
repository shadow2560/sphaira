#pragma once

#include "fs.hpp"
#include <vector>
#include <string>
#include <functional>
#include <unordered_map>
#include <type_traits>
#include <switch.h>

namespace sphaira::curl {

enum class Priority {
    Normal, // gets pushed to the back of the queue
    High, // gets pushed to the front of the queue
};

using Path = fs::FsPath;
using Header = std::unordered_map<std::string, std::string>;
using OnComplete = std::function<void(std::vector<u8>& data, bool success, long code)>;
using OnProgress = std::function<bool(u32 dltotal, u32 dlnow, u32 ultotal, u32 ulnow)>;

struct Url {
    Url() = default;
    Url(const std::string& str) : m_str{str} {}
    std::string m_str;
};

struct Fields {
    Fields() = default;
    Fields(const std::string& str) : m_str{str} {}
    std::string m_str;
};

struct Api;

struct DownloadEventData {
    OnComplete callback;
    std::vector<u8> data;
    long code;
    bool result;
};

auto Init() -> bool;
void Exit();

// sync functions
auto ToMemory(const Api& e) -> std::vector<u8>;
auto ToFile(const Api& e) -> bool;

// async functions
auto ToMemoryAsync(const Api& e) -> bool;
auto ToFileAsync(const Api& e) -> bool;

// removes url from cache (todo: deprecate this)
void ClearCache(const Url& url);

struct Api {
    Api() = default;

    template <typename... Ts>
    Api(Ts&&... ts) {
        Api::set_option(std::forward<Ts>(ts)...);
    }

    template <typename... Ts>
    auto To(Ts&&... ts) {
        if constexpr(std::disjunction_v<std::is_same<Path, Ts>...>) {
            return ToFile(std::forward<Ts>(ts)...);
        } else {
            return ToMemory(std::forward<Ts>(ts)...);
        }
    }

    template <typename... Ts>
    auto ToAsync(Ts&&... ts) {
        if constexpr(std::disjunction_v<std::is_same<Path, Ts>...>) {
            return ToFileAsync(std::forward<Ts>(ts)...);
        } else {
            return ToMemoryAsync(std::forward<Ts>(ts)...);
        }
    }

    template <typename... Ts>
    auto ToMemory(Ts&&... ts) {
        static_assert(std::disjunction_v<std::is_same<Url, Ts>...>, "Url must be specified");
        Api::set_option(std::forward<Ts>(ts)...);
        return curl::ToMemory(*this);
    }

    template <typename... Ts>
    auto ToFile(Ts&&... ts) {
        static_assert(std::disjunction_v<std::is_same<Url, Ts>...>, "Url must be specified");
        static_assert(std::disjunction_v<std::is_same<Path, Ts>...>, "Path must be specified");
        Api::set_option(std::forward<Ts>(ts)...);
        return curl::ToFile(*this);
    }

    template <typename... Ts>
    auto ToMemoryAsync(Ts&&... ts) {
        static_assert(std::disjunction_v<std::is_same<Url, Ts>...>, "Url must be specified");
        static_assert(std::disjunction_v<std::is_same<OnComplete, Ts>...>, "OnComplete must be specified");
        Api::set_option(std::forward<Ts>(ts)...);
        return curl::ToMemoryAsync(*this);
    }

    template <typename... Ts>
    auto ToFileAsync(Ts&&... ts) {
        static_assert(std::disjunction_v<std::is_same<Url, Ts>...>, "Url must be specified");
        static_assert(std::disjunction_v<std::is_same<Path, Ts>...>, "Path must be specified");
        static_assert(std::disjunction_v<std::is_same<OnComplete, Ts>...>, "OnComplete must be specified");
        Api::set_option(std::forward<Ts>(ts)...);
        return curl::ToFileAsync(*this);
    }

    Url m_url;
    Fields m_fields{};
    Header m_header{};
    Path m_path{};
    OnComplete m_on_complete = nullptr;
    OnProgress m_on_progress = nullptr;
    Priority m_prio = Priority::Normal;

private:
    void SetOption(Url&& v) {
        m_url = v;
    }
    void SetOption(Fields&& v) {
        m_fields = v;
    }
    void SetOption(Header&& v) {
        m_header = v;
    }
    void SetOption(Path&& v) {
        m_path = v;
    }
    void SetOption(OnComplete&& v) {
        m_on_complete = v;
    }
    void SetOption(OnProgress&& v) {
        m_on_progress = v;
    }
    void SetOption(Priority&& v) {
        m_prio = v;
    }

    template <typename T>
    void set_option(T&& t) {
        SetOption(std::forward<T>(t));
    }

    template <typename T, typename... Ts>
    void set_option(T&& t, Ts&&... ts) {
        set_option(std::forward<T>(t));
        set_option(std::forward<Ts>(ts)...);
    }
};

} // namespace sphaira::curl

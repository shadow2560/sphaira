#pragma once

#include "fs.hpp"
#include <vector>
#include <string>
#include <functional>
#include <unordered_map>
#include <algorithm>
#include <stop_token>
#include <switch.h>

namespace sphaira::curl {

enum {
    Flag_None = 0,
    // requests to download send etag in the header.
    // the received etag is then saved on success.
    // this api is only available on downloading to file.
    Flag_Cache = 1 << 0,
};

enum class Priority {
    Normal, // gets pushed to the back of the queue
    High, // gets pushed to the front of the queue
};

struct Api;
struct ApiResult;

using Path = fs::FsPath;
using OnComplete = std::function<void(ApiResult& result)>;
using OnProgress = std::function<bool(u32 dltotal, u32 dlnow, u32 ultotal, u32 ulnow)>;
using StopToken = std::stop_token;

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

struct Header {
    Header() = default;
    Header(std::initializer_list<std::pair<const std::string, std::string>> p) : m_map{p} {}
    std::unordered_map<std::string, std::string> m_map;

    auto Find(const std::string& key) const {
        return std::find_if(m_map.cbegin(), m_map.cend(), [&key](auto& e) {
            return !strcasecmp(key.c_str(), e.first.c_str());
        });
    }
};

struct Flags {
    Flags() = default;
    Flags(u32 flags) : m_flags{flags} {}
    u32 m_flags{Flag_None};
};

struct ApiResult {
    bool success;
    long code;
    Header header; // returned headers in request
    std::vector<u8> data; // empty if downloaded a file
    fs::FsPath path; // empty if downloaded memory
};

struct DownloadEventData {
    OnComplete callback;
    ApiResult result;
    StopToken stoken;
};

auto Init() -> bool;
void Exit();

// sync functions
auto ToMemory(const Api& e) -> ApiResult;
auto ToFile(const Api& e) -> ApiResult;

// async functions
auto ToMemoryAsync(const Api& e) -> bool;
auto ToFileAsync(const Api& e) -> bool;

// uses curl to convert string to their %XX
auto EscapeString(const std::string& str) -> std::string;

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
        static_assert(!std::disjunction_v<std::is_same<Path, Ts>...>, "Path must not valid for memory");
        static_assert(!std::disjunction_v<std::is_same<OnComplete, Ts>...>, "OnComplete must not be specified");
        Api::set_option(std::forward<Ts>(ts)...);
        return curl::ToMemory(*this);
    }

    template <typename... Ts>
    auto ToFile(Ts&&... ts) {
        static_assert(std::disjunction_v<std::is_same<Url, Ts>...>, "Url must be specified");
        static_assert(std::disjunction_v<std::is_same<Path, Ts>...>, "Path must be specified");
        static_assert(!std::disjunction_v<std::is_same<OnComplete, Ts>...>, "OnComplete must not be specified");
        Api::set_option(std::forward<Ts>(ts)...);
        return curl::ToFile(*this);
    }

    template <typename... Ts>
    auto ToMemoryAsync(Ts&&... ts) {
        static_assert(std::disjunction_v<std::is_same<Url, Ts>...>, "Url must be specified");
        static_assert(std::disjunction_v<std::is_same<OnComplete, Ts>...>, "OnComplete must be specified");
        static_assert(!std::disjunction_v<std::is_same<Path, Ts>...>, "Path must not valid for memory");
        static_assert(std::disjunction_v<std::is_same<StopToken, Ts>...>, "StopToken must be specified");
        Api::set_option(std::forward<Ts>(ts)...);
        return curl::ToMemoryAsync(*this);
    }

    template <typename... Ts>
    auto ToFileAsync(Ts&&... ts) {
        static_assert(std::disjunction_v<std::is_same<Url, Ts>...>, "Url must be specified");
        static_assert(std::disjunction_v<std::is_same<Path, Ts>...>, "Path must be specified");
        static_assert(std::disjunction_v<std::is_same<OnComplete, Ts>...>, "OnComplete must be specified");
        static_assert(std::disjunction_v<std::is_same<StopToken, Ts>...>, "StopToken must be specified");
        Api::set_option(std::forward<Ts>(ts)...);
        return curl::ToFileAsync(*this);
    }

    auto& GetUrl() const {
        return m_url.m_str;
    }
    auto& GetFields() const {
        return m_fields.m_str;
    }
    auto& GetHeader() const {
        return m_header;
    }
    auto& GetFlags() const {
        return m_flags.m_flags;
    }
    auto& GetPath() const {
        return m_path;
    }
    auto& GetOnComplete() const {
        return m_on_complete;
    }
    auto& GetOnProgress() const {
        return m_on_progress;
    }
    auto& GetPriority() const {
        return m_prio;
    }
    auto& GetToken() const {
        return m_stoken;
    }

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
    void SetOption(Flags&& v) {
        m_flags = v;
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
    void SetOption(StopToken&& v) {
        m_stoken = v;
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

private:
    Url m_url;
    Fields m_fields{};
    Header m_header{};
    Flags m_flags{};
    Path m_path{};
    OnComplete m_on_complete{nullptr};
    OnProgress m_on_progress{nullptr};
    Priority m_prio{Priority::High};
    std::stop_source m_stop_source{};
    StopToken m_stoken{m_stop_source.get_token()};
};

} // namespace sphaira::curl

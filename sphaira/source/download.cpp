#include "download.hpp"
#include "log.hpp"
#include "defines.hpp"
#include "evman.hpp"
#include "fs.hpp"
#include <switch.h>
#include <cstring>
#include <cassert>
#include <vector>
#include <deque>
#include <mutex>
#include <curl/curl.h>

namespace sphaira {
namespace {

#define CURL_EASY_SETOPT_LOG(handle, opt, v) \
    if (auto r = curl_easy_setopt(handle, opt, v); r != CURLE_OK) { \
        log_write("curl_easy_setopt(%s, %s) msg: %s\n", #opt, #v, curl_easy_strerror(r)); \
    } \

#define CURL_SHARE_SETOPT_LOG(handle, opt, v) \
    if (auto r = curl_share_setopt(handle, opt, v); r != CURLSHE_OK) { \
        log_write("curl_share_setopt(%s, %s) msg: %s\n", #opt, #v, curl_share_strerror(r)); \
    } \

void DownloadThread(void* p);
void DownloadThreadQueue(void* p);

#define USE_THREAD_QUEUE 1
constexpr auto API_AGENT = "ITotalJustice";
constexpr u64 CHUNK_SIZE = 1024*1024;
constexpr auto MAX_THREADS = 4;
constexpr int THREAD_PRIO = 0x2C;
constexpr int THREAD_CORE = 1;

std::atomic_bool g_running{};
CURLSH* g_curl_share{};
Mutex g_mutex_share[CURL_LOCK_DATA_LAST]{};

struct UrlCache {
    auto AddToCache(const std::string& url, bool force = false) {
        mutexLock(&mutex);
        ON_SCOPE_EXIT(mutexUnlock(&mutex));
        auto it = std::find(cache.begin(), cache.end(), url);
        if (it == cache.end()) {
            cache.emplace_back(url);
            return true;
        } else {
            if (force) {
                return true;
            } else {
                return false;
            }
        }
    }

    void RemoveFromCache(const std::string& url) {
        mutexLock(&mutex);
        ON_SCOPE_EXIT(mutexUnlock(&mutex));
        auto it = std::find(cache.begin(), cache.end(), url);
        if (it != cache.end()) {
            cache.erase(it);
        }
    }

    std::vector<std::string> cache;
    Mutex mutex{};
};

struct DataStruct {
    std::vector<u8> data;
    u64 offset{};
    FsFileSystem fs{};
    FsFile f{};
    s64 file_offset{};
};

struct ThreadEntry {
    auto Create() -> Result {
        m_curl = curl_easy_init();
        R_UNLESS(m_curl != nullptr, 0x1);

        ueventCreate(&m_uevent, true);
        R_TRY(threadCreate(&m_thread, DownloadThread, this, nullptr, 1024*32, THREAD_PRIO, THREAD_CORE));
        R_TRY(threadStart(&m_thread));
        R_SUCCEED();
    }

    void Close() {
        ueventSignal(&m_uevent);
        threadWaitForExit(&m_thread);
        threadClose(&m_thread);
        if (m_curl) {
            curl_easy_cleanup(m_curl);
            m_curl = nullptr;
        }
    }

    auto InProgress() -> bool {
        return m_in_progress == true;
    }

    auto Setup(DownloadCallback callback, ProgressCallback pcallback, std::string url, std::string file, std::string post) -> bool {
        assert(m_in_progress == false && "Setting up thread while active");
        mutexLock(&m_mutex);
        ON_SCOPE_EXIT(mutexUnlock(&m_mutex));

        if (m_in_progress) {
            return false;
        }
        m_url = url;
        m_file = file;
        m_post = post;
        m_callback = callback;
        m_pcallback = pcallback;
        m_in_progress = true;
        // log_write("started download :)\n");
        ueventSignal(&m_uevent);
        return true;
    }

    CURL* m_curl{};
    Thread m_thread{};
    std::string m_url{};
    std::string m_file{}; // if empty, downloads to buffer
    std::string m_post{}; // if empty, downloads to buffer
    DownloadCallback m_callback{};
    ProgressCallback m_pcallback{};
    std::atomic_bool m_in_progress{};
    Mutex m_mutex{};
    UEvent m_uevent{};
};

struct ThreadQueueEntry {
    std::string url;
    std::string file;
    std::string post;
    DownloadCallback callback;
    ProgressCallback pcallback;
    bool m_delete{};
};

struct ThreadQueue {
    std::deque<ThreadQueueEntry> m_entries;
    Thread m_thread;
    Mutex m_mutex{};
    UEvent m_uevent{};

    auto Create() -> Result {
        ueventCreate(&m_uevent, true);
        R_TRY(threadCreate(&m_thread, DownloadThreadQueue, this, nullptr, 1024*32, THREAD_PRIO, THREAD_CORE));
        R_TRY(threadStart(&m_thread));
        R_SUCCEED();
    }

    void Close() {
        ueventSignal(&m_uevent);
        threadWaitForExit(&m_thread);
        threadClose(&m_thread);
    }

    auto Add(DownloadPriority prio, DownloadCallback callback, ProgressCallback pcallback, std::string url, std::string file, std::string post) -> bool {
        mutexLock(&m_mutex);
        ON_SCOPE_EXIT(mutexUnlock(&m_mutex));

        ThreadQueueEntry entry{};
        entry.url = url;
        entry.file = file;
        entry.post = post;
        entry.callback = callback;
        entry.pcallback = pcallback;

        switch (prio) {
            case DownloadPriority::Normal:
                m_entries.emplace_back(entry);
                break;
            case DownloadPriority::High:
                m_entries.emplace_front(entry);
                break;
        }

        ueventSignal(&m_uevent);
        return true;
    }
};

ThreadEntry g_threads[MAX_THREADS]{};
ThreadQueue g_thread_queue;
UrlCache g_url_cache;

void GetDownloadTempPath(fs::FsPath& buf) {
    static Mutex mutex{};
    static u64 count{};

    mutexLock(&mutex);
    const auto count_copy = count;
    count++;
    mutexUnlock(&mutex);

    std::snprintf(buf, sizeof(buf), "/switch/sphaira/cache/download_temp%lu", count_copy);
}

auto ProgressCallbackFunc1(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) -> size_t {
    if (!g_running) {
        return 1;
    }

    svcSleepThread(YieldType_WithoutCoreMigration);
    return 0;
}

auto ProgressCallbackFunc2(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) -> size_t {
    if (!g_running) {
        return 1;
    }

    // log_write("pcall called %u %u %u %u\n", dltotal, dlnow, ultotal, ulnow);
    auto callback = *static_cast<ProgressCallback*>(clientp);
    if (!callback(dltotal, dlnow, ultotal, ulnow)) {
        return 1;
    }

    svcSleepThread(YieldType_WithoutCoreMigration);
    return 0;
}

auto WriteMemoryCallback(void *contents, size_t size, size_t num_files, void *userp) -> size_t {
    if (!g_running) {
        return 0;
    }

    auto data_struct = static_cast<DataStruct*>(userp);
    const auto realsize = size * num_files;

    // give it more memory
    if (data_struct->data.capacity() < data_struct->offset + realsize) {
        data_struct->data.reserve(data_struct->data.capacity() + CHUNK_SIZE);
    }

    data_struct->data.resize(data_struct->offset + realsize);
    std::memcpy(data_struct->data.data() + data_struct->offset, contents, realsize);

    data_struct->offset += realsize;

    svcSleepThread(YieldType_WithoutCoreMigration);

    return realsize;
}

auto WriteFileCallback(void *contents, size_t size, size_t num_files, void *userp) -> size_t {
    if (!g_running) {
        return 0;
    }

    auto data_struct = static_cast<DataStruct*>(userp);
    const auto realsize = size * num_files;

    // flush data if incomming data would overflow the buffer
    if (data_struct->offset && data_struct->data.size() < data_struct->offset + realsize) {
        if (R_FAILED(fsFileWrite(&data_struct->f, data_struct->file_offset, data_struct->data.data(), data_struct->offset, FsWriteOption_None))) {
            return 0;
        }

        data_struct->file_offset += data_struct->offset;
        data_struct->offset = 0;
    }

    // we have a huge chunk! write it directly to file
    if (data_struct->data.size() < realsize) {
        if (R_FAILED(fsFileWrite(&data_struct->f, data_struct->file_offset, contents, realsize, FsWriteOption_None))) {
            return 0;
        }

        data_struct->file_offset += realsize;
    } else {
        // buffer data until later
        std::memcpy(data_struct->data.data() + data_struct->offset, contents, realsize);
        data_struct->offset += realsize;
    }

    svcSleepThread(YieldType_WithoutCoreMigration);
    return realsize;
}

auto DownloadInternal(CURL* curl, DataStruct& chunk, ProgressCallback pcallback, const std::string& url, const std::string& file, const std::string& post) -> bool {
    fs::FsPath safe_buf;
    fs::FsPath tmp_buf;
    const bool has_file = !file.empty() && file != "";
    const bool has_post = !post.empty() && post != "";

    ON_SCOPE_EXIT(if (has_file) { fsFsClose(&chunk.fs); } );

    if (has_file) {
        std::strcpy(safe_buf, file.c_str());
        GetDownloadTempPath(tmp_buf);
        R_TRY_RESULT(fsOpenSdCardFileSystem(&chunk.fs), false);

        fs::CreateDirectoryRecursivelyWithPath(&chunk.fs, tmp_buf);

        if (auto rc = fsFsCreateFile(&chunk.fs, tmp_buf, 0, 0); R_FAILED(rc) && rc != FsError_ResultPathAlreadyExists) {
            log_write("failed to create file: %s\n", tmp_buf);
            return false;
        }

        if (R_FAILED(fsFsOpenFile(&chunk.fs, tmp_buf, FsOpenMode_Write|FsOpenMode_Append, &chunk.f))) {
            log_write("failed to open file: %s\n", tmp_buf);
            return false;
        }
    }

    // reserve the first chunk
    chunk.data.reserve(CHUNK_SIZE);

    CURL_EASY_SETOPT_LOG(curl, CURLOPT_URL, url.c_str());
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_USERAGENT, "TotalJustice");
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_FAILONERROR, 1L);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_SHARE, g_curl_share);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_BUFFERSIZE, 1024*512);

    if (has_post) {
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_POSTFIELDS, post.c_str());
        log_write("setting post field: %s\n", post.c_str());
    }

    // progress calls.
    if (pcallback) {
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_XFERINFODATA, &pcallback);
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallbackFunc2);
    } else {
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallbackFunc1);
    }
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_NOPROGRESS, 0L);

    // write calls.
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_WRITEFUNCTION, has_file ? WriteFileCallback : WriteMemoryCallback);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_WRITEDATA, &chunk);

    // perform download and cleanup after and report the result.
    const auto res = curl_easy_perform(curl);
    bool success = res == CURLE_OK;

    if (has_file) {
        if (res == CURLE_OK && chunk.offset) {
            fsFileWrite(&chunk.f, chunk.file_offset, chunk.data.data(), chunk.offset, FsWriteOption_None);
        }
        fsFileClose(&chunk.f);
        if (res != CURLE_OK) {
            fsFsDeleteFile(&chunk.fs, tmp_buf);
        } else {
            fsFsDeleteFile(&chunk.fs, safe_buf);
            fs::CreateDirectoryRecursivelyWithPath(&chunk.fs, safe_buf);
            if (R_FAILED(fsFsRenameFile(&chunk.fs, tmp_buf, safe_buf))) {
                fsFsDeleteFile(&chunk.fs, tmp_buf);
                success = false;
            }
        }
    } else {
        // empty data if we failed
        if (res != CURLE_OK) {
            chunk.data.clear();
        }
    }

    log_write("Downloaded %s %s\n", url.c_str(), curl_easy_strerror(res));
    return success;
}

auto DownloadInternal(DataStruct& chunk, ProgressCallback pcallback, const std::string& url, const std::string& file, const std::string& post) -> bool {
    auto curl = curl_easy_init();
    if (!curl) {
        log_write("curl init failed\n");
        return false;
    }
    ON_SCOPE_EXIT(curl_easy_cleanup(curl));
    return DownloadInternal(curl, chunk, pcallback, url, file, post);
}

void DownloadThread(void* p) {
    auto data = static_cast<ThreadEntry*>(p);
    while (g_running) {
        auto rc = waitSingle(waiterForUEvent(&data->m_uevent), UINT64_MAX);
        // log_write("woke up\n");
        if (!g_running) {
            return;
        }
        if (R_FAILED(rc)) {
            continue;
        }

        DataStruct chunk;
        #if 1
        const auto result = DownloadInternal(data->m_curl, chunk, data->m_pcallback, data->m_url, data->m_file, data->m_post);
        if (g_running) {
            DownloadEventData event_data{data->m_callback, std::move(chunk.data), result};
            evman::push(std::move(event_data), false);
        } else {
            break;
        }
        #endif
        // mutexLock(&data->m_mutex);
        // ON_SCOPE_EXIT(mutexUnlock(&data->m_mutex));

        data->m_in_progress = false;
        // notify the queue that there's a space free
        ueventSignal(&g_thread_queue.m_uevent);
    }
    log_write("exited download thread\n");
}

void DownloadThreadQueue(void* p) {
    auto data = static_cast<ThreadQueue*>(p);
    while (g_running) {
        auto rc = waitSingle(waiterForUEvent(&data->m_uevent), UINT64_MAX);
        log_write("[thread queue] woke up\n");
        if (!g_running) {
            return;
        }
        if (R_FAILED(rc)) {
            continue;
        }

        mutexLock(&data->m_mutex);
        ON_SCOPE_EXIT(mutexUnlock(&data->m_mutex));
        if (data->m_entries.empty()) {
            continue;
        }

        // find the next avaliable thread
        u32 pop_count{};
        for (auto& entry : data->m_entries) {
            if (!g_running) {
                return;
            }

            bool keep_going{};

            for (auto& thread : g_threads) {
                if (!g_running) {
                    return;
                }

                if (!thread.InProgress()) {
                    thread.Setup(entry.callback, entry.pcallback, entry.url, entry.file, entry.post);
                    // log_write("[dl queue] starting download\n");
                    // mark entry for deletion
                    entry.m_delete = true;
                    pop_count++;
                    keep_going = true;
                    break;
                }
            }

            if (!g_running) {
                return;
            }

            if (!keep_going) {
                break;
            }
        }

        // delete all entries marked for deletion
        for (u32 i = 0; i < pop_count; i++) {
            data->m_entries.pop_front();
        }
        // if (delete_any) {
            // data->m_entries.clear();
            // data->m_entries.
            // data->m_entries.erase(std::remove_if(data->m_entries.begin(), data->m_entries.end(), [](auto& a) {
                // return a.m_delete;
            // }));
        // }
    }

    log_write("exited download thread queue\n");
}

void my_lock(CURL *handle, curl_lock_data data, curl_lock_access laccess, void *useptr) {
    mutexLock(&g_mutex_share[data]);
}

void my_unlock(CURL *handle, curl_lock_data data, void *useptr) {
    mutexUnlock(&g_mutex_share[data]);
}

} // namespace

auto DownloadInit() -> bool {
    if (CURLE_OK != curl_global_init(CURL_GLOBAL_DEFAULT)) {
        return false;
    }

    g_curl_share = curl_share_init();
    if (g_curl_share) {
        CURL_SHARE_SETOPT_LOG(g_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
        CURL_SHARE_SETOPT_LOG(g_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        CURL_SHARE_SETOPT_LOG(g_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
        CURL_SHARE_SETOPT_LOG(g_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
        CURL_SHARE_SETOPT_LOG(g_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_PSL);
        CURL_SHARE_SETOPT_LOG(g_curl_share, CURLSHOPT_LOCKFUNC, my_lock);
        CURL_SHARE_SETOPT_LOG(g_curl_share, CURLSHOPT_UNLOCKFUNC, my_unlock);
    }

    g_running = true;

    if (R_FAILED(g_thread_queue.Create())) {
        log_write("!failed to create download thread queue\n");
    }

    for (auto& entry : g_threads) {
        if (R_FAILED(entry.Create())) {
            log_write("!failed to create download thread\n");
        }
    }

    log_write("finished creating threads\n");
    return true;
}

void DownloadExit() {
    g_running = false;

    g_thread_queue.Close();

    for (auto& entry : g_threads) {
        entry.Close();
    }

    if (g_curl_share) {
        curl_share_cleanup(g_curl_share);
        g_curl_share = {};
    }

    curl_global_cleanup();
}

auto DownloadMemory(const std::string& url, const std::string& post, ProgressCallback pcallback) -> std::vector<u8> {
    if (g_url_cache.AddToCache(url)) {
        DataStruct chunk{};
        if (DownloadInternal(chunk, pcallback, url, "", post)) {
            return chunk.data;
        }
    }
    return {};
}

auto DownloadFile(const std::string& url, const std::string& out, const std::string& post, ProgressCallback pcallback) -> bool {
    if (g_url_cache.AddToCache(url)) {
        DataStruct chunk{};
        if (DownloadInternal(chunk, pcallback, url, out, post)) {
            return true;
        }
    }
    return false;
}

auto DownloadMemoryAsync(const std::string& url, const std::string& post, DownloadCallback callback, ProgressCallback pcallback, DownloadPriority prio) -> bool {
    #if USE_THREAD_QUEUE
    if (g_url_cache.AddToCache(url)) {
        return g_thread_queue.Add(prio, callback, pcallback, url, "", post);
    } else {
        return false;
    }
    #else
    // mutexLock(&g_thread_queue.m_mutex);
    // ON_SCOPE_EXIT(mutexUnlock(&g_thread_queue.m_mutex));

    for (auto& entry : g_threads) {
        if (!entry.InProgress()) {
            return entry.Setup(callback, url);
        }
    }

    log_write("failed to start download, no avaliable threads\n");
    return false;
    #endif
}

auto DownloadFileAsync(const std::string& url, const std::string& out, const std::string& post, DownloadCallback callback, ProgressCallback pcallback, DownloadPriority prio) -> bool {
    #if USE_THREAD_QUEUE
    if (g_url_cache.AddToCache(url)) {
        return g_thread_queue.Add(prio, callback, pcallback, url, out, post);
    } else {
        return false;
    }
    #else
    // mutexLock(&g_thread_queue.m_mutex);
    // ON_SCOPE_EXIT(mutexUnlock(&g_thread_queue.m_mutex));

    for (auto& entry : g_threads) {
        if (!entry.InProgress()) {
            return entry.Setup(callback, url, out);
        }
    }

    log_write("failed to start download, no avaliable threads\n");
    return false;
    #endif
}

void DownloadClearCache(const std::string& url) {
    g_url_cache.AddToCache(url);
    g_url_cache.RemoveFromCache(url);
}

} // namespace sphaira

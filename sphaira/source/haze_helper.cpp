#include "haze_helper.hpp"

#include "app.hpp"
#include "fs.hpp"
#include "log.hpp"
#include "evman.hpp"
#include "i18n.hpp"

#include <mutex>
#include <algorithm>
#include <haze.h>

namespace sphaira::haze {
namespace {

struct InstallSharedData {
    std::mutex mutex;
    std::deque<std::string> queued_files;

    void* user;
    OnInstallStart on_start;
    OnInstallWrite on_write;
    OnInstallClose on_close;

    bool in_progress;
    bool enabled;
};

constexpr int THREAD_PRIO = PRIO_PREEMPTIVE;
constexpr int THREAD_CORE = 2;
volatile bool g_should_exit = false;
bool g_is_running{false};
std::mutex g_mutex{};
InstallSharedData g_shared_data{};

const char* SUPPORTED_EXT[] = {
    ".nsp", ".xci", ".nsz", ".xcz",
};

// ive given up with good names.
void on_thing() {
    log_write("[MTP] doing on_thing\n");
    std::scoped_lock lock{g_shared_data.mutex};
    log_write("[MTP] locked on_thing\n");

    if (!g_shared_data.in_progress) {
        if (!g_shared_data.queued_files.empty()) {
            log_write("[MTP] pushing new file data\n");
            if (!g_shared_data.on_start || !g_shared_data.on_start(g_shared_data.user, g_shared_data.queued_files[0].c_str())) {
                g_shared_data.queued_files.clear();
            } else {
                log_write("[MTP] success on new file push\n");
                g_shared_data.in_progress = true;
            }
        }
    }
}

struct FsProxyBase : ::haze::FileSystemProxyImpl {
    FsProxyBase(const char* name, const char* display_name) : m_name{name}, m_display_name{display_name} {

    }

    auto FixPath(const char* path) const {
        fs::FsPath buf;
        const auto len = std::strlen(GetName());

        if (len && !strncasecmp(path + 1, GetName(), len)) {
            std::snprintf(buf, sizeof(buf), "/%s", path + 1 + len);
        } else {
            std::strcpy(buf, path);
        }

        log_write("[FixPath] %s -> %s\n", path, buf.s);
        return buf;
    }

    const char* GetName() const override {
        return m_name.c_str();
    }
    const char* GetDisplayName() const override {
        return m_display_name.c_str();
    }

protected:
    const std::string m_name;
    const std::string m_display_name;
};

struct FsProxy final : FsProxyBase {
    FsProxy(std::shared_ptr<fs::Fs> fs, const char* name, const char* display_name) : FsProxyBase{name, display_name} {
        m_fs = fs;
    }

    ~FsProxy() {
        if (m_fs->IsNative()) {
            auto fs = (fs::FsNative*)m_fs.get();
            fsFsCommit(&fs->m_fs);
        }
    }

    // TODO: impl this for stdio
    Result GetTotalSpace(const char *path, s64 *out) override {
        if (m_fs->IsNative()) {
            auto fs = (fs::FsNative*)m_fs.get();
            return fsFsGetTotalSpace(&fs->m_fs, FixPath(path), out);
        }
        *out = 1024ULL * 1024ULL * 1024ULL * 256ULL;
        R_SUCCEED();
    }
    Result GetFreeSpace(const char *path, s64 *out) override {
        if (m_fs->IsNative()) {
            auto fs = (fs::FsNative*)m_fs.get();
            return fsFsGetFreeSpace(&fs->m_fs, FixPath(path), out);
        }
        *out = 1024ULL * 1024ULL * 1024ULL * 256ULL;
        R_SUCCEED();
    }
    Result GetEntryType(const char *path, FsDirEntryType *out_entry_type) override {
        const auto rc = m_fs->GetEntryType(FixPath(path), out_entry_type);
        log_write("[HAZE] GetEntryType(%s) 0x%X\n", path, rc);
        return rc;
    }
    Result CreateFile(const char* path, s64 size, u32 option) override {
        log_write("[HAZE] CreateFile(%s)\n", path);
        return m_fs->CreateFile(FixPath(path), size, option);
    }
    Result DeleteFile(const char* path) override {
        log_write("[HAZE] DeleteFile(%s)\n", path);
        return m_fs->DeleteFile(FixPath(path));
    }
    Result RenameFile(const char *old_path, const char *new_path) override {
        log_write("[HAZE] RenameFile(%s -> %s)\n", old_path, new_path);
        return m_fs->RenameFile(FixPath(old_path), FixPath(new_path));
    }
    Result OpenFile(const char *path, u32 mode, FsFile *out_file) override {
        log_write("[HAZE] OpenFile(%s)\n", path);
        auto fptr = new fs::File();
        const auto rc = m_fs->OpenFile(FixPath(path), mode, fptr);

        if (R_SUCCEEDED(rc)) {
            std::memcpy(&out_file->s, &fptr, sizeof(fptr));
        } else {
            delete fptr;
        }

        return rc;
    }
    Result GetFileSize(FsFile *file, s64 *out_size) override {
        log_write("[HAZE] GetFileSize()\n");
        fs::File* f;
        std::memcpy(&f, &file->s, sizeof(f));
        return f->GetSize(out_size);
    }
    Result SetFileSize(FsFile *file, s64 size) override {
        log_write("[HAZE] SetFileSize()\n");
        fs::File* f;
        std::memcpy(&f, &file->s, sizeof(f));
        return f->SetSize(size);
    }
    Result ReadFile(FsFile *file, s64 off, void *buf, u64 read_size, u32 option, u64 *out_bytes_read) override {
        log_write("[HAZE] ReadFile()\n");
        fs::File* f;
        std::memcpy(&f, &file->s, sizeof(f));
        return f->Read(off, buf, read_size, option, out_bytes_read);
    }
    Result WriteFile(FsFile *file, s64 off, const void *buf, u64 write_size, u32 option) override {
        log_write("[HAZE] WriteFile()\n");
        fs::File* f;
        std::memcpy(&f, &file->s, sizeof(f));
        return f->Write(off, buf, write_size, option);
    }
    void CloseFile(FsFile *file) override {
        log_write("[HAZE] CloseFile()\n");
        fs::File* f;
        std::memcpy(&f, &file->s, sizeof(f));
        if (f) {
            delete f;
        }
        std::memset(file, 0, sizeof(*file));
    }

    Result CreateDirectory(const char* path) override {
        log_write("[HAZE] DeleteFile(%s)\n", path);
        return m_fs->CreateDirectory(FixPath(path));
    }
    Result DeleteDirectoryRecursively(const char* path) override {
        log_write("[HAZE] DeleteDirectoryRecursively(%s)\n", path);
        return m_fs->DeleteDirectoryRecursively(FixPath(path));
    }
    Result RenameDirectory(const char *old_path, const char *new_path) override {
        log_write("[HAZE] RenameDirectory(%s -> %s)\n", old_path, new_path);
        return m_fs->RenameDirectory(FixPath(old_path), FixPath(new_path));
    }
    Result OpenDirectory(const char *path, u32 mode, FsDir *out_dir) override {
        auto fptr = new fs::Dir();
        const auto rc = m_fs->OpenDirectory(FixPath(path), mode, fptr);

        if (R_SUCCEEDED(rc)) {
            std::memcpy(&out_dir->s, &fptr, sizeof(fptr));
        } else {
            delete fptr;
        }

        log_write("[HAZE] OpenDirectory(%s) 0x%X\n", path, rc);
        return rc;
    }
    Result ReadDirectory(FsDir *d, s64 *out_total_entries, size_t max_entries, FsDirectoryEntry *buf) override {
        fs::Dir* f;
        std::memcpy(&f, &d->s, sizeof(f));
        const auto rc = f->Read(out_total_entries, max_entries, buf);
        log_write("[HAZE] ReadDirectory(%zd) 0x%X\n", *out_total_entries, rc);
        return rc;
    }
    Result GetDirectoryEntryCount(FsDir *d, s64 *out_count) override {
        fs::Dir* f;
        std::memcpy(&f, &d->s, sizeof(f));
        const auto rc = f->GetEntryCount(out_count);
        log_write("[HAZE] GetDirectoryEntryCount(%zd) 0x%X\n", *out_count, rc);
        return rc;
    }
    void CloseDirectory(FsDir *d) override {
        log_write("[HAZE] CloseDirectory()\n");
        fs::Dir* f;
        std::memcpy(&f, &d->s, sizeof(f));
        if (f) {
            delete f;
        }
        std::memset(d, 0, sizeof(*d));
    }

private:
    std::shared_ptr<fs::Fs> m_fs{};
};

struct FsDevNullProxy final : FsProxyBase {
    using FsProxyBase::FsProxyBase;

    Result GetTotalSpace(const char *path, s64 *out) override {
        *out = 1024ULL * 1024ULL * 1024ULL * 256ULL;
        R_SUCCEED();
    }
    Result GetFreeSpace(const char *path, s64 *out) override {
        *out = 1024ULL * 1024ULL * 1024ULL * 256ULL;
        R_SUCCEED();
    }
    Result GetEntryType(const char *path, FsDirEntryType *out_entry_type) override {
        if (FixPath(path) == "/") {
            *out_entry_type = FsDirEntryType_Dir;
            R_SUCCEED();
        } else {
            *out_entry_type = FsDirEntryType_File;
            R_SUCCEED();
        }
    }
    Result CreateFile(const char* path, s64 size, u32 option) override {
        R_SUCCEED();
    }
    Result DeleteFile(const char* path) override {
        R_SUCCEED();
    }
    Result RenameFile(const char *old_path, const char *new_path) override {
        R_SUCCEED();
    }
    Result OpenFile(const char *path, u32 mode, FsFile *out_file) override {
        R_SUCCEED();
    }
    Result GetFileSize(FsFile *file, s64 *out_size) override {
        *out_size = 0;
        R_SUCCEED();
    }
    Result SetFileSize(FsFile *file, s64 size) override {
        R_SUCCEED();
    }
    Result ReadFile(FsFile *file, s64 off, void *buf, u64 read_size, u32 option, u64 *out_bytes_read) override {
        *out_bytes_read = 0;
        R_SUCCEED();
    }
    Result WriteFile(FsFile *file, s64 off, const void *buf, u64 write_size, u32 option) override {
        R_SUCCEED();
    }
    void CloseFile(FsFile *file) override {
        std::memset(file, 0, sizeof(*file));
    }

    Result CreateDirectory(const char* path) override {
        R_SUCCEED();
    }
    Result DeleteDirectoryRecursively(const char* path) override {
        R_SUCCEED();
    }
    Result RenameDirectory(const char *old_path, const char *new_path) override {
        R_SUCCEED();
    }
    Result OpenDirectory(const char *path, u32 mode, FsDir *out_dir) override {
        R_SUCCEED();
    }
    Result ReadDirectory(FsDir *d, s64 *out_total_entries, size_t max_entries, FsDirectoryEntry *buf) override {
        *out_total_entries = 0;
        R_SUCCEED();
    }
    Result GetDirectoryEntryCount(FsDir *d, s64 *out_count) override {
        *out_count = 0;
        R_SUCCEED();
    }
    void CloseDirectory(FsDir *d) override {
        std::memset(d, 0, sizeof(*d));
    }
};

struct FsInstallProxy final : FsProxyBase {
    using FsProxyBase::FsProxyBase;

    Result FailedIfNotEnabled() {
        std::scoped_lock lock{g_shared_data.mutex};
        if (!g_shared_data.enabled) {
            App::Notify("Please launch MTP install menu before trying to install"_i18n);
            R_THROW(0x1);
        }
        R_SUCCEED();
    }

    // TODO: impl this.
    Result GetTotalSpace(const char *path, s64 *out) override {
        if (App::GetApp()->m_install_sd.Get()) {
            return fs::FsNativeContentStorage(FsContentStorageId_SdCard).GetTotalSpace("/", out);
        } else {
            return fs::FsNativeContentStorage(FsContentStorageId_User).GetTotalSpace("/", out);
        }
    }
    Result GetFreeSpace(const char *path, s64 *out) override {
        if (App::GetApp()->m_install_sd.Get()) {
            return fs::FsNativeContentStorage(FsContentStorageId_SdCard).GetFreeSpace("/", out);
        } else {
            return fs::FsNativeContentStorage(FsContentStorageId_User).GetFreeSpace("/", out);
        }
    }
    Result GetEntryType(const char *path, FsDirEntryType *out_entry_type) override {
        if (FixPath(path) == "/") {
            *out_entry_type = FsDirEntryType_Dir;
            R_SUCCEED();
        } else {
            *out_entry_type = FsDirEntryType_File;
            R_SUCCEED();
        }
    }
    Result CreateFile(const char* path, s64 size, u32 option) override {
        return FailedIfNotEnabled();
    }
    Result DeleteFile(const char* path) override {
        R_SUCCEED();
    }
    Result RenameFile(const char *old_path, const char *new_path) override {
        R_SUCCEED();
    }
    Result OpenFile(const char *path, u32 mode, FsFile *out_file) override {
        if (mode & FsOpenMode_Read) {
            R_SUCCEED();
        } else {
            std::scoped_lock lock{g_shared_data.mutex};
            if (!g_shared_data.enabled) {
                R_THROW(0x1);
            }

            const char* ext = std::strrchr(path, '.');
            if (!ext) {
                R_THROW(0x1);
            }

            bool found = false;
            for (size_t i = 0; i < std::size(SUPPORTED_EXT); i++) {
                if (!strcasecmp(ext, SUPPORTED_EXT[i])) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                R_THROW(0x1);
            }

            // check if we already have this file queued.
            auto it = std::ranges::find(g_shared_data.queued_files, path);
            if (it != g_shared_data.queued_files.cend()) {
                R_THROW(0x1);
            }

            g_shared_data.queued_files.push_back(path);
        }

        on_thing();
        log_write("[MTP] got file: %s\n", path);
        R_SUCCEED();
    }
    Result GetFileSize(FsFile *file, s64 *out_size) override {
        *out_size = 0;
        R_SUCCEED();
    }
    Result SetFileSize(FsFile *file, s64 size) override {
        R_SUCCEED();
    }
    Result ReadFile(FsFile *file, s64 off, void *buf, u64 read_size, u32 option, u64 *out_bytes_read) override {
        *out_bytes_read = 0;
        R_SUCCEED();
    }
    Result WriteFile(FsFile *file, s64 off, const void *buf, u64 write_size, u32 option) override {
        std::scoped_lock lock{g_shared_data.mutex};
        if (!g_shared_data.enabled) {
            R_THROW(0x1);
        }

        if (!g_shared_data.on_write || !g_shared_data.on_write(g_shared_data.user, buf, write_size)) {
            R_THROW(0x1);
        }

        R_SUCCEED();
    }
    void CloseFile(FsFile *file) override {
        {
            log_write("[MTP] closing file\n");
            std::scoped_lock lock{g_shared_data.mutex};
            log_write("[MTP] closing valid file\n");

            log_write("[MTP] closing current file\n");
            if (g_shared_data.on_close) {
                g_shared_data.on_close(g_shared_data.user);
            }

            g_shared_data.in_progress = false;
            g_shared_data.queued_files.clear();
        }

        on_thing();
        std::memset(file, 0, sizeof(*file));
    }

    Result CreateDirectory(const char* path) override {
        R_SUCCEED();
    }
    Result DeleteDirectoryRecursively(const char* path) override {
        R_SUCCEED();
    }
    Result RenameDirectory(const char *old_path, const char *new_path) override {
        R_SUCCEED();
    }
    Result OpenDirectory(const char *path, u32 mode, FsDir *out_dir) override {
        R_SUCCEED();
    }
    Result ReadDirectory(FsDir *d, s64 *out_total_entries, size_t max_entries, FsDirectoryEntry *buf) override {
        *out_total_entries = 0;
        R_SUCCEED();
    }
    Result GetDirectoryEntryCount(FsDir *d, s64 *out_count) override {
        *out_count = 0;
        R_SUCCEED();
    }
    void CloseDirectory(FsDir *d) override {
        std::memset(d, 0, sizeof(*d));
    }
};

::haze::FsEntries g_fs_entries{};

void haze_callback(const ::haze::CallbackData *data) {
    App::NotifyFlashLed();
}

} // namespace

bool Init() {
    std::scoped_lock lock{g_mutex};
    if (g_is_running) {
        log_write("[MTP] already enabled, cannot open\n");
        return false;
    }

    g_fs_entries.emplace_back(std::make_shared<FsProxy>(std::make_shared<fs::FsNativeSd>(), "", "microSD card"));
    g_fs_entries.emplace_back(std::make_shared<FsProxy>(std::make_shared<fs::FsNativeImage>(FsImageDirectoryId_Nand), "image_nand", "Image nand"));
    g_fs_entries.emplace_back(std::make_shared<FsProxy>(std::make_shared<fs::FsNativeImage>(FsImageDirectoryId_Sd), "image_sd", "Image sd"));
    g_fs_entries.emplace_back(std::make_shared<FsDevNullProxy>("DevNull", "DevNull (Speed Test)"));
    g_fs_entries.emplace_back(std::make_shared<FsInstallProxy>("install", "Install (NSP, XCI, NSZ, XCZ)"));

    g_should_exit = false;
    if (!::haze::Initialize(haze_callback, PRIO_PREEMPTIVE, 2, g_fs_entries)) {
        return false;
    }

    log_write("[MTP] started\n");
    return g_is_running = true;
}

void Exit() {
    std::scoped_lock lock{g_mutex};
    if (!g_is_running) {
        return;
    }

    ::haze::Exit();
    g_is_running = false;
    g_should_exit = true;
    g_fs_entries.clear();

    log_write("[MTP] exitied\n");
}

void InitInstallMode(void* user, OnInstallStart on_start, OnInstallWrite on_write, OnInstallClose on_close) {
    std::scoped_lock lock{g_shared_data.mutex};
    g_shared_data.user = user;
    g_shared_data.on_start = on_start;
    g_shared_data.on_write = on_write;
    g_shared_data.on_close = on_close;
    g_shared_data.enabled = true;
}

void DisableInstallMode() {
    std::scoped_lock lock{g_shared_data.mutex};
    g_shared_data.enabled = false;
}

} // namespace sphaira::haze

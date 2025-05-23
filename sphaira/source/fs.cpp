#include "fs.hpp"
#include "defines.hpp"
#include "ui/nvg_util.hpp"
#include "log.hpp"

#include <switch.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string_view>
#include <algorithm>
#include <ranges>

#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <ftw.h>

namespace fs {
namespace {

// these folders and internals cannot be modified
constexpr std::string_view READONLY_ROOT_FOLDERS[]{
    "/atmosphere/automatic_backups",

    "/bootloader/res",
    "/bootloader/sys",

    "/backup", // some people never back this up...

    "/Nintendo", // Nintendo private folder
    "/Nintendo/Contents",
    "/Nintendo/save",

    "/emuMMC", // emunand
    "/warmboot_mariko",
};

// these files and folders cannot be modified
constexpr std::string_view READONLY_FILES[]{
    "/", // don't allow deleting root

    "/atmosphere", // don't allow deleting all of /atmosphere
    "/atmosphere/hbl.nsp",
    "/atmosphere/package3",
    "/atmosphere/reboot_payload.bin",
    "/atmosphere/stratosphere.romfs",

    "/bootloader", // don't allow deleting all of /bootloader
    "/bootloader/hekate_ipl.ini",

    "/switch", // don't allow deleting all of /switch
    "/hbmenu.nro", // breaks hbl
    "/payload.bin", // some modchips need this

    "/boot.dat", // sxos
    "/license.dat", // sxos

    "/switch/prod.keys",
    "/switch/title.keys",
    "/switch/reboot_to_payload.nro",
};

bool is_read_only_root(std::string_view path) {
    for (auto p : READONLY_ROOT_FOLDERS) {
        if (path.starts_with(p)) {
            return true;
        }
    }

    return false;
}

bool is_read_only_file(std::string_view path) {
    for (auto p : READONLY_FILES) {
        if (path == p) {
            return true;
        }
    }

    return false;
}

bool is_read_only(std::string_view path) {
    if (is_read_only_root(path)) {
        return true;
    }
    if (is_read_only_file(path)) {
        return true;
    }
    return false;
}

} // namespace

FsPath AppendPath(const FsPath& root_path, const FsPath& file_path) {
    FsPath path;
    if (root_path[std::strlen(root_path) - 1] != '/') {
        std::snprintf(path, sizeof(path), "%s/%s", root_path.s, file_path.s);
    } else {
        std::snprintf(path, sizeof(path), "%s%s", root_path.s, file_path.s);
    }
    return path;
}

Result CreateFile(FsFileSystem* fs, const FsPath& path, u64 size, u32 option, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only_root(path), Fs::ResultReadOnly);

    return fsFsCreateFile(fs, path, size, option);
}

Result CreateDirectory(FsFileSystem* fs, const FsPath& path, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only_root(path), Fs::ResultReadOnly);

    return fsFsCreateDirectory(fs, path);
}

Result CreateDirectoryRecursively(FsFileSystem* fs, const FsPath& _path, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only_root(_path), Fs::ResultReadOnly);

    auto path_view = std::string_view{_path};
    // todo: fix this for sdmc: and ums0:
    FsPath path{"/"};
    if (auto s = std::strchr(_path.s, ':')) {
        const int len = (s - _path.s) + 1;
        std::snprintf(path, sizeof(path), "%.*s/", len, _path.s);
        path_view = path_view.substr(len);
    }

    for (const auto dir : std::views::split(path_view, '/')) {
        if (dir.empty()) {
            continue;
        }
        std::strncat(path, dir.data(), dir.size());
        log_write("[FS] dir creation path is now: %s\n", path.s);

        Result rc;
        if (fs) {
            rc = CreateDirectory(fs, path, ignore_read_only);
        } else {
            rc = CreateDirectory(path, ignore_read_only);
        }

        if (R_FAILED(rc) && rc != FsError_PathAlreadyExists) {
            log_write("failed to create folder: %s\n", path.s);
            return rc;
        }

        // log_write("created_directory: %s\n", path);
        std::strcat(path, "/");
    }
    R_SUCCEED();
}

Result CreateDirectoryRecursivelyWithPath(FsFileSystem* fs, const FsPath& _path, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only_root(_path), Fs::ResultReadOnly);

    size_t off = 0;
    while (true) {
        const auto first = std::strchr(_path + off, '/');
        if (!first) {
            R_SUCCEED();
        }

        off = (first - _path.s) + 1;
        FsPath path;
        std::strncpy(path, _path, off);

        Result rc;
        if (fs) {
            rc = CreateDirectory(fs, path, ignore_read_only);
        } else {
            rc = CreateDirectory(path, ignore_read_only);
        }

        if (R_FAILED(rc) && rc != FsError_PathAlreadyExists) {
            log_write("failed to create folder recursively: %s\n", path.s);
            return rc;
        }

        // log_write("created_directory recursively: %s\n", path);
    }
}

Result DeleteFile(FsFileSystem* fs, const FsPath& path, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only(path), Fs::ResultReadOnly);
    return fsFsDeleteFile(fs, path);
}

Result DeleteDirectory(FsFileSystem* fs, const FsPath& path, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only(path), Fs::ResultReadOnly);

    return fsFsDeleteDirectory(fs, path);
}

Result DeleteDirectoryRecursively(FsFileSystem* fs, const FsPath& path, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only(path), Fs::ResultReadOnly);

    return fsFsDeleteDirectoryRecursively(fs, path);
}

Result RenameFile(FsFileSystem* fs, const FsPath& src, const FsPath& dst, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only(src), Fs::ResultReadOnly);
    R_UNLESS(ignore_read_only || !is_read_only(dst), Fs::ResultReadOnly);

    return fsFsRenameFile(fs, src, dst);
}

Result RenameDirectory(FsFileSystem* fs, const FsPath& src, const FsPath& dst, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only(src), Fs::ResultReadOnly);
    R_UNLESS(ignore_read_only || !is_read_only(dst), Fs::ResultReadOnly);

    return fsFsRenameDirectory(fs, src, dst);
}

Result GetEntryType(FsFileSystem* fs, const FsPath& path, FsDirEntryType* out) {
    return fsFsGetEntryType(fs, path, out);
}

Result GetFileTimeStampRaw(FsFileSystem* fs, const FsPath& path, FsTimeStampRaw *out) {
    return fsFsGetFileTimeStampRaw(fs, path, out);
}

bool FileExists(FsFileSystem* fs, const FsPath& path) {
    FsDirEntryType type;
    R_TRY_RESULT(GetEntryType(fs, path, &type), false);
    return type == FsDirEntryType_File;
}

bool DirExists(FsFileSystem* fs, const FsPath& path) {
    FsDirEntryType type;
    R_TRY_RESULT(GetEntryType(fs, path, &type), false);
    return type == FsDirEntryType_Dir;
}

Result read_entire_file(FsFileSystem* _fs, const FsPath& path, std::vector<u8>& out) {
    FsNative fs{_fs, false};
    R_TRY(fs.GetFsOpenResult());

    FsFile f;
    R_TRY(fs.OpenFile(path, FsOpenMode_Read, &f));
    ON_SCOPE_EXIT(fsFileClose(&f));

    s64 size;
    R_TRY(fsFileGetSize(&f, &size));
    out.resize(size);

    u64 bytes_read;
    R_TRY(fsFileRead(&f, 0, out.data(), out.size(), FsReadOption_None, &bytes_read));
    R_UNLESS(bytes_read == out.size(), 1);

    R_SUCCEED();
}

Result write_entire_file(FsFileSystem* _fs, const FsPath& path, const std::vector<u8>& in, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only(path), Fs::ResultReadOnly);

    FsNative fs{_fs, false, ignore_read_only};
    R_TRY(fs.GetFsOpenResult());

    if (auto rc = fs.CreateFile(path, in.size(), 0); R_FAILED(rc) && rc != FsError_PathAlreadyExists) {
        return rc;
    }

    FsFile f;
    R_TRY(fs.OpenFile(path, FsOpenMode_Write, &f));
    ON_SCOPE_EXIT(fsFileClose(&f));

    R_TRY(fsFileSetSize(&f, in.size()));
    R_TRY(fsFileWrite(&f, 0, in.data(), in.size(), FsWriteOption_None));

    R_SUCCEED();
}

Result copy_entire_file(FsFileSystem* fs, const FsPath& dst, const FsPath& src, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only(dst), Fs::ResultReadOnly);

    std::vector<u8> data;
    R_TRY(read_entire_file(fs, src, data));
    return write_entire_file(fs, dst, data, ignore_read_only);
}

Result CreateFile(const FsPath& path, u64 size, u32 option, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only_root(path), Fs::ResultReadOnly);

    auto fd = open(path, O_WRONLY | O_CREAT, DEFFILEMODE);
    if (fd == -1) {
        if (errno == EEXIST) {
            return FsError_PathAlreadyExists;
        }

        R_TRY(fsdevGetLastResult());
        return Fs::ResultUnknownStdioError;
    }
    close(fd);
    R_SUCCEED();
}

Result CreateDirectory(const FsPath& path, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only_root(path), Fs::ResultReadOnly);

    if (mkdir(path, ACCESSPERMS)) {
        if (errno == EEXIST) {
            return FsError_PathAlreadyExists;
        }

        R_TRY(fsdevGetLastResult());
        return Fs::ResultUnknownStdioError;
    }
    R_SUCCEED();
}

Result CreateDirectoryRecursively(const FsPath& path, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only_root(path), Fs::ResultReadOnly);

    return CreateDirectoryRecursively(nullptr, path, ignore_read_only);
}

Result CreateDirectoryRecursivelyWithPath(const FsPath& path, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only_root(path), Fs::ResultReadOnly);

    return CreateDirectoryRecursivelyWithPath(nullptr, path, ignore_read_only);
}

Result DeleteFile(const FsPath& path, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only(path), Fs::ResultReadOnly);

    if (remove(path)) {
        R_TRY(fsdevGetLastResult());
        return Fs::ResultUnknownStdioError;
    }
    R_SUCCEED();
}

Result DeleteDirectory(const FsPath& path, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only(path), Fs::ResultReadOnly);

    return DeleteFile(path, ignore_read_only);
}

// ftw / ntfw isn't found by linker...
Result DeleteDirectoryRecursively(const FsPath& path, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only(path), Fs::ResultReadOnly);

    #if 0
    // const auto unlink_cb = [](const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) -> int {
    const auto unlink_cb = [](const char *fpath, const struct stat *sb, int typeflag) -> int {
        return remove(fpath);
    };
    // todo: check for reasonable max fd limit
    // if (nftw(path, unlink_cb, 16, FTW_DEPTH)) {
    if (ftw(path, unlink_cb, 16)) {
        R_TRY(fsdevGetLastResult());
        return Fs::ResultUnknownStdioError;
    }
    R_SUCCEED();
    #else
    R_THROW(0xFFFF);
    #endif
}

Result RenameFile(const FsPath& src, const FsPath& dst, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only(src), Fs::ResultReadOnly);
    R_UNLESS(ignore_read_only || !is_read_only(dst), Fs::ResultReadOnly);

    if (rename(src, dst)) {
        R_TRY(fsdevGetLastResult());
        return Fs::ResultUnknownStdioError;
    }
    R_SUCCEED();
}

Result RenameDirectory(const FsPath& src, const FsPath& dst, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only(src), Fs::ResultReadOnly);
    R_UNLESS(ignore_read_only || !is_read_only(dst), Fs::ResultReadOnly);

    return RenameFile(src, dst, ignore_read_only);
}

Result GetEntryType(const FsPath& path, FsDirEntryType* out) {
    struct stat st;
    if (stat(path, &st)) {
        R_TRY(fsdevGetLastResult());
        return Fs::ResultUnknownStdioError;
    }
    *out = S_ISREG(st.st_mode) ? FsDirEntryType_File : FsDirEntryType_Dir;
    R_SUCCEED();
}

Result GetFileTimeStampRaw(const FsPath& path, FsTimeStampRaw *out) {
    struct stat st;
    if (stat(path, &st)) {
        R_TRY(fsdevGetLastResult());
        return Fs::ResultUnknownStdioError;
    }

    out->is_valid = true;
    out->created = st.st_ctim.tv_sec;
    out->modified = st.st_mtim.tv_sec;
    out->accessed = st.st_atim.tv_sec;
    R_SUCCEED();
}

bool FileExists(const FsPath& path) {
    FsDirEntryType type;
    R_TRY_RESULT(GetEntryType(path, &type), false);
    return type == FsDirEntryType_File;
}

bool DirExists(const FsPath& path) {
    FsDirEntryType type;
    R_TRY_RESULT(GetEntryType(path, &type), false);
    return type == FsDirEntryType_Dir;
}

Result read_entire_file(const FsPath& path, std::vector<u8>& out) {
    auto f = std::fopen(path, "rb");
    if (!f) {
        R_TRY(fsdevGetLastResult());
        return Fs::ResultUnknownStdioError;
    }
    ON_SCOPE_EXIT(std::fclose(f));

    std::fseek(f, 0, SEEK_END);
    const auto size = std::ftell(f);
    std::rewind(f);

    out.resize(size);

    std::fread(out.data(), 1, out.size(), f);
    R_SUCCEED();
}

Result write_entire_file(const FsPath& path, const std::vector<u8>& in, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only(path), Fs::ResultReadOnly);

    auto f = std::fopen(path, "wb");
    if (!f) {
        R_TRY(fsdevGetLastResult());
        return Fs::ResultUnknownStdioError;
    }
    ON_SCOPE_EXIT(std::fclose(f));

    std::fwrite(in.data(), 1, in.size(), f);
    R_SUCCEED();
}

Result copy_entire_file(const FsPath& dst, const FsPath& src, bool ignore_read_only) {
    R_UNLESS(ignore_read_only || !is_read_only(dst), Fs::ResultReadOnly);

    std::vector<u8> data;
    R_TRY(read_entire_file(src, data));
    return write_entire_file(dst, data, ignore_read_only);
}

Result OpenFile(fs::Fs* fs, const fs::FsPath& path, u32 mode, File* f) {
    *f = {};
    f->m_fs = fs;

    if (f->m_fs->IsNative()) {
        auto fs = (fs::FsNative*)f->m_fs;
        R_TRY(fs->OpenFile(path, mode, &f->m_native));
    } else {
        if ((mode & FsOpenMode_Read) && (mode & FsOpenMode_Write)) {
            // todo:
            R_THROW(0x1);
        } else if (mode & FsOpenMode_Read) {
            f->m_stdio = fopen(path, "rb");
        } else if (mode & FsOpenMode_Write) {
            f->m_stdio = fopen(path, "wb");
        }

        R_UNLESS(f->m_stdio, 0x1);
    }

    std::strcpy(f->m_path, path);
    R_SUCCEED();
}

Result FileRead(File* f, s64 off, void* buf, u64 read_size, u32 option, u64* bytes_read) {
    if (f->m_fs->IsNative()) {
        R_TRY(fsFileRead(&f->m_native, off, buf, read_size, option, bytes_read));
    } else {
        if (f->m_stdio_off != off) {
            f->m_stdio_off = off;
            std::fseek(f->m_stdio, off, SEEK_SET);
        }

        *bytes_read = std::fread(buf, 1, read_size, f->m_stdio);
        f->m_stdio_off += *bytes_read;
    }

    R_SUCCEED();
}

Result FileWrite(File* f, s64 off, const void* buf, u64 write_size, u32 option) {
    if (f->m_fs->IsNative()) {
        R_TRY(fsFileWrite(&f->m_native, off, buf, write_size, option));
    } else {
        if (f->m_stdio_off != off) {
            log_write("[FS] diff seek\n");
            f->m_stdio_off = off;
            std::fseek(f->m_stdio, off, SEEK_SET);
        }

        const auto result = std::fwrite(buf, 1, write_size, f->m_stdio);
        // log_write("[FS] fwrite res: %zu vs %zu\n", result, write_size);
        R_UNLESS(result == write_size, 0x1);

        f->m_stdio_off += write_size;
    }

    R_SUCCEED();
}

Result FileSetSize(File* f, s64 sz) {
    if (f->m_fs->IsNative()) {
        R_TRY(fsFileSetSize(&f->m_native, sz));
    } else {
        R_SUCCEED();
        // const auto fd = fileno(f->m_stdio);
        // R_UNLESS(fd > 0, 0x1);
        // R_UNLESS(!ftruncate(fd, sz), 0x1);
    }

    R_SUCCEED();
}

Result FileGetSize(File* f, s64* out) {
    if (f->m_fs->IsNative()) {
        R_TRY(fsFileGetSize(&f->m_native, out));
    } else {
        struct stat st;
        const auto fd = fileno(f->m_stdio);
        bool did_stat{};

        if (fd && !fstat(fd, &st)) {
            did_stat = true;
        }

        if (!did_stat) {
            R_UNLESS(!lstat(f->m_path, &st), 0x1);
        }

        *out = st.st_size;
    }

    R_SUCCEED();
}

void FileClose(File* f) {
    if (f->m_fs->IsNative()) {
        fsFileClose(&f->m_native);
    } else {
        std::fclose(f->m_stdio);
    }

    *f = {};
}

Result OpenDirectory(fs::Fs* fs, const fs::FsPath& path, u32 mode, Dir* d) {
    *d = {};
    d->m_fs = fs;
    d->m_mode = mode;

    if (d->m_fs->IsNative()) {
        auto fs = (fs::FsNative*)d->m_fs;
        R_TRY(fs->OpenDirectory(path, mode, &d->m_native));
    } else {
        d->m_stdio = opendir(path);
        R_UNLESS(d->m_stdio, 0x1);
    }

    R_SUCCEED();
}

Result DirReadAll(Dir* d, std::vector<FsDirectoryEntry>& buf) {
    buf.clear();

    if (d->m_fs->IsNative()) {
        auto fs = (fs::FsNative*)d->m_fs;

        s64 count;
        R_TRY(fs->DirGetEntryCount(&d->m_native, &count));

        buf.resize(count);
        R_TRY(fs->DirRead(&d->m_native, &count, buf.size(), buf.data()));
        buf.resize(count);
    } else {
        buf.reserve(1000);

        struct dirent* dirent;
        while ((dirent = readdir(d->m_stdio))) {
            if (!std::strcmp(dirent->d_name, ".") || !std::strcmp(dirent->d_name, "..")) {
                continue;
            }

            FsDirectoryEntry entry{};

            if (dirent->d_type == DT_DIR) {
                if (!(d->m_mode & FsDirOpenMode_ReadDirs)) {
                    continue;
                }
                entry.type = FsDirEntryType_Dir;
            } else if (dirent->d_type == DT_REG) {
                if (!(d->m_mode & FsDirOpenMode_ReadFiles)) {
                    continue;
                }
                entry.type = FsDirEntryType_File;
            }

            std::strcpy(entry.name, dirent->d_name);
            buf.emplace_back(entry);
        }
    }

    R_SUCCEED();
}

void DirClose(Dir* d) {
    if (d->m_fs->IsNative()) {
        fsDirClose(&d->m_native);
    } else {
        closedir(d->m_stdio);
    }

    *d = {};
}

Result FileGetSizeAndTimestamp(fs::Fs* m_fs, const FsPath& path, FsTimeStampRaw* ts, s64* size) {
    *ts = {};
    *size = {};

    if (m_fs->IsNative()) {
        auto fs = (fs::FsNative*)m_fs;
        R_TRY(fs->GetFileTimeStampRaw(path, ts));

        File f;
        R_TRY(m_fs->OpenFile(path, FsOpenMode_Read, &f));
        ON_SCOPE_EXIT(fs->FileClose(&f));

        R_TRY(m_fs->FileGetSize(&f, size));
    } else {
        struct stat st;
        R_UNLESS(!lstat(path, &st), 0x1);

        ts->is_valid = true;
        ts->created = st.st_ctim.tv_sec;
        ts->modified = st.st_mtim.tv_sec;
        ts->accessed = st.st_atim.tv_sec;
        *size = st.st_size;
    }

    R_SUCCEED();
}

Result IsDirEmpty(fs::Fs* m_fs, const fs::FsPath& path, bool* out) {
    *out = true;

    if (m_fs->IsNative()) {
        auto fs = (fs::FsNative*)m_fs;

        s64 count;
        R_TRY(fs->DirGetEntryCount(path, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles | FsDirOpenMode_NoFileSize, &count));
        *out = !count;
    } else {
        auto dir = opendir(path);
        R_UNLESS(dir, 0x1);
        ON_SCOPE_EXIT(closedir(dir));

        struct dirent* dirent;
        while ((dirent = readdir(dir))) {
            if (!std::strcmp(dirent->d_name, ".") || !std::strcmp(dirent->d_name, "..")) {
                continue;
            }

            *out = false;
            break;
        }
    }

    R_SUCCEED();
}

} // namespace fs

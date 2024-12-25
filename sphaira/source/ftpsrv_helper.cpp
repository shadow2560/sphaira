#include "ftpsrv_helper.hpp"
#include <ftpsrv.h>
#include <ftpsrv_vfs.h>
#include "app.hpp"
#include "fs.hpp"
#include "log.hpp"

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <mutex>
#include <algorithm>

namespace {

FtpSrvConfig g_ftpsrv_config = {0};
volatile bool g_should_exit = false;
bool g_is_running{false};
Thread g_thread;
std::mutex g_mutex{};
FsFileSystem* g_fs;

void ftp_log_callback(enum FTP_API_LOG_TYPE type, const char* msg) {
    sphaira::App::NotifyFlashLed();
}

void ftp_progress_callback(void) {
    sphaira::App::NotifyFlashLed();
}

int vfs_fs_set_errno(Result rc) {
    switch (rc) {
        case FsError_TargetLocked: errno = EBUSY; break;
        case FsError_PathNotFound: errno = ENOENT; break;
        case FsError_PathAlreadyExists: errno = EEXIST; break;
        case FsError_UsableSpaceNotEnoughMmcCalibration: errno = ENOSPC; break;
        case FsError_UsableSpaceNotEnoughMmcSafe: errno = ENOSPC; break;
        case FsError_UsableSpaceNotEnoughMmcUser: errno = ENOSPC; break;
        case FsError_UsableSpaceNotEnoughMmcSystem: errno = ENOSPC; break;
        case FsError_UsableSpaceNotEnoughSdCard: errno = ENOSPC; break;
        case FsError_OutOfRange: errno = ESPIPE; break;
        case FsError_TooLongPath: errno = ENAMETOOLONG; break;
        case FsError_UnsupportedWriteForReadOnlyFileSystem: errno = EROFS; break;
        default: errno = EIO; break;
    }
    return -1;
}

Result flush_buffered_write(struct FtpVfsFile* f) {
    Result rc;
    if (R_SUCCEEDED(rc = fsFileSetSize(&f->fd, f->off + f->buf_off))) {
        rc = fsFileWrite(&f->fd, f->off, f->buf, f->buf_off, FsWriteOption_None);
    }
    return rc;
}

void loop(void* arg) {
    while (!g_should_exit) {
        ftpsrv_init(&g_ftpsrv_config);
        while (!g_should_exit) {
            if (ftpsrv_loop(100) != FTP_API_LOOP_ERROR_OK) {
                svcSleepThread(1e+6);
                break;
            }
        }
        ftpsrv_exit();
    }
}

} // namespace

namespace sphaira::ftpsrv {

bool Init() {
    std::scoped_lock lock{g_mutex};
    if (g_is_running) {
        return false;
    }

    g_fs = fsdevGetDeviceFileSystem("sdmc");

    g_ftpsrv_config.log_callback = ftp_log_callback;
    g_ftpsrv_config.progress_callback = ftp_progress_callback;
    g_ftpsrv_config.anon = true;
    g_ftpsrv_config.timeout = 15;
    g_ftpsrv_config.port = 5000;

    Result rc;
    if (R_FAILED(rc = threadCreate(&g_thread, loop, nullptr, nullptr, 1024*64, 0x2C, 2))) {
        log_write("failed to create nxlink thread: 0x%X\n", rc);
        return false;
    }

    if (R_FAILED(rc = threadStart(&g_thread))) {
        log_write("failed to start nxlink thread: 0x%X\n", rc);
        threadClose(&g_thread);
        return false;
    }

    return g_is_running = true;
}

void Exit() {
    std::scoped_lock lock{g_mutex};
    if (g_is_running) {
        g_is_running = false;
    }
    g_should_exit = true;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
}

} // namespace sphaira::ftpsrv

extern "C" {

#define VFS_NX_BUFFER_IO 1

int ftp_vfs_open(struct FtpVfsFile* f, const char* path, enum FtpVfsOpenMode mode) {
    u32 open_mode;
    if (mode == FtpVfsOpenMode_READ) {
        open_mode = FsOpenMode_Read;
        f->is_write = false;
    } else {
        fsFsCreateFile(g_fs, path, 0, 0);
        open_mode = FsOpenMode_Write | FsOpenMode_Append;
        #if !VFS_NX_BUFFER_IO
        open_mode |= FsOpenMode_Append;
        #endif
        f->is_write = true;
    }

    Result rc;
    if (R_FAILED(rc = fsFsOpenFile(g_fs, path, open_mode, &f->fd))) {
        return vfs_fs_set_errno(rc);
    }

    f->off = f->buf_off = f->buf_size = 0;

    if (mode == FtpVfsOpenMode_WRITE) {
        if (R_FAILED(rc = fsFileSetSize(&f->fd, 0))) {
            goto fail_close;
        }
    } else if (mode == FtpVfsOpenMode_APPEND) {
        if (R_FAILED(rc = fsFileGetSize(&f->fd, &f->off))) {
            goto fail_close;
        }
    }

    f->is_valid = true;
    return 0;

fail_close:
    fsFileClose(&f->fd);
    return vfs_fs_set_errno(rc);
}

int ftp_vfs_read(struct FtpVfsFile* f, void* buf, size_t size) {
    Result rc;

    #if VFS_NX_BUFFER_IO
    if (f->buf_off == f->buf_size) {
        u64 bytes_read;
        if (R_FAILED(rc = fsFileRead(&f->fd, f->off, f->buf, sizeof(f->buf), FsReadOption_None, &bytes_read))) {
            return vfs_fs_set_errno(rc);
        }

        f->buf_off = 0;
        f->buf_size = bytes_read;
    }

    if (!f->buf_size) {
        return 0;
    }

    size = size < f->buf_size - f->buf_off ? size : f->buf_size - f->buf_off;
    memcpy(buf, f->buf + f->buf_off, size);
    f->off += size;
    f->buf_off += size;
    return size;
#else
    u64 bytes_read;
    if (R_FAILED(rc = fsFileRead(&f->fd, f->off, buf, size, FsReadOption_None, &bytes_read))) {
        return vfs_fs_set_errno(rc);
    }
    f->off += bytes_read;
    return bytes_read;
#endif
}

int ftp_vfs_write(struct FtpVfsFile* f, const void* buf, size_t size) {
    Result rc;

#if VFS_NX_BUFFER_IO
    const size_t ret = size;
    while (size) {
        if (f->buf_off + size > sizeof(f->buf)) {
            const u64 sz = sizeof(f->buf) - f->buf_off;
            memcpy(f->buf + f->buf_off, buf, sz);
            f->buf_off += sz;

            if (R_FAILED(rc = flush_buffered_write(f))) {
                return vfs_fs_set_errno(rc);
            }

            buf += sz;
            size -= sz;
            f->off += f->buf_off;
            f->buf_off = 0;
        } else {
            memcpy(f->buf + f->buf_off, buf, size);
            f->buf_off += size;
            size = 0;
        }
    }

    return ret;
#else
    if (R_FAILED(rc = fsFileWrite(&f->fd, f->off, buf, size, FsWriteOption_None))) {
        return vfs_fs_set_errno(rc);
    }
    f->off += size;
    return size;
    const size_t ret = size;
#endif
}

// buf and size is the amount of data sent.
int ftp_vfs_seek(struct FtpVfsFile* f, const void* buf, size_t size, size_t off) {
#if VFS_NX_BUFFER_IO
    if (!f->is_write) {
        f->buf_off -= f->off - off;
    }
#endif
    f->off = off;
    return 0;
}

int ftp_vfs_close(struct FtpVfsFile* f) {
    if (!ftp_vfs_isfile_open(f)) {
        return -1;
    }

    if (f->is_write && f->buf_off) {
        flush_buffered_write(f);
    }

    fsFileClose(&f->fd);
    f->is_valid = false;
    return 0;
}

int ftp_vfs_isfile_open(struct FtpVfsFile* f) {
    return f->is_valid;
}

int ftp_vfs_opendir(struct FtpVfsDir* f, const char* path) {
    Result rc;
    if (R_FAILED(rc = fsFsOpenDirectory(g_fs, path, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles | FsDirOpenMode_NoFileSize, &f->dir))) {
        return vfs_fs_set_errno(rc);
    }
    f->is_valid = true;
    return 0;
}

const char* ftp_vfs_readdir(struct FtpVfsDir* f, struct FtpVfsDirEntry* entry) {
    Result rc;
    s64 total_entries;
    if (R_FAILED(rc = fsDirRead(&f->dir, &total_entries, 1, &entry->buf))) {
        vfs_fs_set_errno(rc);
        return NULL;
    }

    if (total_entries <= 0) {
        return NULL;
    }

    return entry->buf.name;
}

int ftp_vfs_dirlstat(struct FtpVfsDir* f, const struct FtpVfsDirEntry* entry, const char* path, struct stat* st) {
    return lstat(path, st);
}

int ftp_vfs_closedir(struct FtpVfsDir* f) {
    if (!ftp_vfs_isdir_open(f)) {
        return -1;
    }

    fsDirClose(&f->dir);
    f->is_valid = false;
    return 0;
}

int ftp_vfs_isdir_open(struct FtpVfsDir* f) {
    return f->is_valid;
}

int ftp_vfs_stat(const char* path, struct stat* st) {
    return stat(path, st);
}

int ftp_vfs_lstat(const char* path, struct stat* st) {
    return lstat(path, st);
}

int ftp_vfs_mkdir(const char* path) {
    return mkdir(path, 0777);
}

int ftp_vfs_unlink(const char* path) {
    return unlink(path);
}

int ftp_vfs_rmdir(const char* path) {
    return rmdir(path);
}

int ftp_vfs_rename(const char* src, const char* dst) {
    return rename(src, dst);
}

int ftp_vfs_readlink(const char* path, char* buf, size_t buflen) {
    return -1;
}

const char* ftp_vfs_getpwuid(const struct stat* st) {
    return "unknown";
}

const char* ftp_vfs_getgrgid(const struct stat* st) {
    return "unknown";
}

} // extern "C"

#include "ftpsrv_helper.hpp"

#include "app.hpp"
#include "fs.hpp"
#include "log.hpp"

#include <mutex>
#include <algorithm>
#include <minIni.h>
#include <ftpsrv.h>
#include <ftpsrv_vfs.h>
#include <nx/vfs_nx.h>
#include <nx/utils.h>

namespace {

const char* INI_PATH = "/config/ftpsrv/config.ini";
FtpSrvConfig g_ftpsrv_config = {0};
volatile bool g_should_exit = false;
bool g_is_running{false};
Thread g_thread;
std::mutex g_mutex{};

void ftp_log_callback(enum FTP_API_LOG_TYPE type, const char* msg) {
    sphaira::App::NotifyFlashLed();
}

void ftp_progress_callback(void) {
    sphaira::App::NotifyFlashLed();
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

    if (R_FAILED(fsdev_wrapMountSdmc())) {
        return false;
    }

    g_ftpsrv_config.log_callback = ftp_log_callback;
    g_ftpsrv_config.progress_callback = ftp_progress_callback;
    g_ftpsrv_config.anon = ini_getbool("Login", "anon", 0, INI_PATH);
    int user_len = ini_gets("Login", "user", "", g_ftpsrv_config.user, sizeof(g_ftpsrv_config.user), INI_PATH);
    int pass_len = ini_gets("Login", "pass", "", g_ftpsrv_config.pass, sizeof(g_ftpsrv_config.pass), INI_PATH);
    g_ftpsrv_config.port = ini_getl("Network", "port", 5000, INI_PATH); // 5000 to keep compat with older sphaira
    g_ftpsrv_config.timeout = ini_getl("Network", "timeout", 0, INI_PATH);
    g_ftpsrv_config.use_localtime = ini_getbool("Misc", "use_localtime", 0, INI_PATH);
    bool log_enabled = ini_getbool("Log", "log", 0, INI_PATH);

    // get nx config
    bool mount_devices = ini_getbool("Nx", "mount_devices", 1, INI_PATH);
    bool mount_bis = ini_getbool("Nx", "mount_bis", 0, INI_PATH);
    bool save_writable = ini_getbool("Nx", "save_writable", 0, INI_PATH);
    g_ftpsrv_config.port = ini_getl("Nx", "app_port", g_ftpsrv_config.port, INI_PATH); // compat

    // get Nx-App overrides
    g_ftpsrv_config.anon = ini_getbool("Nx-App", "anon", g_ftpsrv_config.anon, INI_PATH);
    user_len = ini_gets("Nx-App", "user", g_ftpsrv_config.user, g_ftpsrv_config.user, sizeof(g_ftpsrv_config.user), INI_PATH);
    pass_len = ini_gets("Nx-App", "pass", g_ftpsrv_config.pass, g_ftpsrv_config.pass, sizeof(g_ftpsrv_config.pass), INI_PATH);
    g_ftpsrv_config.port = ini_getl("Nx-App", "port", g_ftpsrv_config.port, INI_PATH);
    g_ftpsrv_config.timeout = ini_getl("Nx-App", "timeout", g_ftpsrv_config.timeout, INI_PATH);
    g_ftpsrv_config.use_localtime = ini_getbool("Nx-App", "use_localtime", g_ftpsrv_config.use_localtime, INI_PATH);
    log_enabled = ini_getbool("Nx-App", "log", log_enabled, INI_PATH);
    mount_devices = ini_getbool("Nx-App", "mount_devices", mount_devices, INI_PATH);
    mount_bis = ini_getbool("Nx-App", "mount_bis", mount_bis, INI_PATH);
    save_writable = ini_getbool("Nx-App", "save_writable", save_writable, INI_PATH);

    if (!g_ftpsrv_config.port) {
        return false;
    }

    // keep compat with older sphaira
    if (!user_len && !pass_len) {
        g_ftpsrv_config.anon = true;
    }

    vfs_nx_init(mount_devices, save_writable, mount_bis);

    Result rc;
    if (R_FAILED(rc = threadCreate(&g_thread, loop, nullptr, nullptr, 1024*16, 0x2C, 2))) {
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

    vfs_nx_exit();
    fsdev_wrapUnmountAll();
}

} // namespace sphaira::ftpsrv

extern "C" {

void log_file_write(const char* msg) {
    log_write("%s", msg);
}

void log_file_fwrite(const char* fmt, ...) {
    std::va_list v{};
    va_start(v, fmt);
    log_write_arg(fmt, v);
    va_end(v);
}

} // extern "C"

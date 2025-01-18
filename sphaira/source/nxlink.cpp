// very quick and dirty way to talk to nxlink-pc
#include "nxlink.h"
#include "defines.hpp"
#include "nro.hpp"
#include "log.hpp"
#include "fs.hpp"

#include <cstring>
#include <vector>
#include <mutex>
#include <string>
// #include <string_view>

#include <zlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>

namespace {

using Socket = int;
constexpr s32 SERVER_PORT = NXLINK_SERVER_PORT;
constexpr s32 CLIENT_PORT = NXLINK_CLIENT_PORT;
constexpr s32 ZLIB_CHUNK = 0x4000;

constexpr s32 ERR_OK = 0;
constexpr s32 ERR_FILE = -1;
constexpr s32 ERR_SPACE = -2;
constexpr s32 ERR_MEM = -3;

constexpr const char UDP_MAGIC_SERVER[] = {"nxboot"};
constexpr const char UDP_MAGIC_CLIENT[] = {"bootnx"};

Thread g_thread{};
std::mutex g_mutex{};
std::atomic_bool g_quit{false};
bool g_is_running{false};
NxlinkCallback g_callback{};

struct SocketWrapper {
    SocketWrapper(Socket socket) : sock{socket} {
        this->nonBlocking();
    }
    SocketWrapper(int af, int type, int proto) {
        this->sock = socket(af, type, proto);
        this->nonBlocking();
    }
    ~SocketWrapper() {
        if (this->sock > 0) {
            shutdown(this->sock, SHUT_RDWR);
            close(this->sock);
        }
    }
    void nonBlocking() {
        fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
    }
    Socket operator=(Socket s) { return this->sock = s; }
    operator int() { return this->sock; }
    Socket sock{};
};

struct ZlibWrapper {
    ZlibWrapper() { inflateInit(&strm); }
    ZlibWrapper(u8* out, size_t size) {
        inflateInit(&strm);
        strm.avail_out = size;
        strm.next_out = out;
    }
    ~ZlibWrapper() { inflateEnd(&strm); }
    z_stream* operator&() { return &this->strm; }
    int Inflate(int flush) { return inflate(&this->strm, flush); }
    void Setup(u8* in, size_t size) {
        strm.next_in = in;
        strm.avail_in = size;
    }
    z_stream strm{};
};

void WriteCallbackNone(NxlinkCallbackType type) {
    if (!g_callback) {
        return;
    }
    NxlinkCallbackData data = {type};
    g_callback(&data);
}

void WriteCallbackFile(NxlinkCallbackType type, const char* name) {
    if (!g_callback) {
        return;
    }
    NxlinkCallbackData data = {type};
    std::strcpy(data.file.filename, name);
    g_callback(&data);
}

void WriteCallbackProgress(NxlinkCallbackType type, s64 offset, s64 size) {
    if (!g_callback) {
        return;
    }
    NxlinkCallbackData data = {type};
    data.progress.offset = offset;
    data.progress.size = size;
    g_callback(&data);
}

auto recvall(int sock, void* buf, int size) -> bool {
    auto p = static_cast<u8*>(buf);
    int got{}, left{size};
    while (!g_quit && got < size) {
        const auto len = recv(sock, p + got, left, 0);
        if (len == -1) {
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                return false;
            }
            svcSleepThread(1e+6);
        } else {
            got += len;
            left -= len;
        }
    }
    return !g_quit;
}

auto sendall(Socket sock, const void* buf, int size) -> bool {
    auto p = static_cast<const u8*>(buf);
    int sent{}, left{size};
    while (!g_quit && sent < size) {
        const auto len = send(sock, p + sent, left, 0);
        if (len == -1) {
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                return false;
            }
            svcSleepThread(1e+6);
        }
        sent += len;
        left -= len;
    }
    return !g_quit;
}

auto get_file_data(Socket sock, int max) -> std::vector<u8> {
    std::vector<u8> buf(max);
    std::vector<u8> chunk(ZLIB_CHUNK);
    ZlibWrapper zlib{buf.data(), buf.size()};
    u32 want{};

    while (zlib.strm.total_out < buf.size()) {
        if (g_quit) {
            return {};
        }

        if (!recvall(sock, &want, sizeof(want))) {
            return {};
        }

        if (want > chunk.size()) {
            want = chunk.size();
        }

        if (!recvall(sock, chunk.data(), want)) {
            return {};
        }

        WriteCallbackProgress(NxlinkCallbackType_WriteProgress, want, max);
        zlib.Setup(chunk.data(), want);
        zlib.Inflate(Z_NO_FLUSH);
    }

    return buf;
}

void loop(void* args) {
    log_write("in nxlink thread func\n");
    const sockaddr_in servaddr{
        .sin_family = AF_INET,
        .sin_port = htons(SERVER_PORT),
        .sin_addr = htonl(INADDR_ANY),
    };

    Result rc;

    const auto poll_network_change = []() -> bool {
        static u32 current_addr, subnet_mask, gateway, primary_dns_server, secondary_dns_server;
        u32 t_current_addr, t_subnet_mask, t_gateway, t_primary_dns_server, t_secondary_dns_server;

        if (R_FAILED(nifmGetCurrentIpConfigInfo(&t_current_addr, &t_subnet_mask, &t_gateway, &t_primary_dns_server, &t_secondary_dns_server))) {
            return true;
        }

        if (current_addr != t_current_addr || subnet_mask != t_subnet_mask || gateway != t_gateway || primary_dns_server != t_primary_dns_server || secondary_dns_server != t_secondary_dns_server) {
            current_addr = t_current_addr;
            subnet_mask = t_subnet_mask;
            gateway = t_gateway;
            primary_dns_server = t_primary_dns_server;
            secondary_dns_server = t_secondary_dns_server;
            return true;
        }

        return false;
    };

    while (!g_quit) {
        svcSleepThread(1e+8);

        if (poll_network_change()) {
            continue;
        }

        SocketWrapper sock{AF_INET, SOCK_STREAM, 0};
        SocketWrapper sock_udp(AF_INET, SOCK_DGRAM, 0);

        if (sock < 0 || sock_udp < 0) {
            log_write("failed to get sock/sock_udp: 0x%X\n", socketGetLastResult());
            continue;
        }

        u32 tmpval = 1;
        if (0 > setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &tmpval, sizeof(tmpval))) {
            log_write("set sockopt(): 0x%X\n", socketGetLastResult());
            continue;
        }

        if (0 > setsockopt(sock_udp, SOL_SOCKET, SO_REUSEADDR, &tmpval, sizeof(tmpval))) {
            log_write("set sockopt(): 0x%X\n", socketGetLastResult());
            continue;
        }

        if (0 > bind(sock, (const sockaddr*)&servaddr, sizeof(servaddr))) {
            log_write("failed to get bind(sock): 0x%X\n", socketGetLastResult());
            continue;
        }

        if (0 > bind(sock_udp, (const sockaddr*)&servaddr, sizeof(servaddr))) {
            log_write("failed to get bind(sock_udp): 0x%X\n", socketGetLastResult());
            continue;
        }

        if (0 > listen(sock, 10)) {
            log_write("failed to get listen: 0x%X\n", socketGetLastResult());
            continue;
        }

        sockaddr_in sa_remote{};

        pollfd pfds[2];
        pfds[0].fd = sock;
        pfds[0].events = POLLIN;
        pfds[1].fd = sock_udp;
        pfds[1].events = POLLIN;

        while (!g_quit) {
            auto poll_rc = poll(pfds, std::size(pfds), 1000/60);
            if (poll_rc < 0) {
                break;
            } else if (poll_rc == 0) {
                continue;
            } else if ((pfds[0].revents & (POLLERR|POLLHUP|POLLNVAL)) || (pfds[1].revents & (POLLERR|POLLHUP|POLLNVAL))) {
                break;
            }

            if (pfds[1].revents & POLLIN) {
                char recvbuf[6];
                socklen_t from_len = sizeof(sa_remote);
                auto udp_len = recvfrom(sock_udp, recvbuf, sizeof(recvbuf), 0, (sockaddr*)&sa_remote, &from_len);
                if (udp_len == sizeof(recvbuf) && !std::strncmp(recvbuf, UDP_MAGIC_SERVER, std::strlen(UDP_MAGIC_SERVER))) {
                    // log_write("got udp len: %d - %.*s\n", udp_len, udp_len, recvbuf);
                    sa_remote.sin_family = AF_INET;
                    sa_remote.sin_port = htons(NXLINK_CLIENT_PORT);
                    udp_len = sendto(sock_udp, UDP_MAGIC_CLIENT, std::strlen(UDP_MAGIC_CLIENT), 0, (const sockaddr*)&sa_remote, sizeof(sa_remote));
                    if (udp_len != std::strlen(UDP_MAGIC_CLIENT)) {
                        log_write("nxlink failed to send udp packet\n");
                        continue;
                    }
                }
            }

            socklen_t accept_len = sizeof(sa_remote);
            SocketWrapper connfd = accept(sock, (sockaddr*)&sa_remote, &accept_len);
            if (connfd < 0) {
                continue;
            }
            WriteCallbackNone(NxlinkCallbackType_Connected);

            u32 namelen{};
            if (!recvall(connfd, &namelen, sizeof(namelen))) {
                log_write("failed to get name: 0x%X\n", socketGetLastResult());
                continue;
            }

            fs::FsPath name{};
            if (namelen >= sizeof(name)) {
                log_write("namelen is bigger than name: 0x%X\n", socketGetLastResult());
                continue;
            }

            if (!recvall(connfd, name, namelen)) {
                log_write("failed to get name: 0x%X\n", socketGetLastResult());
                continue;
            }

            log_write("got name: %s\n", name.s);

            u32 filesize{};
            if (!recvall(connfd, &filesize, sizeof(filesize))) {
                log_write("failed to get filesize: 0x%X\n", socketGetLastResult());
                continue;
            }

            // check that we have enough space
            fs::FsNativeSd fs;
            s64 sd_storage_space_free;
            if (R_FAILED(fs.GetFreeSpace("/", &sd_storage_space_free)) || filesize >= sd_storage_space_free) {
                sendall(connfd, &ERR_SPACE, sizeof(ERR_SPACE));
                continue;
            }

            // tell nxlink that we want this file
            if (!sendall(connfd, &ERR_OK, sizeof(ERR_OK))) {
                log_write("failed to tell nxlink that we want the file: 0x%X\n", socketGetLastResult());
                continue;
            }

            // todo: verify nro magic here
            WriteCallbackFile(NxlinkCallbackType_WriteBegin, name);
            const auto file_data = get_file_data(connfd, filesize);
            WriteCallbackFile(NxlinkCallbackType_WriteEnd, name);

            if (file_data.empty()) {
                continue;
            }

            fs::FsPath path;
            // if (!name_view.starts_with("/") && !name_view.starts_with("sdmc:/")) {
            if (name[0] != '/' && strncasecmp(name, "sdmc:/", std::strlen("sdmc:/"))) {
                path = "/switch/" + name;
            } else {
                path = name;
            }

            // if (R_FAILED(rc = create_directories(fs, path))) {
            if (R_FAILED(rc = fs.CreateDirectoryRecursivelyWithPath(path))) {
                sendall(connfd, &ERR_FILE, sizeof(ERR_FILE));
                log_write("failed to create directories: %X\n", rc);
                continue;
            }

            // this is the path we will write to
            const auto temp_path = path + "~";
            if (R_FAILED(rc = fs.CreateFile(temp_path, file_data.size(), 0)) && rc != FsError_PathAlreadyExists) {
                sendall(connfd, &ERR_FILE, sizeof(ERR_FILE));
                log_write("failed to create file: %X\n", rc);
                continue;
            }
            ON_SCOPE_EXIT(fs.DeleteFile(temp_path));

            {
                FsFile f;
                if (R_FAILED(rc = fs.OpenFile(temp_path, FsOpenMode_Write, &f))) {
                    sendall(connfd, &ERR_FILE, sizeof(ERR_FILE));
                    log_write("failed to open file %X\n", rc);
                    continue;
                }
                ON_SCOPE_EXIT(fsFileClose(&f));

                if (R_FAILED(rc = fsFileSetSize(&f, file_data.size()))) {
                    sendall(connfd, &ERR_FILE, sizeof(ERR_FILE));
                    log_write("failed to set file size: 0x%X\n", socketGetLastResult());
                    continue;
                }

                u64 offset = 0;
                while (offset < file_data.size()) {
                    svcSleepThread(YieldType_WithoutCoreMigration);

                    u64 chunk_size = ZLIB_CHUNK;
                    if (offset + chunk_size > file_data.size()) {
                        chunk_size = file_data.size() - offset;
                    }
                    if (R_FAILED(rc = fsFileWrite(&f, offset, file_data.data() + offset, chunk_size, FsWriteOption_None))) {
                        break;
                    }
                    offset += chunk_size;
                }

                // if (R_FAILED(rc = fsFileWrite(&f, 0, file_data.data(), file_data.size(), FsWriteOption_None))) {
                if (R_FAILED(rc)) {
                    sendall(connfd, &ERR_FILE, sizeof(ERR_FILE));
                    log_write("failed to write: 0x%X\n", socketGetLastResult());
                    continue;
                }
            }

            if (R_FAILED(rc = fs.DeleteFile(path)) && rc != FsError_PathNotFound) {
                log_write("failed to delete %X\n", rc);
                continue;
            }

            if (R_FAILED(rc = fs.RenameFile(temp_path, path))) {
                log_write("failed to rename %X\n", rc);
                continue;
            }

            if (!sendall(connfd, &ERR_OK, sizeof(ERR_OK))) {
                log_write("failed to send ok message: 0x%X\n", socketGetLastResult());
                continue;
            }

            if (R_SUCCEEDED(sphaira::nro_verify(file_data))) {
                std::string args{};

                // try and get args
                u32 args_len{};
                char args_buf[256]{};
                if (recvall(connfd, &args_len, sizeof(args_len))) {
                    args_len = std::min<u32>(args_len, sizeof(args_buf));
                    if (recvall(connfd, args_buf, args_len) && args_len > 0) {
                        // change NULL into spaces
                        for (u32 i = 0; i < args_len; i++) {
                            if (args_buf[i] == '\0') {
                                args_buf[i] = ' ';
                            }
                        }

                        args += args_buf;
                    }
                }

                // this allows for nxlink server to be activated
                char nxlinked[17]{};
                std::snprintf(nxlinked, sizeof(nxlinked), "%08X_NXLINK_", sa_remote.sin_addr.s_addr);
                if (!args.empty()) {
                    args += ' ';
                }
                args += nxlinked;

                // log_write("launching with: %s %s\n", path.c_str(), args.c_str());
                if (R_SUCCEEDED(sphaira::nro_launch(path, args))) {
                    g_quit = true;
                }
            }
        }
    }
}

} // namespace

extern "C" {

bool nxlinkInitialize(NxlinkCallback callback) {
    std::scoped_lock lock{g_mutex};
    if (g_is_running) {
        return false;
    }

    g_callback = callback;
    g_quit = false;

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

void nxlinkExit() {
    std::scoped_lock lock{g_mutex};
    if (g_is_running) {
        g_is_running = false;
    }
    g_quit = true;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
}

} // extern "C"

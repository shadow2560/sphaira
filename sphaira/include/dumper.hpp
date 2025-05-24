#pragma once

#include "fs.hpp"
#include <switch.h>
#include <vector>
#include <memory>
#include <functional>

namespace sphaira::dump {

enum DumpLocationType {
    // dump using native fs.
    DumpLocationType_SdCard,
    // dump to usb using tinfoil protocol.
    DumpLocationType_UsbS2S,
    // speed test, only reads the data, doesn't write anything.
    DumpLocationType_DevNull,
    // dump to stdio, ideal for custom mount points using devoptab, such as hdd.
    DumpLocationType_Stdio,
    // dump to custom locations found in locations.ini.
    DumpLocationType_Network,
};

enum DumpLocationFlag {
    DumpLocationFlag_SdCard = 1 << DumpLocationType_SdCard,
    DumpLocationFlag_UsbS2S = 1 << DumpLocationType_UsbS2S,
    DumpLocationFlag_DevNull = 1 << DumpLocationType_DevNull,
    DumpLocationFlag_Stdio = 1 << DumpLocationType_Stdio,
    DumpLocationFlag_Network = 1 << DumpLocationType_Network,
    DumpLocationFlag_All = DumpLocationFlag_SdCard | DumpLocationFlag_UsbS2S | DumpLocationFlag_DevNull | DumpLocationFlag_Stdio | DumpLocationFlag_Network,
};

struct BaseSource {
    virtual ~BaseSource() = default;
    virtual Result Read(const std::string& path, void* buf, s64 off, s64 size, u64* bytes_read) = 0;
    virtual auto GetName(const std::string& path) const -> std::string = 0;
    virtual auto GetSize(const std::string& path) const -> s64 = 0;
    virtual auto GetIcon(const std::string& path) const -> int { return 0; }
};

// called after dump has finished.
using OnExit = std::function<void(Result rc)>;

void Dump(std::shared_ptr<BaseSource> source, const std::vector<fs::FsPath>& paths, OnExit on_exit = [](Result){}, u32 location_flags = DumpLocationFlag_All);

} // namespace sphaira::dump

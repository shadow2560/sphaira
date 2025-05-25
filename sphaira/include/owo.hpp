#pragma once

#include <switch.h>
#include <string>
#include <vector>
#include "ui/progress_box.hpp"

namespace sphaira {

struct OwoConfig {
    std::string nro_path;
    std::string args{};
    std::string name{};
    std::string author{};
    NacpStruct nacp;
    std::vector<u8> icon;
    std::vector<u8> logo;
    std::vector<u8> gif;

    std::vector<u8> program_nca{};
};

enum {
    Module_Owo = 424,
};

enum OwoError {
    OwoError_BadArgs = MAKERESULT(Module_Owo, 1),
};

// fwd
// struct ui::ProgressBox;

auto install_forwarder(OwoConfig& config, NcmStorageId storage_id) -> Result;
auto install_forwarder(ui::ProgressBox* pbox, OwoConfig& config, NcmStorageId storage_id) -> Result;

} // namespace sphaira

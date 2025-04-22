#include "swkbd.hpp"
#include "defines.hpp"
#include <cstdlib>

namespace sphaira::swkbd {
namespace {

struct Config {
    char out_text[FS_MAX_PATH]{};
    bool numpad{};
};

Result ShowInternal(Config& cfg, const char* guide, const char* initial, s64 len_min, s64 len_max) {
    SwkbdConfig c;
    R_TRY(swkbdCreate(&c, 0));
    swkbdConfigMakePresetDefault(&c);
    swkbdConfigSetInitialCursorPos(&c, 1);

    if (cfg.numpad) {
        swkbdConfigSetType(&c, SwkbdType_NumPad);
    }

    if (guide) {
        swkbdConfigSetGuideText(&c, guide);
    }

    if (initial) {
        swkbdConfigSetInitialText(&c, initial);
    }

    if (len_min >= 0) {
        swkbdConfigSetStringLenMin(&c, len_min);
    }

    if (len_max >= 0) {
        swkbdConfigSetStringLenMax(&c, len_max);
    }

    return swkbdShow(&c, cfg.out_text, sizeof(cfg.out_text));
}

} // namespace

Result ShowText(std::string& out, const char* guide, const char* initial, s64 len_min, s64 len_max) {
    Config cfg{};
    R_TRY(ShowInternal(cfg, guide, initial, len_min, len_max));
    out = cfg.out_text;
    R_SUCCEED();
}

Result ShowNumPad(s64& out, const char* guide, const char* initial, s64 len_min, s64 len_max) {
    Config cfg{};
    cfg.numpad = true;
    R_TRY(ShowInternal(cfg, guide, initial, len_min, len_max));
    out = std::atoll(cfg.out_text);
    R_SUCCEED();
}

} // namespace sphaira::swkbd

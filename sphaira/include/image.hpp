#pragma once

#include <vector>
#include <span>
#include <switch.h>
#include "fs.hpp"

namespace sphaira {

struct ImageResult {
    std::vector<u8> data;
    int w, h;
};

auto ImageLoadFromMemory(std::span<const u8> data) -> ImageResult;
auto ImageLoadFromFile(const fs::FsPath& file) -> ImageResult;
auto ImageResize(std::span<const u8> data, int inx, int iny, int outx, int outy) -> ImageResult;
auto ImageConvertToJpg(std::span<const u8> data, int x, int y) -> ImageResult;

} // namespace sphaira

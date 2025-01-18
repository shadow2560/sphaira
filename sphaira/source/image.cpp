#include "image.hpp"

// disable warnings for stb
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Warray-bounds="
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_STATIC
#include <stb_image_resize2.h>
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop

#include "log.hpp"
#include <cstring>

namespace sphaira {
namespace {

constexpr int BPP = 4;

auto ImageLoadInternal(stbi_uc* image_data, int x, int y) -> ImageResult {
    if (image_data) {
        log_write("loaded image: w: %d h: %d\n", x, y);
        ImageResult result{};
        result.data.resize(x*y*BPP);
        result.w = x;
        result.h = y;
        std::memcpy(result.data.data(), image_data, result.data.size());
        stbi_image_free(image_data);
        return result;
    }

    log_write("failed image load\n");
    return {};
}

} // namespace

auto ImageLoadFromMemory(std::span<const u8> data) -> ImageResult {
    int x, y, channels;
    return ImageLoadInternal(stbi_load_from_memory(data.data(), data.size(), &x, &y, &channels, BPP), x, y);
}

auto ImageLoadFromFile(const fs::FsPath& file) -> ImageResult {
    log_write("doing file load\n");
    int x, y, channels;
    return ImageLoadInternal(stbi_load(file, &x, &y, &channels, BPP), x, y);
}

auto ImageResize(std::span<const u8> data, int inx, int iny, int outx, int outy) -> ImageResult {
    log_write("doing resize inx: %d iny: %d outx: %d outy: %d\n", inx, iny, outx, outy);
    std::vector<u8> resized_data(outx*outy*BPP);

    // if (stbir_resize_uint8(data.data(), inx, iny, inx * BPP, resized_data.data(), outx, outy, outx*BPP, BPP)) {
    if (stbir_resize_uint8_linear(data.data(), inx, iny, inx * BPP, resized_data.data(), outx, outy, outx*BPP, (stbir_pixel_layout)BPP)) {
        log_write("did resize\n");
        return { resized_data, outx, outy };
    }
    log_write("failed resize\n");
    return {};
}

auto ImageConvertToJpg(std::span<const u8> data, int x, int y) -> ImageResult {
    std::vector<u8> out;
    out.reserve(x*y*BPP);
    log_write("doing jpeg convert\n");

    const auto cb = [](void *context, void *data, int size) -> void {
        auto buf = static_cast<std::vector<u8>*>(context);
        const auto offset = buf->size();
        buf->resize(offset + size);
        std::memcpy(buf->data() + offset, data, size);
    };

    if (stbi_write_jpg_to_func(cb, &out, x, y, 4, data.data(), 93)) {
        // out.shrink_to_fit();
        log_write("did jpg convert\n");
        return { out, x, y };
    }

    log_write("failed jpg convert\n");
    return {};
}

} // namespace sphaira

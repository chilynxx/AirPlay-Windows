#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ap::video {

struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;   // width * height * 4, straight (non-premultiplied) alpha
};

// Decodes a PNG, JPEG or BMP image into RGBA.
//  * A JPEG's EXIF orientation is applied, so photos taken with a phone come
//    out the right way up.
//  * An image whose longer side exceeds `max_side` is shrunk (box filter) so a
//    huge photo does not waste video memory.
//  * Files over 40 MB, images over 16 megapixels, or dimensions beyond 16384 pixels per side are refused.
// On failure returns false and, when `error` is given, a short reason.
bool decode_image_rgba(const uint8_t* data, std::size_t size, int max_side,
                       DecodedImage& out, std::string* error = nullptr);

bool load_image_file_rgba(const std::filesystem::path& path, int max_side,
                          DecodedImage& out, std::string* error = nullptr);

// EXIF orientation (1..8) of a JPEG, or 1 when absent/invalid. Exposed for tests.
int jpeg_exif_orientation(const uint8_t* data, std::size_t size);

} // namespace ap::video

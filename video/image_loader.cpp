#include "video/image_loader.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS (1 << 14)
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

namespace ap::video {

namespace {

constexpr std::size_t kMaxFileBytes = 40u * 1024u * 1024u;
// Bound the decoded RGBA allocation: 16 MP is at most 64 MiB per buffer.
constexpr std::size_t kMaxDecodedPixels = 16u * 1024u * 1024u;

void set_error(std::string* error, const char* text) {
    if (error) *error = text;
}

uint16_t rd16(const uint8_t* p, bool little) {
    return little ? static_cast<uint16_t>(p[0] | (p[1] << 8))
                  : static_cast<uint16_t>((p[0] << 8) | p[1]);
}
uint32_t rd32(const uint8_t* p, bool little) {
    return little ? (static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                     (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24))
                  : ((static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                     (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]));
}

// Returns a transformed copy. `orientation` follows the EXIF numbering.
DecodedImage apply_orientation(DecodedImage in, int orientation) {
    if (orientation <= 1 || orientation > 8) return in;
    const bool swap = orientation >= 5;                  // 90-degree turns exchange the sides
    DecodedImage out;
    out.width  = swap ? in.height : in.width;
    out.height = swap ? in.width  : in.height;
    out.rgba.resize(in.rgba.size());
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            int nx = x, ny = y;
            switch (orientation) {
                case 2: nx = in.width - 1 - x;                       break;  // mirror
                case 3: nx = in.width - 1 - x; ny = in.height - 1 - y; break;  // 180
                case 4: ny = in.height - 1 - y;                      break;  // flip
                case 5: nx = y;                  ny = x;                   break;  // transpose
                case 6: nx = in.height - 1 - y;  ny = x;                   break;  // 90 cw
                case 7: nx = in.height - 1 - y;  ny = in.width - 1 - x;    break;  // transverse
                case 8: nx = y;                  ny = in.width - 1 - x;    break;  // 90 ccw
                default: break;
            }
            std::memcpy(&out.rgba[(static_cast<std::size_t>(ny) * out.width + nx) * 4],
                        &in.rgba[(static_cast<std::size_t>(y) * in.width + x) * 4], 4);
        }
    }
    return out;
}

// Box-filter downscale so that the longer side is at most `max_side`.
DecodedImage shrink(DecodedImage in, int max_side) {
    const int longest = std::max(in.width, in.height);
    if (max_side <= 0 || longest <= max_side) return in;
    const double scale = static_cast<double>(max_side) / longest;
    DecodedImage out;
    out.width  = std::max(1, static_cast<int>(in.width * scale + 0.5));
    out.height = std::max(1, static_cast<int>(in.height * scale + 0.5));
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height * 4, 0);
    for (int y = 0; y < out.height; ++y) {
        const int y0 = static_cast<int>(static_cast<double>(y) * in.height / out.height);
        const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<double>(y + 1) * in.height / out.height));
        for (int x = 0; x < out.width; ++x) {
            const int x0 = static_cast<int>(static_cast<double>(x) * in.width / out.width);
            const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<double>(x + 1) * in.width / out.width));
            uint64_t sum[4] = {0, 0, 0, 0};
            int count = 0;
            for (int yy = y0; yy < std::min(y1, in.height); ++yy)
                for (int xx = x0; xx < std::min(x1, in.width); ++xx) {
                    const uint8_t* p = &in.rgba[(static_cast<std::size_t>(yy) * in.width + xx) * 4];
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; sum[3] += p[3];
                    ++count;
                }
            uint8_t* q = &out.rgba[(static_cast<std::size_t>(y) * out.width + x) * 4];
            for (int c = 0; c < 4; ++c) q[c] = static_cast<uint8_t>(count ? sum[c] / count : 0);
        }
    }
    return out;
}

} // namespace

int jpeg_exif_orientation(const uint8_t* data, std::size_t size) {
    if (!data || size < 4 || data[0] != 0xFF || data[1] != 0xD8) return 1;

    std::size_t i = 2;
    while (i <= size && size - i >= 4) {
        if (data[i] != 0xFF) return 1;
        const uint8_t marker = data[i + 1];
        if (marker == 0xD9 || marker == 0xDA) return 1; // end / start of scan

        const std::size_t length = rd16(data + i + 2, false);
        if (length < 2 || length > size - i - 2) return 1;

        if (marker == 0xE1 && length >= 16 &&
            std::memcmp(data + i + 4, "Exif\0\0", 6) == 0) {
            const uint8_t* tiff = data + i + 10;
            const std::size_t tiff_size = length - 8;
            if (tiff_size < 8) return 1;

            const bool little = tiff[0] == 'I' && tiff[1] == 'I';
            if (!little && !(tiff[0] == 'M' && tiff[1] == 'M')) return 1;
            if (rd16(tiff + 2, little) != 42) return 1;

            const std::size_t ifd = static_cast<std::size_t>(rd32(tiff + 4, little));
            // Check the offset before subtracting. `ifd + 2` can overflow when
            // the TIFF offset is attacker-controlled (e.g. 0xfffffff0).
            if (ifd > tiff_size || tiff_size - ifd < 2) return 1;

            const std::size_t entries = rd16(tiff + ifd, little);
            const std::size_t available_entries = (tiff_size - ifd - 2) / 12;
            if (entries > available_entries) return 1;

            for (std::size_t e = 0; e < entries; ++e) {
                const std::size_t at = ifd + 2 + e * 12;
                if (rd16(tiff + at, little) != 0x0112) continue;
                // Orientation is stored inline as one SHORT value.
                if (rd16(tiff + at + 2, little) != 3 ||
                    rd32(tiff + at + 4, little) != 1) return 1;
                const int value = rd16(tiff + at + 8, little);
                return (value >= 1 && value <= 8) ? value : 1;
            }
            return 1;
        }
        i += 2 + length;
    }
    return 1;
}

bool decode_image_rgba(const uint8_t* data, std::size_t size, int max_side,
                       DecodedImage& out, std::string* error) {
    out = DecodedImage{};
    if (error) error->clear();
    if (!data || size < 16) { set_error(error, "the file is empty or too small"); return false; }
    if (size > kMaxFileBytes) { set_error(error, "the file is larger than 40 MB"); return false; }

    // Inspect dimensions before decoding. A tiny compressed image can expand
    // into a huge RGBA allocation, so the compressed-file limit is not enough.
    int w = 0, h = 0, channels = 0;
    if (!stbi_info_from_memory(data, static_cast<int>(size), &w, &h, &channels)) {
        set_error(error, "not a valid PNG, JPEG or BMP image");
        return false;
    }
    if (w <= 0 || h <= 0 || w > (1 << 14) || h > (1 << 14)) {
        set_error(error, "image dimensions are outside the supported range");
        return false;
    }
    const std::size_t width = static_cast<std::size_t>(w);
    const std::size_t height = static_cast<std::size_t>(h);
    if (width > kMaxDecodedPixels / height) {
        set_error(error, "image exceeds the 16-megapixel decoded limit");
        return false;
    }

    stbi_uc* loaded = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 4);
    if (!loaded) { set_error(error, "image decoding failed"); return false; }
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(loaded, &stbi_image_free);
    if (w != static_cast<int>(width) || h != static_cast<int>(height)) {
        set_error(error, "image dimensions changed during decoding");
        return false;
    }

    try {
        DecodedImage raw;
        raw.width = w;
        raw.height = h;
        const std::size_t rgba_bytes = width * height * 4;
        raw.rgba.assign(pixels.get(), pixels.get() + rgba_bytes);
        // Drop stb's temporary buffer before rotate/resize allocations.
        pixels.reset();

        if (size > 3 && data[0] == 0xFF && data[1] == 0xD8)
            raw = apply_orientation(std::move(raw), jpeg_exif_orientation(data, size));
        out = shrink(std::move(raw), max_side);
        return true;
    } catch (const std::bad_alloc&) {
        out = DecodedImage{};
        set_error(error, "not enough memory to decode the image");
        return false;
    } catch (const std::length_error&) {
        out = DecodedImage{};
        set_error(error, "decoded image is too large");
        return false;
    }
}

bool load_image_file_rgba(const std::filesystem::path& path, int max_side,
                          DecodedImage& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { set_error(error, "the file cannot be opened"); return false; }
    in.seekg(0, std::ios::end);
    const auto length = static_cast<std::streamoff>(in.tellg());
    if (length < 0) {
        set_error(error, "the file size cannot be determined");
        return false;
    }
    if (static_cast<std::uintmax_t>(length) > kMaxFileBytes) {
        set_error(error, "the file is larger than 40 MB");
        return false;
    }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(length));
    if (length > 0) in.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!in) { set_error(error, "the file cannot be read"); return false; }
    return decode_image_rgba(bytes.data(), bytes.size(), max_side, out, error);
}

} // namespace ap::video

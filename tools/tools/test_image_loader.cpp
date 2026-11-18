// Wallpaper image decoding: formats, EXIF orientation, shrinking and bad input.

#include "image_fixtures.h"
#include "video/image_loader.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

using ap::video::DecodedImage;
using ap::video::decode_image_rgba;

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_failures;
}

static bool is_red(const DecodedImage& im, int x, int y) {
    const uint8_t* p = &im.rgba[(static_cast<size_t>(y) * im.width + x) * 4];
    return p[0] > 200 && p[1] < 60 && p[2] < 60;
}
static bool is_blue(const DecodedImage& im, int x, int y) {
    const uint8_t* p = &im.rgba[(static_cast<size_t>(y) * im.width + x) * 4];
    return p[2] > 200 && p[0] < 60 && p[1] < 60;
}

template <size_t N>
static bool decode(const uint8_t (&bytes)[N], DecodedImage& out, int max_side = 4096) {
    return decode_image_rgba(bytes, N, max_side, out);
}

int main() {
    DecodedImage im;
    std::string why;

    check(decode(fixtures::kPng, im) && im.width == 4 && im.height == 2, "PNG decodes with the right size");
    check(is_red(im, 0, 0) && is_blue(im, 3, 1), "PNG colours are right");

    check(decode(fixtures::kBmp, im) && im.width == 4 && im.height == 2, "BMP decodes with the right size");
    check(is_red(im, 1, 0) && is_blue(im, 2, 1), "BMP is not upside down");

    check(decode(fixtures::kJpeg, im) && im.width == 4 && im.height == 2, "JPEG decodes with the right size");
    check(is_red(im, 0, 0) && is_blue(im, 3, 1), "JPEG colours are right");
    check(ap::video::jpeg_exif_orientation(fixtures::kJpeg, sizeof(fixtures::kJpeg)) == 1, "plain JPEG: orientation 1");

    // A phone photo is stored sideways and tagged; it must come out upright.
    check(ap::video::jpeg_exif_orientation(fixtures::kJpegExif6, sizeof(fixtures::kJpegExif6)) == 6, "EXIF orientation 6 is read");
    check(decode(fixtures::kJpegExif6, im) && im.width == 2 && im.height == 4, "orientation 6 turns the picture 90 degrees");
    // 90 degrees clockwise: the left (red) half becomes the top.
    check(is_red(im, 0, 0) && is_red(im, 1, 1) && is_blue(im, 0, 3) && is_blue(im, 1, 2),
          "orientation 6: left half ends up on top");
    check(decode(fixtures::kJpegExif3, im) && im.width == 4 && im.height == 2, "orientation 3 keeps the size");
    check(is_blue(im, 0, 0) && is_red(im, 3, 1), "orientation 3 turns the picture 180 degrees");

    // Attacker-controlled offsets/counts in EXIF must not cause out-of-bounds reads.
    std::vector<uint8_t> bad_exif_offset(fixtures::kJpegExif6,
                                         fixtures::kJpegExif6 + sizeof(fixtures::kJpegExif6));
    std::size_t tiff_at = bad_exif_offset.size();
    for (std::size_t i = 0; i + 6 <= bad_exif_offset.size(); ++i) {
        if (std::memcmp(bad_exif_offset.data() + i, "Exif\0\0", 6) == 0) {
            tiff_at = i + 6;
            break;
        }
    }
    check(tiff_at + 10 <= bad_exif_offset.size(), "EXIF fixture contains a TIFF header");
    if (tiff_at + 10 <= bad_exif_offset.size()) {
        bad_exif_offset[tiff_at + 4] = 0xff;
        bad_exif_offset[tiff_at + 5] = 0xff;
        bad_exif_offset[tiff_at + 6] = 0xff;
        bad_exif_offset[tiff_at + 7] = 0xf0;
        check(ap::video::jpeg_exif_orientation(bad_exif_offset.data(), bad_exif_offset.size()) == 1,
              "overflowing EXIF IFD offset is rejected");
        check(decode_image_rgba(bad_exif_offset.data(), bad_exif_offset.size(), 4096, im) &&
              im.width == 4 && im.height == 2,
              "JPEG with invalid EXIF metadata still decodes without applying orientation");
    }
    check(ap::video::jpeg_exif_orientation(nullptr, 128) == 1,
          "null EXIF input is handled safely");
    std::vector<uint8_t> bad_exif_count(fixtures::kJpegExif6,
                                        fixtures::kJpegExif6 + sizeof(fixtures::kJpegExif6));
    if (tiff_at + 10 <= bad_exif_count.size()) {
        bad_exif_count[tiff_at + 8] = 0xff;
        bad_exif_count[tiff_at + 9] = 0xff;
        check(ap::video::jpeg_exif_orientation(bad_exif_count.data(), bad_exif_count.size()) == 1,
              "EXIF entry count exceeding the segment is rejected");
    }

    // A tiny compressed header must not trigger a huge decoded RGBA allocation.
    std::vector<uint8_t> oversized_bmp(54, 0);
    oversized_bmp[0] = 'B'; oversized_bmp[1] = 'M';
    auto put32le = [&](std::size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) oversized_bmp[at + i] = static_cast<uint8_t>(v >> (8 * i));
    };
    put32le(2, 54u + 75'000'000u); put32le(10, 54); put32le(14, 40);
    put32le(18, 5000); put32le(22, 5000);
    oversized_bmp[26] = 1; oversized_bmp[28] = 24;
    put32le(34, 75'000'000u);
    why.clear();
    check(!decode_image_rgba(oversized_bmp.data(), oversized_bmp.size(), 4096, im, &why) &&
          why.find("16-megapixel") != std::string::npos,
          "image exceeding the decoded pixel budget is rejected before full decode");

    // Shrinking keeps the proportions and never enlarges.
    std::vector<uint8_t> big;   // a 3000x1500 BMP built by hand (24-bit)
    {
        const int w = 3000, h = 1500, row = (w * 3 + 3) & ~3;
        const uint32_t pix = static_cast<uint32_t>(row) * h, total = 54 + pix;
        big.assign(total, 0);
        auto w32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) big[at + i] = (v >> (8 * i)) & 0xff; };
        big[0] = 'B'; big[1] = 'M'; w32(2, total); w32(10, 54); w32(14, 40); w32(18, w); w32(22, h);
        big[26] = 1; big[28] = 24; w32(34, pix);
        for (size_t i = 54; i < total; ++i) big[i] = 0x80;
    }
    check(decode_image_rgba(big.data(), big.size(), 1500, im) && im.width == 1500 && im.height == 750,
          "a 3000x1500 picture is shrunk to 1500x750 for max_side 1500");
    check(decode_image_rgba(big.data(), big.size(), 8000, im) && im.width == 3000 && im.height == 1500,
          "a picture below max_side is left alone");

    // Bad input must fail cleanly.
    const uint8_t junk[64] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    check(!decode_image_rgba(junk, sizeof(junk), 1000, im, &why) && !why.empty(), "garbage is refused with a reason");
    check(!decode_image_rgba(junk, 4, 1000, im, &why), "a tiny buffer is refused");
    check(!decode_image_rgba(nullptr, 0, 1000, im, &why), "null input is refused");
    std::vector<uint8_t> cut(fixtures::kPng, fixtures::kPng + sizeof(fixtures::kPng) / 2);
    check(!decode_image_rgba(cut.data(), cut.size(), 1000, im, &why), "a truncated PNG is refused");
    check(!ap::video::load_image_file_rgba("/definitely/not/here.png", 1000, im, &why), "a missing file is refused");

    std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}

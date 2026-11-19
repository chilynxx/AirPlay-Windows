#!/usr/bin/env python3
"""Regenerates src/video/app_logo.h from assets/winairplay-192.png."""
import pathlib
root = pathlib.Path(__file__).resolve().parent.parent
data = (root / "assets" / "winairplay-192.png").read_bytes()
rows = ["    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 24]) + "," for i in range(0, len(data), 24)]
(root / "src" / "video" / "app_logo.h").write_text(
    "#pragma once\n// The WinAirPlay logo (192x192 PNG with alpha), embedded so the interface does\n"
    "// not depend on files next to the executable. Generated from assets/winairplay-192.png:\n"
    "//   python tools/embed_logo.py\n#include <cstddef>\n\nnamespace ap::video {\n"
    "inline const unsigned char kAppLogoPng[] = {\n" + "\n".join(rows) +
    "\n};\ninline constexpr std::size_t kAppLogoPngSize = sizeof(kAppLogoPng);\n} // namespace ap::video\n")
print("wrote src/video/app_logo.h")

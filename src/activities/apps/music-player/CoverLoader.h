#pragma once

// Album cover loading: turns an ID3 APIC picture or a sibling cover file into
// a small 1-bit BMP the UI can draw straight onto the framebuffer.

#include <cstdint>
#include <string>
#include <vector>

#include "TrackTags.h"

namespace CoverLoader {

// Square edge length of the BMP produced for the now-playing screen.
constexpr int TARGET_SIZE = 400;

// Looks for a cover file next to the track (cover.jpg, folder.jpg, ...).
// Returns an empty string when none exists.
std::string findSibling(const std::string& audioPath);

// Converts an image file to a TARGET_SIZE square 1-bit BMP.
bool fileToMonoBmp(const char* path, bool isPng, std::vector<uint8_t>& out);

// Writes the embedded APIC bytes to a temp file, then converts it.
bool embeddedToMonoBmp(const EmbeddedArt& art, std::vector<uint8_t>& out);

}  // namespace CoverLoader

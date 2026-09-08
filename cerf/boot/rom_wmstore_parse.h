#pragma once

#include "rom_image_parse.h"

#include <cstdint>
#include <span>

namespace cerf::rom_image_parse {

constexpr uint8_t kEscoZipLocalSignature[4] = {'P', 'K', 0x03, 0x04};
constexpr uint8_t kWmstoreSignature[8] =
    {'_', 'w', 'm', 's', 't', 'o', 'r', 'e'};
constexpr uint8_t kWmpartSignature[8] =
    {'_', 'w', 'm', 'p', 'a', 'r', 't', '_'};

/* PKWARE APPNOTE.TXT 6.3.10 §4.3.7 local file header: compression method at
   +8, file name length at +26, extra field length at +28, fixed part 30
   bytes; §4.4.5 method 0 = stored; §4.3.8 puts the file data immediately
   after the header, the name and the extra field. */
constexpr size_t   kZipLocalHeaderSize = 30u;
constexpr size_t   kZipMethodOff       = 8u;
constexpr size_t   kZipNameLenOff      = 26u;
constexpr size_t   kZipExtraLenOff     = 28u;
constexpr uint16_t kZipMethodStore     = 0u;

constexpr size_t   kWmstoreSuperblockOff   = 0x200u;
constexpr size_t   kWmstorePartTableOff    = 0x400u;
constexpr size_t   kWmstorePartEntrySize   = 0x200u;
constexpr size_t   kWmstorePartNameOff     = 0x08u;
constexpr size_t   kWmstorePartNameChars   = 0x20u;
constexpr size_t   kWmstorePartStartLbaOff = 0x4Cu;
constexpr size_t   kWmstorePartSizeLbaOff  = 0x54u;
constexpr uint32_t kWmstoreSectorBytes     = 512u;

struct WmstoreOsXip {
    size_t   data_off  = 0;
    uint32_t flat_size = 0;
    uint32_t base_va   = 0;
};

bool WmstoreLocateOsXip(std::span<const uint8_t> raw, WmstoreOsXip& out);

}  /* namespace cerf::rom_image_parse */

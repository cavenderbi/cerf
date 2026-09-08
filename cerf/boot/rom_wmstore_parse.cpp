#include "rom_wmstore_parse.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace cerf::rom_image_parse {

namespace {

size_t EscoPayloadOffset(std::span<const uint8_t> raw) {
    if (raw.size() < kZipLocalHeaderSize) return 0;
    if (std::memcmp(raw.data(), kEscoZipLocalSignature,
                    sizeof(kEscoZipLocalSignature)) != 0)
        return 0;
    if (U16(raw.data(), kZipMethodOff) != kZipMethodStore) return SIZE_MAX;
    const size_t off = kZipLocalHeaderSize
                     + size_t(U16(raw.data(), kZipNameLenOff))
                     + size_t(U16(raw.data(), kZipExtraLenOff));
    return (off < raw.size()) ? off : SIZE_MAX;
}

std::string PartitionName(const uint8_t* entry) {
    std::string name;
    for (size_t i = 0; i < kWmstorePartNameChars; ++i) {
        const uint16_t c = U16(entry, kWmstorePartNameOff + i * 2u);
        if (c == 0) break;
        if (c > 0x7Fu) return {};
        name.push_back(char(c));
    }
    return name;
}

}  /* namespace */

bool WmstoreLocateOsXip(std::span<const uint8_t> raw, WmstoreOsXip& out) {
    const size_t payload = EscoPayloadOffset(raw);
    if (payload == SIZE_MAX) return false;

    if (payload + kWmstoreSuperblockOff + sizeof(kWmstoreSignature) > raw.size())
        return false;
    if (std::memcmp(raw.data() + payload + kWmstoreSuperblockOff,
                    kWmstoreSignature, sizeof(kWmstoreSignature)) != 0)
        return false;

    for (size_t i = 0;; ++i) {
        const size_t entry_off =
            payload + kWmstorePartTableOff + i * kWmstorePartEntrySize;
        if (entry_off + kWmstorePartEntrySize > raw.size()) break;

        const uint8_t* entry = raw.data() + entry_off;
        if (std::memcmp(entry, kWmpartSignature, sizeof(kWmpartSignature)) != 0)
            break;
        if (PartitionName(entry) != "NK") continue;

        const uint64_t start_off =
            uint64_t(payload)
            + uint64_t(U32(entry, kWmstorePartStartLbaOff)) * kWmstoreSectorBytes;
        const uint64_t part_bytes =
            uint64_t(U32(entry, kWmstorePartSizeLbaOff)) * kWmstoreSectorBytes;
        if (start_off + kRomSignatureOffset + 8u > raw.size()) return false;

        const size_t avail = size_t(std::min<uint64_t>(
            part_bytes, uint64_t(raw.size()) - start_off));
        std::span<const uint8_t> xip = raw.subspan(size_t(start_off), avail);

        if (xip.size() < kRomSignatureOffset + 12u) return false;
        if (U32(xip.data(), kRomSignatureOffset) != kRomSignature) return false;

        const uint32_t ptoc_va    = U32(xip.data(), kRomSignatureOffset + 4u);
        const uint32_t romhdr_off = U32(xip.data(), kRomSignatureOffset + 8u);

        ParsedROMHDR hdr;
        if (!ParseRomHdr(xip, romhdr_off, hdr)) return false;
        if (uint64_t(hdr.physfirst) + romhdr_off != ptoc_va) return false;
        if (hdr.physlast <= hdr.physfirst) return false;

        const uint32_t flat_size = hdr.physlast - hdr.physfirst;
        if (uint64_t(flat_size) > xip.size()) return false;

        out.data_off  = size_t(start_off);
        out.flat_size = flat_size;
        out.base_va   = hdr.physfirst;
        return true;
    }
    return false;
}

}  /* namespace cerf::rom_image_parse */

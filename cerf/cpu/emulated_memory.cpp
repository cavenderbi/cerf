#include "emulated_memory.h"

#include "../core/cerf_emulator.h"
#include "../core/fatal.h"
#include "../core/log.h"
#include "../boards/page_table_builder.h"
#include "../state/state_stream.h"

#include <cstring>

REGISTER_SERVICE(EmulatedMemory);

void EmulatedMemory::OnReady() {
    auto& page = emu_.Get<PageTableBuilder>();
    for (const auto& r : page.BackedMemoryRegions()) {
        AddRegion(r.pa_base, r.size, r.page_protect, r.decode_span);
    }
}

void EmulatedMemory::AddRegion(uint32_t base, uint32_t size,
                               DWORD page_protect, uint32_t decode_span) {
    std::lock_guard<std::mutex> lk(add_mutex_);

    const uint32_t span = decode_span ? decode_span : size;
    if (size == 0 || span < size || uint64_t(base) + span > (uint64_t{1} << 32) ||
        span % size != 0 ||
        (span != size && (size & (size - 1u)) != 0)) {
        LOG(Caution, "EmulatedMemory::AddRegion invalid region: base=0x%08X "
                "size=0x%X span=0x%X\n", base, size, span);
        CerfFatalExit(CERF_FATAL_RUNTIME_ERROR);
    }
    const uint32_t wrap_mask = (span == size) ? 0xFFFFFFFFu : (size - 1u);

    /* count_.load(acquire) inside the writer lock - we hold the writer
       lock, so no other writer can be racing; the acquire pairs with
       earlier store_release publications. */
    const size_t n = count_.load(std::memory_order_acquire);

    for (size_t i = 0; i < n; ++i) {
        const Region& r = regions_[i];
        if (uint64_t(base) < uint64_t(r.base) + r.span &&
            uint64_t(r.base) < uint64_t(base) + span) {
            LOG(Caution, "EmulatedMemory::AddRegion overlap: new "
                    "[0x%08X..0x%llX) vs existing [0x%08X..0x%llX)\n",
                    base, static_cast<unsigned long long>(base) + span,
                    r.base, static_cast<unsigned long long>(r.base) + r.span);
            CerfFatalExit(CERF_FATAL_RUNTIME_ERROR);
        }
    }

    if (n >= kMaxRegions) {
        LOG(Caution, "EmulatedMemory::AddRegion: kMaxRegions=%zu exceeded "
                "(new [0x%08X..0x%08X)). Bump kMaxRegions in "
                "emulated_memory.h.\n",
                kMaxRegions, base, base + size);
        CerfFatalExit(CERF_FATAL_RUNTIME_ERROR);
    }

    regions_[n].base         = base;
    regions_[n].size         = size;
    regions_[n].span         = span;
    regions_[n].wrap_mask    = wrap_mask;
    regions_[n].page_protect = page_protect;

    count_.store(n + 1, std::memory_order_release);

    LOG(Mem, "AddRegion 0x%08X size 0x%X span 0x%X protect 0x%X (slot %zu)\n",
        base, size, span, page_protect, n);
}

EmulatedMemory::Region* EmulatedMemory::FindRegion(uint32_t vaddr) {
    const size_t n = count_.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
        Region& r = regions_[i];
        if (vaddr >= r.base && vaddr - r.base < r.span) {
            return &r;
        }
    }
    return nullptr;
}

uint8_t* EmulatedMemory::EnsureBacked(Region* r) {
    /* Fast path - already backed. memory_order_acquire so reads of the
       host page that follow this load see the VirtualAlloc memory. */
    if (uint8_t* ptr = r->host_ptr.load(std::memory_order_acquire)) {
        return ptr;
    }

    uint8_t* allocated = static_cast<uint8_t*>(::VirtualAlloc(
        nullptr, r->size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!allocated) {
        LOG(Caution, "EmulatedMemory::EnsureBacked VirtualAlloc failed "
                "region 0x%08X size 0x%X (GLE=%lu)\n",
                r->base, r->size, ::GetLastError());
        CerfFatalExit(CERF_FATAL_RUNTIME_ERROR);
    }

    uint8_t* expected = nullptr;
    if (r->host_ptr.compare_exchange_strong(
            expected, allocated,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        LOG(Mem, "Region 0x%08X backed at host %p\n", r->base, allocated);
        return allocated;
    }

    /* Lost the race. expected now holds the winner's pointer. Free
       our orphan allocation and return the winner. */
    ::VirtualFree(allocated, 0, MEM_RELEASE);
    return expected;
}

uint8_t* EmulatedMemory::Translate(uint32_t vaddr) {
    Region* r = FindRegion(vaddr);
    if (!r)
        emu_.Get<Fatal>().Die("EmulatedMemory::Translate unmapped 0x%08X", vaddr);
    return EnsureBacked(r) + ((vaddr - r->base) & r->wrap_mask);
}

uint8_t* EmulatedMemory::TryTranslate(uint32_t paddr) {
    Region* r = FindRegion(paddr);
    if (!r) return nullptr;
    return EnsureBacked(r) + ((paddr - r->base) & r->wrap_mask);
}

uint8_t* EmulatedMemory::TryTranslateWrite(uint32_t paddr) {
    Region* r = FindRegion(paddr);
    if (!r) {
        return nullptr;
    }

    /* PAGE_READONLY / PAGE_EXECUTE_READ → flash / ROM region. Write
       must dispatch to the flash controller / I/O peripheral instead
       of caching a host pointer; signal that to the walker by
       returning nullptr. */
    if (r->page_protect == PAGE_READONLY ||
        r->page_protect == PAGE_EXECUTE_READ) {
        return nullptr;
    }

    return EnsureBacked(r) + ((paddr - r->base) & r->wrap_mask);
}

uint8_t* EmulatedMemory::TryTranslateRange(uint64_t paddr, uint64_t size, bool write) {
    if (size == 0 || paddr > UINT32_MAX || size > (uint64_t{1} << 32) - paddr)
        return nullptr;
    Region* r = FindRegion(static_cast<uint32_t>(paddr));
    if (!r || (write && (r->page_protect == PAGE_READONLY ||
                        r->page_protect == PAGE_EXECUTE_READ)))
        return nullptr;
    const uint64_t offset = paddr - r->base;
    const uint64_t backed_offset = offset & r->wrap_mask;
    if (size > r->span - offset || size > r->size - backed_offset)
        return nullptr;
    return EnsureBacked(r) + static_cast<size_t>(backed_offset);
}

bool EmulatedMemory::IsSlotRangeUniform(uint32_t span_bytes, uint32_t pa) {
    if (span_bytes <= 0x1000u) return true;
    const uint32_t base = pa & ~(span_bytes - 1u);
    Region* r = FindRegion(base);
    if (!r) return true;
    if (r->wrap_mask != 0xFFFFFFFFu) return false;
    return base >= r->base &&
           static_cast<uint64_t>(base) + span_bytes <=
               static_cast<uint64_t>(r->base) + r->size;
}

uint8_t EmulatedMemory::ReadByte(uint32_t vaddr) {
    return *Translate(vaddr);
}

uint16_t EmulatedMemory::ReadHalf(uint32_t vaddr) {
    uint16_t v;
    std::memcpy(&v, Translate(vaddr), sizeof(v));
    return v;
}

uint32_t EmulatedMemory::ReadWord(uint32_t vaddr) {
    uint32_t v;
    std::memcpy(&v, Translate(vaddr), sizeof(v));
    return v;
}

uint64_t EmulatedMemory::ReadDword(uint32_t vaddr) {
    uint64_t v;
    std::memcpy(&v, Translate(vaddr), sizeof(v));
    return v;
}

void EmulatedMemory::WriteByte(uint32_t vaddr, uint8_t value) {
    *Translate(vaddr) = value;
}

void EmulatedMemory::WriteHalf(uint32_t vaddr, uint16_t value) {
    std::memcpy(Translate(vaddr), &value, sizeof(value));
}

void EmulatedMemory::WriteWord(uint32_t vaddr, uint32_t value) {
    std::memcpy(Translate(vaddr), &value, sizeof(value));
}

void EmulatedMemory::WriteDword(uint32_t vaddr, uint64_t value) {
    std::memcpy(Translate(vaddr), &value, sizeof(value));
}

EmulatedMemory::Region* EmulatedMemory::BulkRegionFor(uint32_t vaddr,
                                                      size_t size,
                                                      const char* op) {
    Region* r = FindRegion(vaddr);
    if (!r) {
        LOG(Caution, "EmulatedMemory::%s unmapped address "
                "0x%08X size 0x%zX\n", op, vaddr, size);
        CerfFatalExit(CERF_FATAL_RUNTIME_ERROR);
    }
    uint64_t end = static_cast<uint64_t>(vaddr) + size;
    if (end > static_cast<uint64_t>(r->base) + r->size) {
        LOG(Caution, "EmulatedMemory::%s crosses region boundary at "
                "0x%08X size 0x%zX (region 0x%08X size 0x%X)\n",
                op, vaddr, size, r->base, r->size);
        CerfFatalExit(CERF_FATAL_RUNTIME_ERROR);
    }
    return r;
}

void EmulatedMemory::CopyIn(uint32_t vaddr, const void* host_src, size_t size) {
    Region* r = BulkRegionFor(vaddr, size, "CopyIn");
    uint8_t* host = EnsureBacked(r);
    std::memcpy(host + (vaddr - r->base), host_src, size);
}

void EmulatedMemory::CopyOut(uint32_t vaddr, void* host_dst, size_t size) {
    Region* r = BulkRegionFor(vaddr, size, "CopyOut");
    uint8_t* host = EnsureBacked(r);
    std::memcpy(host_dst, host + (vaddr - r->base), size);
}

void EmulatedMemory::WipeVolatileRegions() {
    const size_t n = count_.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
        Region& r = regions_[i];
        if (r.page_protect == PAGE_READONLY ||
            r.page_protect == PAGE_EXECUTE_READ) {
            continue;
        }
        /* Unbacked regions are already zero on first touch. */
        if (uint8_t* host = r.host_ptr.load(std::memory_order_acquire)) {
            std::memset(host, 0, r.size);
            LOG(Mem, "WipeVolatileRegions: region 0x%08X size 0x%X zeroed\n",
                r.base, r.size);
        }
    }
}

void EmulatedMemory::SaveState(StateWriter& w) {
    const size_t n = count_.load(std::memory_order_acquire);
    uint32_t volatile_count = 0;
    for (size_t i = 0; i < n; ++i) {
        const Region& r = regions_[i];
        if (r.page_protect == PAGE_READONLY || r.page_protect == PAGE_EXECUTE_READ)
            continue;
        ++volatile_count;
    }
    w.Write(volatile_count);
    for (size_t i = 0; i < n; ++i) {
        Region& r = regions_[i];
        if (r.page_protect == PAGE_READONLY || r.page_protect == PAGE_EXECUTE_READ)
            continue;
        w.Write(r.base);
        w.Write(r.size);
        uint8_t* host = r.host_ptr.load(std::memory_order_acquire);
        const uint8_t backed = host ? 1u : 0u;
        w.Write(backed);
        if (host) w.WriteBytes(host, r.size);
    }
}

void EmulatedMemory::SaveFlashRegions(StateWriter& w) {
    const size_t n = count_.load(std::memory_order_acquire);
    uint32_t flash_count = 0;
    for (size_t i = 0; i < n; ++i) {
        const Region& r = regions_[i];
        if (r.page_protect != PAGE_READONLY && r.page_protect != PAGE_EXECUTE_READ)
            continue;
        if (r.host_ptr.load(std::memory_order_acquire) == nullptr)
            continue;   /* never touched: nothing programmed, the ROM repopulates it */
        ++flash_count;
    }
    w.Write(flash_count);
    for (size_t i = 0; i < n; ++i) {
        Region& r = regions_[i];
        if (r.page_protect != PAGE_READONLY && r.page_protect != PAGE_EXECUTE_READ)
            continue;
        uint8_t* host = r.host_ptr.load(std::memory_order_acquire);
        if (!host) continue;
        w.Write(r.base);
        w.Write(r.size);
        w.WriteBytes(host, r.size);
    }
}

void EmulatedMemory::RestoreFlashRegions(StateReader& r) {
    uint32_t flash_count = 0;
    r.Read(flash_count);
    for (uint32_t k = 0; k < flash_count; ++k) {
        uint32_t base = 0, size = 0;
        r.Read(base);
        r.Read(size);
        if (!r.Ok()) return;
        Region* reg = FindRegion(base);
        if (!reg || reg->base != base || reg->size != size ||
            (reg->page_protect != PAGE_READONLY &&
             reg->page_protect != PAGE_EXECUTE_READ)) {
            LOG(Caution, "EmulatedMemory::RestoreFlashRegions: region mismatch "
                "base=0x%08X size=0x%X - image incompatible with memory map\n",
                base, size);
            CerfFatalExit(CERF_FATAL_RUNTIME_ERROR);
        }
        r.ReadBytes(EnsureBacked(reg), size);
    }
}

uint64_t EmulatedMemory::VolatileByteCount() const {
    const size_t n = count_.load(std::memory_order_acquire);
    uint64_t total = 0;
    for (size_t i = 0; i < n; ++i) {
        const Region& r = regions_[i];
        if (r.page_protect == PAGE_READONLY || r.page_protect == PAGE_EXECUTE_READ)
            continue;
        total += r.size;
    }
    return total;
}

void EmulatedMemory::RestoreState(StateReader& r) {
    uint32_t volatile_count = 0;
    r.Read(volatile_count);
    for (uint32_t k = 0; k < volatile_count; ++k) {
        uint32_t base = 0, size = 0;
        uint8_t  backed = 0;
        r.Read(base);
        r.Read(size);
        r.Read(backed);
        if (!r.Ok()) return;
        Region* reg = FindRegion(base);
        if (!reg || reg->base != base || reg->size != size ||
            reg->page_protect == PAGE_READONLY ||
            reg->page_protect == PAGE_EXECUTE_READ) {
            LOG(Caution, "EmulatedMemory::RestoreState: region mismatch "
                "base=0x%08X size=0x%X - image incompatible with memory map\n",
                base, size);
            CerfFatalExit(CERF_FATAL_RUNTIME_ERROR);
        }
        if (backed) {
            r.ReadBytes(EnsureBacked(reg), size);
        } else if (uint8_t* host = reg->host_ptr.load(std::memory_order_acquire)) {
            std::memset(host, 0, size);
        }
    }
}

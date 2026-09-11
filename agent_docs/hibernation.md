# Hibernation - full machine-state save / restore

CERF can snapshot a running guest to a `.img` file and later resume it bit-for-bit
("hibernation" / "state saver"). This page is the **contract every peripheral
author must obey**, so a restore returns a live, correct machine instead of
a half-reset one. If you create or modify ANY peripheral, SoC block, board device,
codec, or worker thread, the § "Peripheral contract" rules below are mandatory.
A skipped rule does not produce a smaller feature. It produces a broken restore
(dead display, frozen scheduler, missed interrupts) that appears hours later on a
different device.

Host-side implementation: `cerf/state/` (`hibernation.{h,cpp}`,
`state_stream.h`, `state_image_format.h`, `state_boot_gate.cpp`,
`emulation_freeze.h`, `shutdown_dialog.cpp`). The JIT pause lives in
`cerf/jit/jit_runner.{h,cpp}`.

## What it is

Three outcomes from a saved `.img`:

- **Full restore** - resume the exact desktop (every register, all RAM, all
  peripheral state) as if the machine never stopped.
- **Warm boot** - keep RAM + flash, re-init CPU / cp15 / peripherals cold. The OS
  reboots, but the filesystem-in-RAM and any flash writes survive.
- **Cold boot** - ignore the image, boot from scratch.

**Why it exists:** real vintage HPC devices (Jornada 720, …) are battery-backed and
keep DRAM alive across suspend. Only a battery pull wipes it. The running OS
and its RAM-backed object store (the CE in-memory filesystem) therefore persist
across power cycles. CERF instead wipes all guest RAM on close. Every exit is
effectively a cold boot, and all guest state from since boot is lost. Hibernation
preserves the full running machine across an exit.

**Triggers:** the Actions-menu Save/Load state, a Shutdown dialog on window close
(save-on-exit), and a boot-time prompt when a default `state.img` exists in the
device directory.

## The `.img` format

`StateImageHeader` (magic `CERFIMG1`, `format_version`, ROM fingerprint =
`rom_entry_va` + `rom_total_bytes` + `periph_layout_sig` + a `guest_additions`
byte), followed by a section count and length-framed sections
(`StateSectionHeader{ id, length }`). The length frame lets restore **skip** a
section that it does not apply (warm boot). It also lets restore tolerate a
peripheral whose Save/Restore are asymmetric, with no desync of the whole stream.
`SeekTo(body_start + length)` re-aligns after every section.

`ValidateHeader` verifies identity **before** CERF mutates any live state. A wrong
ROM, a wrong peripheral layout signature, or a guest-additions mismatch is refused
at that point. `periph_layout_sig` is a hash over the registered peripheral set
(count + each `MmioBase()`). CERF therefore rejects a cross-build-incompatible
image instead of a mis-apply. Identity comes from `RomParserService`.

## Build-specific by design

CERF state images are **build-specific**. Only the exact binary that wrote an
`.img` ever restores it. `ValidateHeader` enforces this at load.
`periph_layout_sig` (named above), the ROM fingerprint, and `format_version`
work together. They cause CERF to **refuse, never mis-apply**, any image that
does not align with the peripheral set of the running build. There is deliberately no
per-peripheral image versioning. At the chip/board count of CERF, such versioning
is intractable.

The consequence for the peripheral contract: the ONLY serialization requirement is
that the `SaveState` and `RestoreState` of a peripheral are **exact mirrors of each
other in the same build** (a clean round-trip). Cross-build `.img` compatibility is
**not** a requirement. Never engineer for it. During bring-up a peripheral can grow
its `SaveState` (new registers), reorder fields, or drop a field that it no longer
has. It can also move onto a shared core that serializes in a different order.
Such a peripheral does nothing wrong. `ValidateHeader` refuses the older images that used
the previous format, exactly as intended.

## Sections - what each captures

CERF saves and restores in file order: **Cpu → Mmu → Ram → Flash → Periph →
Presentation → Widget → Reset**.

- **Cpu** - the flat CPU-state POD of the engine, through the ISA-neutral
  `GuestEngine::SaveCpuState` / `RestoreCpuState` seam. The ARM engine
  serializes `ArmCpuState` (GPRs, `guest_cycle_counter`, CPSR, banked regs,
  `irq_interrupt_pending`). The MIPS engine serializes `MipsCpuState`.
- **Mmu** - the persistent MMU state of the engine, through
  `GuestEngine::SaveMmuState` / `RestoreMmuState`. ARM: cp15 persistent register
  fields only. The TLBs and SMC bitmaps are derived state. Restore flushes them
  (`ArmTlbFlushAll`) and never serializes them. MIPS: the `MipsMmu` residual state.
- **Ram** - `EmulatedMemory` volatile (PAGE_READWRITE) regions.
- **Flash** - `EmulatedMemory` backed PAGE_READONLY / PAGE_EXECUTE_READ regions
  (flash writes-since-boot ARE machine state). Restore applies this section on
  warm boot too, because flash survives a reboot on real hardware.
- **Periph** - every `PeripheralDispatcher::RegisteredPeripherals()` entry, tagged
  with its `MmioBase()`. The save pass and the restore pass walk the set in the same
  order. The restore pass verifies the tag.
- **Presentation** - `HostCanvas` guest-surface dimensions, so a custom resolution
  restores its window size.
- **Widget** - `HostWidgetRegistry` state that drives guest-visible hardware
  (for example the charge level / AC of the battery widget, which a board service
  feeds into GPIO/MCU lines). Full restore only. On a warm boot the board service
  re-asserts it when it re-drives at startup.
- **Reset** - `GuestCpuReset` + `GuestColdBoot` state (reset cause, registered
  boot-time guest-RAM write replays).

CERF **flushes** the JIT translation cache and never saves it
(`FlushTranslationCache()`). Interrupt delivery re-arms from the restored
state: each engine's `Run` re-reads the live interrupt line every
iteration, and the INTC's `PostRestore` re-drives that line. An INTC
without the `PostRestore` re-drive silently drops a restored-pending IRQ.

## The two-thread freeze model - read before touching ANY peripheral

A safe snapshot requires a quiescent guest. Two things execute guest-visible
state, and a pause of one does NOT pause the other:

1. **The JIT (guest CPU) thread.** `JitRunner::Pause()` parks it between translated
   blocks. `Resume()` releases it. `Pause()` is **host-thread only**. A call from
   the JIT thread self-deadlocks. Hibernation runs `Pause() → work → Resume()`.

2. **Peripheral worker threads.** A peripheral that owns a `std::thread` continues
   to mutate guest-visible state, whatever the JIT pause does. **`EmulationFreeze`**
   (`cerf/state/emulation_freeze.h`) freezes them:
   - A worker holds `WorkerSection()` (a `shared_lock`) **around the part of each
     iteration that reads or writes guest state**.
   - Hibernation holds `SnapshotSection()` (a `unique_lock`) across the whole
     save or restore.
   - **Lock-order invariant (a violation deadlocks):** take the freeze lock
     BEFORE any peripheral mutex. A worker **never** holds `WorkerSection`
     across a cv wait / sleep / thread join. Acquire it, do the state touch,
     release it, then wait.

`VirtualTimerList::ExpiryLoop` (`cerf/core/virtual_timer_list.cpp`) wraps its
`RunExpired()` pass in a worker section. Every peripheral that owns a worker
thread does the same. A peripheral whose state advances only on the JIT thread
has no worker and needs no `WorkerSection`.

## The peripheral contract - MANDATORY when you create or modify a peripheral

A `Peripheral` (or any object that holds mutable guest-visible state) gets a
maximum of three methods. **Every peripheral with mutable state MUST implement the
first two.**

### 1. `SaveState(StateWriter&)` / `RestoreState(StateReader&)`

Serialize **every mutable register / latch / counter / FIFO**, not only an obvious
`storage_[]` array: timer counters, DMA transfer registers, RTC base, LCD
framebuffer configuration, blit-engine latched-op params, FIFO contents,
mode/command FSM latches. Save and Restore must be **exact mirrors** (same field
order). Use `#include "../../state/state_stream.h"` with `w.Write<T>()` /
`r.Read<T>()`. Length-prefix variable-size data (member-vector NAND/NOR, FIFOs),
so forward-skip stays valid. For `std::atomic<uintN>`, use
`.load(std::memory_order_acquire)` / `.store(v, std::memory_order_release)`.

### 2. `PostRestore()`

It runs in a **second pass, after the `RestoreState` of every peripheral is
complete** (`Hibernation::RestorePeripherals`). It is therefore order-independent,
because all registers are already in place. Use it to re-assert **computed** state
that a single `RestoreState` cannot establish. The interrupt line
`source → INTC → JIT` is the most important case:

- An **INTC** re-notifies the JIT of its restored pending/mask state
  (`SetInterruptPending` / re-derive `HasPendingUnmasked`). A restored INTC that
  only reloads its registers never re-arms the JIT → missed or stale IRQ → hang.
- A **level-driving source** (GPIO edge/level lines, OST match level, an SA-1111
  cascade) re-drives its INTC source level. See `sa11xx_intc` (`NotifyLocked`),
  `os_timer` (`PushMatchLevel` from `PostRestore`), `sa11xx_gpio`
  (`PublishEdgeSourcesLocked`), `sa1111_intc` (`DriveCascadeOutput`).

`PostRestore` is a no-op default in `peripheral_base.h`. Override it only when you
own a computed line. **Fix the whole bug class, not one instance.** If one INTC
needs `PostRestore`, audit every INTC.

### 3. Worker-thread wrapping

If your peripheral starts a worker thread that touches guest state, wrap the
state touch in `emu_.Get<EmulationFreeze>().WorkerSection()`, per § The two-thread
freeze model.

## Patterns by peripheral shape

- **Unified peripheral** (`class Foo : public Peripheral` that holds its own regs)
  - SaveState/RestoreState directly on the class.
- **Split peripheral** (a stateless MMIO `Peripheral` that delegates to a
  state-owner registered `_AS` a base, for example the S3C2410 INTC) - the
  **state-owner** implements SaveState/RestoreState/PostRestore. The MMIO override
  delegates through `static_cast<Owner&>(emu_.Get<Base>())`. **Read BOTH `.h` and
  `.cpp`** before you call a peripheral a gap. Save/Restore is often inline in the
  header.
- **Codec / PMIC parts are Services, NOT Peripherals** → they are not in the
  `RegisteredPeripherals()` walk. Add virtual `SaveState/RestoreState` to the codec
  base (`Ac97Codec`, `Sa11xxMcpCodec`). Override it in the concrete and serialize
  its `reg_`. Then **forward from the SaveState/RestoreState of the owning
  peripheral** with `if (auto* c = emu_.TryGet<Base>()) c->SaveState(w);`
  (symmetric in Save+Restore).
- **Non-`Peripheral` stateful objects** (PCMCIA `PcmciaSlot` / `PcmciaCard`,
  sub-devices like a companion-ASIC `Ps2Mouse`) are not auto-enumerated → they need
  an explicit serialization walk + card-presence recreation
  (`PcmciaCardCatalog::Create(id, binding)`).
- **Rebase timers** - timers anchored to a guest-cycle baseline, to a
  virtual-clock-ns counter, or to a wall clock - **never
  raw-serialize a `std::chrono::time_point` or a guest-cycle baseline.** Save
  the live counter. On restore, re-anchor the baseline so the counter resumes
  continuously. Guest-cycle → `baseline = (saved_count, GuestCycles())`, and
  per-channel match anchors stay valid (same counter domain). A
  virtual-clock-ns timer saves a computed live counter, because CERF does
  not serialize `VirtualClock`. Wall-clock →
  `period_start_ = Clock::now()`.
- **In-flight host coupling** resets on restore, because no host sink / pen /
  socket exists after a restore. In RestoreState or PostRestore, clear audio-DMA
  `in_flight`/`tx_running`, touch `pen_down`/`pen_timer_enabled`, and the
  equivalent flags. See `sa11xx_dma`, `sa1111_sac`, `odo_arm720_touch_sound`.

## What NOT to serialize (host-side members)

Skip anything that is host machinery, not guest state. CERF reconstructs it and
never restores it: raw host pointers, `std::thread`, `HANDLE`, audio sinks,
`std::function`, pointers into the own buffer of the object,
`std::string`/`std::vector` TX-line accumulators. A **file-backed `DiskImage`**
(host `HANDLE`) is NOT serialized. It persists on host disk across the restart.
Only its in-flight transfer state (`AtaDrive`) is serialized.

## Verifying a peripheral's serialization

A peripheral that compiles is not a peripheral that serializes. For completeness,
read every `public Peripheral` (both `.h` and `.cpp`) and verify that it has
SaveState/RestoreState. Then do a **save → restore round-trip on the actual
device** and exercise it. A single clean round-trip on one OS does not prove
restore correct. Per-device runtime testing does.

## Common failure shapes

- A peripheral resets on restore (its registers reload but a `PostRestore`-computed
  line / cleared in-flight flag is missing) → dead display or frozen scheduler.
- A worker thread mutates state mid-snapshot (missing `WorkerSection`) → torn,
  inconsistent image.
- A rebase timer raw-saved its baseline → the clock jumps or stalls on restore.
- An INTC reloaded registers but never re-notified the JIT (missing `PostRestore`)
  → a pending IRQ is lost and the guest hangs.

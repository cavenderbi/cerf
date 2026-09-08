# Features

What CERF can do beyond booting the ROM. Each of these has its own controls in the running
window - the status bar, the Actions menu, or a command-line flag.

## Guest Additions

Windows CE devices shipped with the display driver their maker wrote, and nothing else. CERF can
replace it: with **Guest Additions** enabled, CERF injects its own display driver into the ROM as
it loads, and the guest gains things it was never built to have.

- **Any resolution, live.** Run a handheld that shipped with a 640x240 screen at whatever size you
  want, and resize the window while the guest is running - the desktop follows.
- **Host mouse and keyboard.** A real pointer instead of a stylus - and on a device that never had
  a pointer at all, a pointer where there was none. The same goes the other way: a device with no
  keyboard gets one.
- **Accelerated drawing.** Blits are handed to the host instead of being drawn in software by the
  guest, so the UI keeps up at resolutions the hardware never saw.
- **Shared folders.** A folder on your PC appears inside the guest as a storage card, so files
  move both ways with no cables and no disk images.
- **Task manager.** List, switch to and kill guest processes - and start new ones - from the host.
- **Networking on any board.** A virtual Ethernet adapter appears and reaches the internet even on boards which lack corresponding interfaces.

One driver covers Windows CE 2.0 through Windows CE 7, including Pocket PC and Windows Mobile.

!!! warning "Experimental"

    Guest Additions modify the ROM as it loads. Some ROMs do not survive it, and some behave
    oddly. It is off by default, and the launcher lists the boards where it is known to cause
    trouble. If a device misbehaves, turn it off first.

## PC Cards and serial ports

The PC Card slot is where these devices got everything they did not ship with - a network card, a
modem, storage, a display adapter. CERF emulates the cards themselves, so the guest's own drivers
find them, bind them and drive them.

Cards go in and come out from the status bar, while the guest is running.

- **NE2000 Ethernet (RTL8019).** The network card the Windows CE era was built around. The guest
  gets an address and its own TCP/IP stack goes online - browser, FTP, sockets, all of it.
- **CompactFlash storage.** Point CERF at a folder and it builds a FAT16 or FAT32 card from it, or
  mount an image you already have. The guest sees a storage card.
- **Serial Port Forwarder.** Bridges the guest's serial port to a real COM port on your PC - bytes,
  baud rate and control lines pass straight through. Pair it with a virtual COM pair and the guest
  can talk to anything on the host, including a desktop running ActiveSync.
- **Serial Modem.** A modem that answers the guest's `AT` commands, takes the call and speaks PPP -
  so dial-up networking works over an emulated phone line, and the guest reaches the internet the
  way it did in 1999.
- **HP Palmtop VGA (F1252A).** The external display card, for the boards that used one.

Boards with a built-in serial port expose it the same way, without using the card slot.

## Save and restore

CERF can snapshot a running machine and bring it back exactly as it was - the CPU, the MMU, all of
RAM, the flash contents, every peripheral's registers, and the window itself.

- **Save state / Load state** from the Actions menu, at any time.
- **On close**, CERF offers to save the machine first, so the running desktop survives the exit.
- **On start**, if a saved machine is present, you choose what to do with it: **restore** it
  exactly, **warm boot** it - keep RAM and flash but let the OS reboot, so the files the guest
  wrote are still there - or **cold boot** and start from scratch.

Guests that suspend themselves are handled too: when the OS powers the device down, CERF halts the
virtual CPU and can wake it, and the guest resumes at the instruction it slept on.

!!! note

    A saved machine belongs to the CERF build that wrote it. A newer CERF refuses an older file
    rather than restoring it incorrectly, and there is no migration between versions - nor will
    there be. A snapshot is the exact state of nearly three hundred emulated components and a few
    thousand hardware registers; carrying that across versions would mean writing, and then
    maintaining forever, a bespoke migration for every one of them. Save states are for resuming
    the machine you are working with, not for archiving it.

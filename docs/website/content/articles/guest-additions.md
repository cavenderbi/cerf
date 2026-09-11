# Guest Additions

Guest Additions is a very complex guest driver, that covers every Windows CE version starting from 2.0 and up to Windows Embedded Compact 2013.

## Video driver

The Guest Additions driver is literally a video driver. It replaces the stock one with a very small stub. That stub manual-maps the big real driver, which it receives over a virtual I/O channel.

- **Resolution**: Supports up to 4K resolution on **all** devices. Can live resize since CE 4.
- **Input**: mouse and keyboard are directly passed as the native devices into the guest. This can conflict - check the status bar tools to configure
- **Accelerated drawing**: Guest Additions driver draws only exclusively through the host process. This is the hardware acceleration for guests.
- **Shared folders.** A folder on your PC appears inside the guest as a storage card. ``\CERF Storage\*``
- **Task manager.** List, switch to and kill guest processes - and start new ones - from the host.
- **Networking**. A virtual Ethernet adapter appears and reaches the internet even on boards which lack corresponding interfaces.

## What it looks like

Windows CE 2.0 on a Handheld PC, at a width the hardware never had. The panel is 8bpp, so the
wallpaper dithers.

![Windows CE 2.0 setup wizard across a wide desktop](/assets/articles/guest-additions/ce2-hpc-setup-wide.png)

![Windows CE 2.0 desktop and Start menu across a wide desktop](/assets/articles/guest-additions/ce2-hpc-desktop-wide.png)

A Palm-size PC on CE 2.11, in Japanese.

![Palm-size PC setup wizard on CE 2.11](/assets/articles/guest-additions/ce211-palmsize-setup.png)

![Palm-size PC Start menu on CE 2.11](/assets/articles/guest-additions/ce211-palmsize-startmenu.png)

Windows Embedded Compact 7, with the shared folder mounted, a browser on a live site, and a
third-party shell. Everything here is the guest's own software.

![A wide CE 7 desktop with several windows open](/assets/articles/guest-additions/ce7-desktop.png)

![ClassiCube running on CE 7](/assets/articles/guest-additions/ce7-classicube.png)

## How the driver gets in

A Windows CE ROM ships with the display driver its device will use.
[Windows CE ROM containers](rom-containers.md) covers the forms those images come in.

Almost everything CERF boots is a cold-boot image: a blob placed in memory and started, with no
bootloader and no BIOS in between. Some boards carry persistent storage instead, mutable from the
first run.

You cannot replace a module with a larger one in place. The image is laid out to be executed where
it sits, and a bigger driver has nowhere to go. CERF replaces the stock display driver with an
extremely small stub instead, and the stub fetches the rest. It receives the real, much larger
driver over a virtual I/O channel, and manual-maps it into memory itself.

On Windows Mobile the manual map is the only thing that works. WM will not
load an unsigned driver, and a DLL of your own does not get past that. The stub does, because to WM
it is the signed display driver the ROM shipped. The real driver never goes near the loader, so
nothing ever checks it. It is a harsh hack, and it works.

One mechanism covers both. CERF hands the emulator either a fake image that reads back the stub, or
a drive that still reads back the stub. What changes between ROMs is only which of the two it is.

On a regular XIP, MultiXIP or NB0 image - close to everything CERF boots - CERF replaces the PE
outright.

Where the ROM is persistent storage instead, the guest reads it through its own flash controller.
CERF goes as far as changing individual read requests, so the guest reads the chip and gets the stub
back instead of the display driver.

CERF takes the simple path wherever a simple path exists. Where the result cannot be made
deterministic, it does not do it at all.

With Guest Additions off, none of this happens. The emulator reads back the real thing, and the
guest runs the display driver its own ROM shipped.

!!! warning "Experimental"

    Guest Additions modify the ROM as it loads. Some ROMs do not survive it, and some behave
    oddly. It is off by default, and the launcher lists the boards where it is known to cause
    trouble. If a device misbehaves, turn it off first. Launcher usually tells if Guest Additions 
    supported or not.

# Save and restore

CERF snapshots a running machine into a single state file: the CPU, the MMU, all of RAM, the flash
contents, and the registers of every emulated component on the board.

**Save state...** and **Load state...** are in the Actions menu. Closing the window offers to save
on the way out.

![The shutdown dialog offering to save the state](/assets/articles/hibernation/save-on-exit.png)

At startup, the launcher - or `--boot=resume|warm|cold` on the
[command line](command-line.md) - decides what happens to a saved machine: restore it, warm boot it
(RAM and flash kept, the OS reboots), or ignore it.

!!! note

    A state file belongs to the exact CERF build that wrote it. Any other build refuses the file. CERF
    models thousands of peripherlas of hundrends of machines (if you want to compare it to VMware, VirtualBox - those a single machine, CERF is dozens, maybe hundrends later) - it's impossible for CERF itself. If it saves that's a luck and wont ever be a feature.

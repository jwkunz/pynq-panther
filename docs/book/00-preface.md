# Chapter 0 - Preface

## What this project is

The PYNQ-Z2 is a small board that carries a **Xilinx Zynq-7020**: a chip that contains an ordinary dual-core ARM
computer *and* a block of reconfigurable hardware (an FPGA) on the same piece of silicon. The board ships with a Linux
image called **PYNQ**, designed for controlling the FPGA from Python notebooks in a web browser. Out of the box the board
is *headless*: there is no desktop on its HDMI connector, even though the board has an HDMI output.

That is a pity, because the hardware for an HDMI output is already there. PYNQ's standard FPGA design, the **base
overlay**, contains a video DMA engine, a video timing generator, a programmable clock and an HDMI transmitter. What is
missing is the *software* that lets the Linux kernel use them. This project writes that software:

1. a **kernel driver** (`panther_drm`) that makes the hardware appear to Linux as a standard graphics card;
2. a small **supervisor program in Rust** (`panther-session`) that starts a minimal desktop at boot and, if anything
   goes wrong, leaves the board exactly as it was (headless) instead of breaking it;
3. **configuration** that makes the board's buttons, switches and LEDs show up as ordinary Linux devices;
4. an **experimental audio driver** for the board's headphone/microphone codec (chapter 8);
5. an **installer** (`install.sh`) that does all of the above on a fresh board with one command.

No new FPGA design is built. Everything happens on the stock base overlay. This is deliberate: it keeps the project
about *systems software* (drivers, boot, userland) rather than FPGA design, and it means the work applies to any board
that runs the same overlay.

## Why this is a good teaching example

Almost every layer of a modern computer is visible in one small project:

* **Hardware/software boundary** - registers, memory-mapped I/O, DMA, clocks, interrupts (chapters 1-2).
* **Operating-system kernel** - drivers, the device model, the graphics subsystem, kernel modules (chapters 3-5).
* **System start-up** - systemd units, ordering, failure handling (chapter 6).
* **User space** - the X window system, input handling, a window manager (chapters 3, 6, 7).
* **Software engineering** - building against a kernel you do not control, packaging, testing on real hardware, and
  designing for graceful failure (chapters 9-10).

## Prerequisites

You should be comfortable with C (pointers, structs, function pointers), the Linux command line, and basic digital
logic. You do *not* need prior FPGA, kernel or graphics experience; each chapter introduces what it needs. Rust is used for one
small program; the code is explained as it appears.

## What you need

* A PYNQ-Z2 board with the **PYNQ v3.1** SD-card image, powered from its barrel supply (USB power alone is marginal)
* A network connection for the board (Ethernet) and a way to log in (`ssh xilinx@pynq-panther.local`, password `xilinx` on a default image)
* An HDMI monitor and cable, and a USB keyboard and mouse (to the board's USB-A port; a powered hub is kind to the board)
* About 30 minutes for the install, most of it waiting

## Conventions

Shell commands starting with `$` run as a normal user, with `#` as root. File names are relative to the repository root.
Boxes labelled **Try it** are short experiments to run on the board. Boxes labelled **Think about it** pose questions without
answers in the text; the exercises in chapter 11 revisit them.

## A word on honesty

This text describes what was *measured* on a real board, and says so when something was not. The status table in the
top-level `README.md` lists what has been verified on hardware and what has not. Where the text shows numbers
(bandwidths, timings, speeds), they come from the board used to develop the project.

# Chapter 1 - The board: a computer made of two halves

> **Companion reading (official PYNQ documentation):** [PYNQ-Z2 setup](https://pynq.readthedocs.io/en/v3.1/getting_started/pynq_z2_setup.html), [the PYNQ-Z2 base overlay](https://pynq.readthedocs.io/en/v3.1/pynq_overlays/pynqz2.html), [PL and overlays](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/pl.html), [Overlay design methodology](https://pynq.readthedocs.io/en/v3.1/overlay_design_methodology.html); for the chip, the [Zynq-7000 TRM (UG585)](https://docs.amd.com/v/u/en-US/ug585-Zynq-7000-TRM). All pages are listed in [PYNQ-RESOURCES.md](../PYNQ-RESOURCES.md).

## 1.1 A system on a chip

A *system on a chip* (SoC) puts a whole computer - processor cores, memory controller, USB, Ethernet, timers - on
one piece of silicon. The PYNQ-Z2 uses the **Xilinx Zynq-7020**, which adds something unusual: next to the processors sits a
large field of **programmable logic**, an FPGA. The chip therefore has two halves.

| | Processing System (PS) | Programmable Logic (PL) |
|---|---|---|
| What it is | Two ARM Cortex-A9 cores at up to 650 MHz, caches, DDR controller, USB, Ethernet, SD, UART, I2C, GPIO | About 85,000 logic cells, 220 DSP slices, 4.9 Mbit of block RAM |
| How you change it | Write software | Load a *bitstream* that wires up the logic |
| Fixed or flexible | Fixed silicon, always there | Blank until configured |
| Runs | Linux | Whatever hardware design you load |

The board adds 512 MB of DDR3 memory, an HDMI output (and input), a headphone/microphone codec, four buttons, two switches,
six LEDs, and connectors (Arduino, Raspberry-Pi and Pmod headers) that go to the PL.

**Think about it.** Why might a designer want logic that can be rewired at run time instead of a fixed chip? What does
it cost (hint: compare clock speeds and silicon area with the ARM cores)?

## 1.2 Two halves, one memory: AXI

The halves talk over **AXI** (Advanced eXtensible Interface), ARM's standard on-chip bus. You do not need its details, but three
ideas matter for everything that follows.

1. **Memory-mapped I/O.** A block in the PL appears to the CPU as a range of addresses. Reading or writing those
   addresses does not touch DDR memory; it reads or writes the block's *registers*. The same `ldr`/`str` instructions are used.
2. **Masters and slaves.** A *master* starts transfers; a *slave* answers them. The CPU is a master. A register block is a
   slave. A DMA engine is a master that reads and writes memory by itself.
3. **Different ports for different jobs.** The PS has *general-purpose* (GP) ports for the CPU to reach PL registers, and
   *high-performance* (HP) ports that let PL masters read and write DDR memory directly.

```
        +----------------------------- Zynq-7020 -----------------------------+
        |   Processing System                       Programmable Logic          |
        |  +------+  +------+                                                   |
        |  | ARM  |  | ARM  |        GP port  ---->  register blocks (slaves)   |
        |  +--+---+  +--+---+     (CPU reaches PL)    VDMA, clock, timing, ...  |
        |     |  caches  |                                                      |
        |   +-+----------+-+       HP port   <----   VDMA, audio (masters)      |
        |   | DDR controller| <--  (PL reaches DDR)   read the framebuffer      |
        |   +------+--------+                                                   |
        +----------|------------------------------------------------------------+
                   |
               512 MB DDR3
```

The physical address map of the PS puts the PL's general-purpose window at `0x40000000-0x7FFFFFFF` (first GP port) and
`0x80000000-0xBFFFFFFF` (second). The base overlay's blocks live in the first window, for example:

| Block (in the base overlay) | Address | Purpose |
|---|---|---|
| Video DMA (`axi_vdma`) | `0x43000000` | reads frames from DDR and streams them out |
| Pixel clock generator (`axi_dynclk`) | `0x43C10000` | reprogrammable clock for the video output |
| Video timing controller (`vtc_out`) | `0x43C20000` | generates sync pulses and blanking |
| Colour converter / pixel unpacker | `0x43C60000`, `0x43C70000` | reformat pixels in the stream |
| Switches, buttons, RGB LEDs, LEDs | `0x41200000`, `0x41210000`, `0x41240000`, `0x41250000` | simple GPIO blocks |
| Audio codec controller | `0x43C00000` | I2S data registers for the headphone/mic codec |

**Try it.** On the board, as root, read one register without any driver: `devmem 0x43C20010` (or use the Python snippet in
`docs/book/10-debugging-tales.md`). You will read the video timing controller's version register, `0x0602000c`: version 6.2.
That single read crossed from the ARM core, through an AXI port, to a block of logic that did not exist a moment before the
bitstream was loaded.

## 1.3 Why the bitstream must be loaded first

The PL starts empty after power-up. Linux boots on the PS, and later *something* downloads a bitstream into the PL. On a
PYNQ image that something is a boot script, `/boot/boot.py`, run by the service `bootpy.service`. It loads PYNQ's **base
overlay** (`base.bit`), which is what the project builds on.

This has an important and dangerous consequence: **reading an address in the PL before the bitstream is loaded does not
return garbage; it can hang the processor**, because nothing answers the bus transaction. Therefore:

* drivers for PL blocks must refuse to touch them until the PL is configured (our driver checks a status flag in the
  Zynq's device-configuration block);
* our services are ordered to start *after* `bootpy.service` has finished.

Chapters 4 and 6 return to both points.

## 1.4 The base overlay

An *overlay* in PYNQ terminology is a bitstream plus a description of what it contains. The **base overlay** is PYNQ's standard
design for this board. It includes the video path used in this project, controllers for the buttons, switches and LEDs, an
audio controller, and MicroBlaze soft processors for the Arduino, RPi and Pmod connectors. The description is the
`base.hwh` file ("hardware handoff"), an XML file generated by Xilinx's Vivado tool. This project reads that file to find
the register addresses in the table above; no new bitstream is built, so you never need Vivado to use it.

The decision not to modify the FPGA design has a price: whatever the base overlay *cannot* do, this project cannot do. It is
why the maximum video mode is 1080p at 30 Hz (the video DMA's memory port is only 32 bits wide, chapter 2), why there is no
hardware mouse cursor, and why audio must be polled (chapter 8).

## 1.5 Summary

* The Zynq has an ARM computer (PS) and an FPGA (PL) on one chip, joined by AXI.
* PL blocks look like memory-mapped registers; PL masters can read and write DDR.
* The PL is empty until a bitstream is loaded; touching it earlier can freeze the CPU.
* The base overlay supplies, ready-made, everything this project needs except the Linux drivers.

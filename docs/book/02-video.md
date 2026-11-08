# Chapter 2 - How a picture becomes a signal

> **Companion reading (official PYNQ documentation):** [PYNQ video library](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/video.html) (how PYNQ drives the same HDMI blocks from Python), the board's notebooks `getting_started/5_base_overlay_video` and `base/video`. These notebooks drive the same video blocks as the desktop, so use `sudo panther-mode pynq` first ([PYNQ-RESOURCES.md](../PYNQ-RESOURCES.md), section 6).

## 2.1 Pixels in memory

A picture is a grid of *pixels*. In this project each pixel is 32 bits: eight bits each for red, green and blue, plus eight unused
bits. The format is called `XRGB8888` (the "X" byte is ignored). In memory, bytes of a pixel are stored in the order
**B, G, R, X** (the machine is little-endian, so the 32-bit value `0x00RRGGBB` is stored lowest byte first).

For a 1920 x 1080 picture:

* one line is 1920 x 4 = **7,680 bytes** (the *pitch* or *stride*)
* the whole frame is 7,680 x 1,080 = **8,294,400 bytes** (about 7.9 MiB)

The picture lives in ordinary DDR memory. Something must read it *continuously* and turn it into the signal that goes down the HDMI cable.

## 2.2 Scanning: pixels in time

A monitor does not receive a picture; it receives a *stream*: the pixels of the top line left to right, then the next line,
and so on, over and over, many times per second. The monitor must know where each line and each frame begins, so the stream has
extra gaps and pulses in it. This is a legacy of cathode-ray tubes, whose electron beam needed time to fly back, and it is
still part of every digital video standard.

```
              <------- 1920 active pixels ------->|<- front porch ->|<- sync ->|<- back porch ->|
 line 1       P P P P P P P ...................... P  . . . . . . . .  _______    . . . . . . . . 
 line 2       ...
   ...
 line 1080    ...
 (vertical gaps: 4 lines front porch, 5 lines of sync, 36 lines of back porch)
```

For the standard **1080p30** mode (CEA-861 "VIC 34") the numbers are:

| | Horizontal (pixels) | Vertical (lines) |
|---|---|---|
| Active picture | 1920 | 1080 |
| Front porch | 88 | 4 |
| Sync pulse | 44 | 5 |
| Back porch | 148 | 36 |
| **Total** | **2200** | **1125** |

Every pixel *slot*, active or not, takes one tick of the **pixel clock**. Therefore

> pixel clock = total columns x total lines x frames per second = 2200 x 1125 x 30 = **74,250,000 Hz**.

This one multiplication explains much of video engineering. Want 60 frames per second at the same resolution? The pixel clock doubles
(148.5 MHz). Want to know why a monitor "has no signal"? Probably because the number of slots per line, per frame, or the clock
is not what it expected.

**Try it.** Compute the pixel clock for 1280x720 at 60 Hz (timing: 1650 x 750 total). You should get 74.25 MHz - the *same* clock
as 1080p30. Look at the mode table in `kmod/panther_drm/panther_drm.c` and check the other rows the same way.

## 2.3 Producing the timing: the video timing controller

The **video timing controller** (VTC) is a counter pair: one counts pixel slots along a line, one counts lines. At fixed counts it raises
and lowers the horizontal-sync, vertical-sync and "active video" signals. Its registers hold exactly the table above. Programming a
new display mode means writing the new totals and sync positions (Chapter 5 shows the code).

## 2.4 Producing the clock: an MMCM

The pixel clock must be exactly what the mode needs, and different modes need different clocks (25.175 MHz to 74.25 MHz here). The
FPGA contains **MMCM** blocks (mixed-mode clock managers): a phase-locked loop that multiplies a reference clock, then divides it. Starting from the 100 MHz
clock available in the base overlay:

> output = 100 MHz x (feedback multiplier) / (input divider x output divider)

The multiplied internal oscillator (the *VCO*) must stay between 600 and 1200 MHz. For 1080p30 the driver searches for the
combination that best matches **five times** the pixel clock (371.25 MHz; it will see why in 2.6) and finds input divider 7... hmm,
specifically *divider 7, multiplier 52, output divider 2*, which gives 371.43 MHz - 0.05% fast, which monitors tolerate.

The base overlay's clock block (`axi_dynclk`, from Digilent) lets software *reprogram the MMCM while it runs* through its
Dynamic Reconfiguration Port. The registers are written with divider and multiplier fields plus "lock" and "filter" values
looked up from a table in a Xilinx application note (XAPP888). The table is in `kmod/panther_drm/mmcm_tables.h`.

## 2.5 Feeding the pipeline: the video DMA

Pixels must get from DDR to the timing logic without the CPU copying them. A **DMA** (direct memory access) engine is a master
on the AXI bus that moves data by itself. The base overlay uses the Xilinx **AXI VDMA** (video DMA). Software tells it three things:

* **where** each frame store is in memory (*start address*; it supports four),
* **how big** a line is (*HSIZE*, bytes) and how far apart lines are (*stride*),
* **how many lines** (*VSIZE*); writing this register starts the transfer.

From then on it fetches line after line forever and pushes the pixels into an AXI-Stream (a simple "data plus valid/ready" pipe)
that flows to the video-output block. The CPU is free to do other things - or to draw into memory behind its back.

> **Bandwidth arithmetic.** At 1080p30 the VDMA must read 8,294,400 bytes x 30 frames/s = **248.8 MB/s**. At 1080p60 it would be
> **497.7 MB/s**. The base overlay's VDMA reads memory through a **32-bit-wide** AXI port; on a clock near 142 MHz that is a peak of about
> 568 MB/s, shared with other traffic and with protocol overhead. 1080p30 fits comfortably. 1080p60 does not. This is why the
> project's largest mode is 1080p30.

Because the VDMA keeps reading whatever is in memory, **changing what is on screen means changing what is in the frame store**, or
pointing the VDMA at a different frame store. The second idea is *page flipping*, chapter 3.

## 2.6 From pixels to the wire: HDMI

The final blocks convert the pixel stream to the electrical signal. HDMI (like its predecessor DVI) sends each colour channel as a serial
stream using **TMDS** (transition-minimized differential signalling):

* every 8-bit colour value is expanded into a **10-bit** code chosen to keep the number of 0/1 transitions low (less
  electromagnetic noise) and the DC level balanced;
* each channel (red, green, blue) uses a **differential pair** - two wires carrying opposite polarity - so noise common to both wires cancels;
* a fourth pair carries the pixel clock.

Ten bits per pixel, per channel, in one pixel-clock period means the serial bit rate is **10 x the pixel clock** - 742.5 Mbit/s per
channel at 1080p30. The transmitter IP generates this using both edges of a clock at **5 x the pixel clock**; that is the 371 MHz clock we asked
the MMCM for.

One detail that matters here: the base overlay's transmitter is Digilent's `rgb2dvi`, a **DVI** encoder. It does *not* send HDMI
audio packets, so **this board's HDMI port cannot carry sound** with this overlay. Sound comes from the separate analog codec (chapter 8).

## 2.7 The whole path

```
 DDR memory --(AXI HP port, 32-bit)--> VDMA --> pixel_unpack --> color_convert --> video-out + VTC --> rgb2dvi --> HDMI
   framebuffer          start addr, stride          XRGB->RGB    (identity matrix)     timing & sync     TMDS 10x
                                                                                                         ^
                                 axi_dynclk (MMCM) ---- pixel clock 74.25 MHz and 5x serial clock ------+
```

## 2.8 What can go wrong

| Symptom | Likely cause |
|---|---|
| "No signal" | wrong pixel clock; VTC not enabled; VDMA not started |
| Picture rolls or tears | the video-out FIFO empties (the VDMA cannot keep up) |
| Colours swapped (blue skin) | byte order of the pixel format vs. the converter matrix |
| Wrong size picture | HSIZE/stride/VSIZE disagree with the timing |
| Whole-system freeze | a register of a block in the PL accessed while the PL was unconfigured |

## 2.9 Summary

* A frame is a block of memory; a monitor wants a continuous stream with sync gaps.
* pixel clock = total columns x total lines x frame rate.
* VTC makes the timing, an MMCM makes the clock, a VDMA fetches the memory, a TMDS encoder makes the wire signal.
* The VDMA's 32-bit port caps the project at 1080p30.

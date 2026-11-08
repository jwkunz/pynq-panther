# Chapter 3 - The Linux graphics stack

Chapter 2 ended with hardware that, once programmed, continuously scans a block of memory onto a monitor. This chapter explains how
Linux presents such hardware to programs, and how a desktop is built on top.

## 3.1 Layers

```
  applications (xterm, a browser, ...)
        |   draw using the X protocol (or Wayland)
  X server (Xorg)  --- window manager (fluxbox) decorates and moves windows
        |   "modesetting" driver talks DRM
  libdrm  (user-space library that wraps the ioctls)
  ---------------------------------------------------------- user / kernel boundary
  /dev/dri/card1  (character device)
  DRM core + KMS helpers       <- shared, written once for all GPUs
  panther_drm (our driver)     <- knows *this* hardware
        |  register writes (MMIO) and DMA
  VDMA, VTC, clock, ...  (the base overlay)
```

The point of the middle layers is that **an application should not need to know what graphics hardware it is running on**. The
kernel's *Direct Rendering Manager* (DRM) subsystem defines one interface; each hardware gets a driver that implements it.

## 3.2 DRM and KMS

DRM has two halves. **Rendering** (running shaders on a GPU) is not needed here: the board has no GPU, so all drawing happens
in software on the CPU. **KMS** (kernel mode setting) is what we need: it lets a program choose *what to display, where, and in
which video mode*. KMS models the display hardware as a small set of objects:

| KMS object | Meaning | In our hardware |
|---|---|---|
| **Framebuffer** (`drm_framebuffer`) | A block of memory holding pixels, plus its format and pitch | A *dumb buffer* in DDR |
| **Plane** | A layer that reads a framebuffer and sends pixels to a CRTC | one plane: the VDMA's read channel |
| **CRTC** | The timing generator and scan-out engine for one display | the VTC and the pixel clock |
| **Encoder** | Converts pixels to a transmission format | the HDMI transmitter |
| **Connector** | The physical port, with its list of supported modes | the HDMI-A connector |

Data flows plane -> CRTC -> encoder -> connector. A bigger GPU has several of each (overlay planes, multiple CRTCs for multiple
monitors); ours has exactly one of everything, which makes it a good first driver. The kernel provides a helper called the
*simple display pipe* for exactly this one-of-each case, and `panther_drm` uses it.

A *mode* (`drm_display_mode`) is the timing table of Chapter 2: active size, porches, sync widths, pixel clock, and polarity flags.

## 3.3 Buffers: GEM and "dumb" buffers

Where do framebuffers come from? DRM's memory manager is **GEM** (graphics execution manager). A GEM object is a kernel-allocated
chunk of memory with a handle that a program can map into its own address space. A *dumb buffer* is the simplest possible
kind: plain linear pixels, no tiling, no GPU features, created by the generic `DRM_IOCTL_MODE_CREATE_DUMB` call.

Our driver allocates dumb buffers with the kernel's **DMA helpers** (`drm_gem_dma_*`), which obtain *physically contiguous*
memory from the **CMA** (contiguous memory allocator) pool. Contiguity is essential: the VDMA reads physical addresses
and knows nothing about virtual memory or page tables, so a frame has to occupy one unbroken run of physical memory.

**Think about it.** The CPU and the VDMA both access the buffer. The CPU has data caches; the VDMA reads DDR directly. What goes wrong if the CPU's
last writes are still sitting in its cache when the VDMA reads? (Our buffers are mapped *write-combining*, which bypasses the
data cache for writes; Chapter 5 and the measurements below show the price and the payoff.)

## 3.4 Atomic mode setting and page flipping

Modern KMS is **atomic**: a program bundles all its desired changes - "plane 0 shows framebuffer 7 at this size on CRTC 0 in this
mode" - into one *commit*; the kernel first *checks* it (can the hardware do this?), then applies it all at once or not at all. The driver supplies:

* `enable` - called when a mode is first set (program the clock, timing, DMA);
* `update` - called when the plane's framebuffer changes (a *page flip*);
* `disable` - called when the display is turned off.

**Page flipping.** To animate without tearing, a program draws the next frame into a *second* buffer while the first is on
screen, then asks the kernel to switch. Our VDMA has four frame stores and a *park pointer*: write the new buffer's address into a
free store, point the park pointer at it, and the VDMA switches at the next frame boundary. The program wants a notification
when the switch has happened (so it knows the old buffer is free), delivered as a **flip event** on the device's file descriptor.

**Vblank.** The interval between the last line of one frame and the first of the next is the *vertical blanking interval*, the
moment when switching buffers is invisible. DRM counts vblanks and delivers events at them. A GPU raises an interrupt each vblank; the base overlay does not
wire the VDMA's interrupt to the CPU, so `panther_drm` uses a **high-resolution timer** that fires once per frame period
(33.3 ms at 30 Hz) and checks which frame store the VDMA is reading. Software-emulated vblank is common for simple devices.

## 3.5 The X server and shadow buffers

Applications do not talk to KMS. They talk to the **X server** (Xorg): they ask it to draw lines, text and bitmaps, and to
manage windows; it draws into a framebuffer and shows it. Xorg is largely *driver-independent* because of the
`modesetting` driver, which uses only the generic KMS and dumb-buffer interface of Section 3.2. That is why a new KMS driver can run a desktop with no X-specific code.

We configure Xorg for software rendering and one more trick, **ShadowFB**:

1. X draws into an ordinary-RAM copy of the screen (the *shadow*), because ordinary RAM can be read quickly;
2. the changed rectangles are copied to the scan-out buffer.

This matters because the scan-out buffer is write-combining uncached memory. Measured on this board with `tests/fb_bench`:

| Operation | Speed |
|---|---|
| copy into the scan-out buffer (write) | **491 MB/s** (a full 1080p frame in ~17 ms) |
| copy within normal RAM | 443 MB/s |
| **read** the scan-out buffer | **82 MB/s** |

Writes are as fast as ordinary RAM; reads are six times slower. A drawing program does many read-modify-write operations
(blending, scrolling); the shadow keeps those in fast memory and only *writes* to the scan-out buffer.

**Think about it.** Could the buffer be cached for even faster writes? What would the driver then have to do before the VDMA reads it (hint: cache maintenance)? Why is the measured gain small here?

## 3.6 The window manager and the cursor

X draws windows but does not move or decorate them; a **window manager** does. We use **fluxbox**, a small one that gives
title bars, a taskbar and menus. A window manager is just another X client, so the desktop is easy to replace.

The mouse pointer is drawn by software (`SWcursor`): hardware cursor planes exist on real GPUs, but the base overlay has none.
The cost is small because only the few pixels under the cursor change as it moves.

## 3.7 Summary

* DRM/KMS models display hardware as framebuffer, plane, CRTC, encoder and connector; a driver implements enable, update and disable.
* Buffers are GEM objects; "dumb buffers" in contiguous CMA memory are all the VDMA can use.
* Atomic commits change state all at once; page flips swap buffers at vblank and report completion as events.
* Xorg's `modesetting` driver, plus a shadow buffer, gives a desktop on any KMS driver.

# Chapter 5 - Reading `panther_drm.c`

This chapter walks through `kmod/panther_drm/panther_drm.c` in the order the code runs. Keep the file open. Excerpts here are shortened.

## 5.1 The shape of the file

| Section | What it does |
|---|---|
| register definitions | names for the offsets inside the VDMA, VTC and clock blocks |
| `struct panther` | everything the driver knows: mapped registers, the DRM objects, the state of the vblank timer |
| pixel clock | find and program the MMCM settings |
| timing | program the video timing controller |
| VDMA | start, stop and flip the frame reader |
| the display pipe | `enable`, `disable`, `update` and the vblank timer |
| connector | the fixed list of modes |
| probe and init | construct the DRM device; create the devicetree node |

## 5.2 The driver structure

```c
struct panther {
	struct drm_device drm;                    /* must be first: container_of() recovers us from it */
	struct drm_simple_display_pipe pipe;      /* plane + CRTC + encoder in one */
	struct drm_connector connector;
	void __iomem *vdma, *dynclk, *vtc, *cc, *pu;   /* mapped register blocks */
	bool running;  unsigned int slot;         /* is scan-out on; which VDMA frame store is current */
	struct hrtimer vblank_timer;  ktime_t vblank_period;
	struct drm_pending_vblank_event *flip_event;   /* a flip waiting for the hardware */
	unsigned int flip_slot, flip_wait;
};
```

The kernel's idiom for "an object embedded in a bigger one" is `container_of`: given a pointer to the embedded `drm_device`
the helper `to_panther()` computes the address of the enclosing `struct panther`. C has no inheritance; this is how the kernel gets it.

## 5.3 Mapping the registers

```c
static ulong vdma_base = 0x43000000;   module_param(vdma_base, ulong, 0444);
...
p->vdma = devm_ioremap(dev, vdma_base, 0x10000);
```

`ioremap` creates a virtual address that maps to a physical register address with the right memory attributes (device memory:
no caching, no reordering across accesses). The `devm_` prefix means the mapping is released automatically when the device goes away. Registers are
then accessed with `readl()` and `writel()`, never with plain pointer dereferences, so the compiler cannot cache or reorder them.

The addresses come from the base overlay's `.hwh` file; they are module *parameters* so that a different design could be used without recompiling.

## 5.4 Choosing the pixel clock

`dyn_find_params()` implements the search described in chapter 2. For every input divider 1 to 10 and feedback multiplier
between `6*div` and `12*div` (which keeps the 600-1200 MHz VCO limit), it computes the output divider that lands closest to five times the
pixel clock and keeps the best match:

```c
u64 cdiv = div_u64(ref * fb + div_u64(div * target, 2), div * target);   /* rounded */
f   = div_u64(ref * fb, div * cdiv);                                      /* what we would get */
err = abs(f - target);
```

**No floating point in the kernel.** The original (PYNQ's C code) uses `double`; the kernel does not save the FPU state for
kernel code, so all arithmetic is integer, in kHz. `div_u64()` is the helper for 64-bit division, which 32-bit ARM cannot do with the `/` operator.

`panther_set_pixel_clock()` then converts the three numbers into the clock IP's register encoding (`dyn_divider`, `dyn_count_calc`) and writes them, together
with the lock and filter constants from `mmcm_tables.h`. It restarts the clock: write control = 0, wait until the status bit says stopped, write
control = 1, wait until running. Each wait is `readl_poll_timeout()`, which polls a register but gives up after a time limit instead of hanging if the hardware never answers.

## 5.5 Programming the video timing controller

`panther_vtc_set_timing()` turns a `drm_display_mode` into register values. For example, the total line length and vertical
size:

```c
writel(htotal & 0x1fff, p->vtc + XVTC_GHSIZE);
reg = vtotal & 0x1fff;
writel(reg | (reg << 16), p->vtc + XVTC_GVSIZE);
```

Fields are 13 bits wide (hence the `0x1fff` mask), and the register layout packs two values (start and end) into one 32-bit word.
The sequence is: reset the block, hold "register update", write all the registers, release the update (so the new timing takes
effect together), and set the *source select* bits so the timing comes from these generator registers and not from the block's input detector. Polarity bits are set from the mode's `PHSYNC`/`PVSYNC` flags.

The register layout is the same as the Xilinx `xlnx_vtc` driver, which we read as a reference (chapter 11 lists the sources).

## 5.6 The VDMA

```c
static int panther_vdma_start(struct panther *p, const struct drm_display_mode *m, dma_addr_t addr, u32 pitch)
{
	for (i = 0; i < VDMA_NUM_FSTORES; i++)
		writel(lower_32_bits(addr), p->vdma + VDMA_MM2S_START(i));     /* all four stores point at the frame */
	writel(m->hdisplay * 4, p->vdma + VDMA_MM2S_HSIZE);                /* bytes of picture per line */
	writel(pitch & 0xffff, p->vdma + VDMA_MM2S_STRIDE);                /* bytes between line starts */
	writel(VDMA_CR_RUN_FLAGS, p->vdma + VDMA_MM2S_CR);                 /* run, park mode */
	wait until running ...
	panther_vdma_set_park(p, 0);                                       /* show frame store 0 */
	writel(m->vdisplay, p->vdma + VDMA_MM2S_VSIZE);                    /* writing VSIZE starts the transfer */
}
```

Note the order, which follows the hardware's rules: size registers first, enable, and the *last* write (VSIZE) starts the
reading. The value `0x00011089` written to the control register is copied from PYNQ's own driver: it sets "run", "park mode"
(show one chosen frame store instead of cycling), internal genlock, and a frame-count interrupt setting.

## 5.7 The display pipe

The three callbacks the DRM core calls:

**`panther_pipe_enable`** - bring up a mode: stop anything running; program the clock; program the timing; set `pixel_unpack` to
32 bits per pixel and the colour matrix to the identity (so `XRGB8888` shows with correct colours); enable the timing generator
*before* starting the VDMA (enabling it releases back-pressure on the stream); start the VDMA; compute the frame period for the timer; turn vblank handling on (`drm_crtc_vblank_on`).

**`panther_pipe_disable`** - turn vblank off, complete any pending flip event so no program waits forever, stop the generator and the VDMA.

**`panther_pipe_update`** - the page flip. If scan-out is already running and the framebuffer changed:

```c
unsigned int next = (p->slot + 1) % VDMA_NUM_FSTORES;
writel(addr_of_new_fb, p->vdma + VDMA_MM2S_START(next));   /* put it in a free frame store */
panther_vdma_set_park(p, next);                            /* point the reader at it */
p->slot = next;
p->flip_event = event;                                     /* remember to tell user space when it happens */
```

The hardware switches at the next frame boundary, which we only discover by *asking it*.

## 5.8 The emulated vblank

```c
static enum hrtimer_restart panther_vblank_tick(struct hrtimer *t)
{
	hrtimer_forward_now(t, p->vblank_period);      /* schedule the next tick one frame later */
	drm_crtc_handle_vblank(crtc);                  /* count a vblank */
	if (p->flip_event && (panther_vdma_active(p) == p->flip_slot || ++p->flip_wait > 4)) {
		drm_crtc_send_vblank_event(crtc, p->flip_event);    /* flip done: wake the program */
		p->flip_event = NULL;  drm_crtc_vblank_put(crtc);
	}
	return HRTIMER_RESTART;
}
```

Every 33.3 ms the timer asks the VDMA which frame store it is currently reading (bits 20:16 of its park-pointer register). Once that equals the store we
flipped to, the old buffer is free and the event is delivered. A safety counter releases the event after four ticks in any case, so a stuck VDMA can never freeze user space.

This timer-based vblank is not synchronised to the real scan-out, so it can be up to one frame early or late. For a desktop that is fine. A driver for
tear-free video would use the interrupt; wiring it would need a change in the FPGA design.

## 5.9 The fixed mode list

The HDMI connector on this board has no *DDC* wiring (the pair of wires through which a monitor tells the computer its supported modes, the "EDID"). The driver
therefore cannot ask the monitor and offers a fixed list, 1920x1080@30 first (marked *preferred*), followed by 1366x768, 1280x800, 1280x720, 1024x768, 800x600 and 640x480. Any other mode with a pixel clock from 25 to 85 MHz is accepted if a user adds it
(`xrandr --newmode ...`).

## 5.10 Probe and init

`panther_probe` builds the device. Read it as a checklist: set the DMA mask (32-bit addresses); check the PL is configured; allocate the DRM device (`devm_drm_dev_alloc`); map the registers; initialise the mode configuration;
create the connector; create the display pipe; initialise vblank handling; `drm_dev_register` - after this call user space sees `/dev/dri/card1`.

`panther_init` (module init) creates the devicetree node with an OF changeset, which makes the kernel create the platform device, which triggers `probe`. Since `probe` runs synchronously inside that call, `panther_init`
can inspect a flag (`panther_bound`) to learn whether it succeeded, and fail the module load with `-ENODEV` if the PL was not ready. This is how loading the module "without the base overlay" fails cleanly instead of leaving a dead device.

## 5.11 Exercises for this chapter

1. Change the preferred mode to 1280x720 and reload the module. What else on the board changes? (Hint: look at what X chooses.)
2. Add a mode of your own (for example 1600x900 with reduced blanking) to the table. What pixel clock does it need, and does it fit the 25-85 MHz window? What does it cost in bandwidth?
3. The flip logic waits at most four vblank ticks. What would happen to a video player if the VDMA were slower than that? Discuss the trade-off between a long wait and a short one.
4. `panther_pipe_update` writes `VDMA_MM2S_STRIDE` on every flip. Is that necessary? What does the hardware do if two framebuffers have different pitches?

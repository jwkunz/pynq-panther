// SPDX-License-Identifier: GPL-2.0
/*
 * panther_drm - DRM/KMS driver for the HDMI output of the stock PYNQ-Z2 "base" overlay.
 *
 * The base overlay already contains everything needed to put a framebuffer on the HDMI connector:
 *
 *   DDR --> AXI VDMA (MM2S) --> pixel_unpack --> color_convert --> ... --> v_axi4s_vid_out --> rgb2dvi --> HDMI
 *                                                         axi_dynclk (MMCM) -> pixel clock,  v_tc -> timing
 *
 * but Linux has no driver for that arrangement (the kernel is built without DRM_XLNX_PL_DISP). PYNQ itself drives
 * the blocks from Python via /dev/mem. This module does the same from the kernel and registers the result as a
 * normal DRM device with one connector, so X.org (modesetting) or any KMS client can use it.
 *
 * It has no devicetree node: the base overlay is loaded at run time, so the module creates its own platform
 * device and uses the fixed register addresses of the base overlay (module parameters allow overriding them).
 * The module must only be loaded once the base bitstream is in the PL; reading an unconfigured PL hangs the
 * CPU, so probe refuses to touch the registers unless the Zynq DEVCFG block reports the PL as configured.
 *
 * Scan-out format: XRGB8888 only (the pixel_unpack core is run in its 32 bit mode, the color_convert matrix is the
 * identity). Pixel clocks above ~85 MHz do not fit the 32 bit VDMA memory port, so 1080p30 is the largest mode.
 */
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modes.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#include "mmcm_tables.h"

#define DRIVER_NAME	"panther-drm"

/* ---- register addresses of the base overlay (docs/hardware.md) ---------------------------------------------- */
static ulong vdma_base = 0x43000000;
static ulong dynclk_base = 0x43C10000;
static ulong vtc_base = 0x43C20000;
static ulong cc_base = 0x43C60000;	/* color_convert (HLS, ap_ctrl_none) */
static ulong pu_base = 0x43C70000;	/* pixel_unpack  (HLS, ap_ctrl_none) */
static bool force;
module_param(vdma_base, ulong, 0444);
module_param(dynclk_base, ulong, 0444);
module_param(vtc_base, ulong, 0444);
module_param(cc_base, ulong, 0444);
module_param(pu_base, ulong, 0444);
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "skip the 'PL is configured' safety check (a wrong bitstream will hang the CPU)");

#define DEVCFG_BASE		0xF8007000UL
#define DEVCFG_INT_STS		0x0C	/* the same PCFG_DONE flag the Linux zynq-fpga driver waits for */
#define DEVCFG_INT_STS_PCFG_DONE BIT(2)

/* AXI VDMA, MM2S channel (PG020) */
#define VDMA_MM2S_CR		0x00
#define VDMA_MM2S_SR		0x04
#define VDMA_PARK_PTR		0x28
#define VDMA_MM2S_VSIZE		0x50
#define VDMA_MM2S_HSIZE		0x54
#define VDMA_MM2S_STRIDE	0x58
#define VDMA_MM2S_START(n)	(0x5C + 4 * (n))
#define VDMA_NUM_FSTORES	4
#define VDMA_CR_RUN_FLAGS	0x00011089	/* RS, GenlockEn, internal genlock, IOC irq count 1 (same as PYNQ) */
#define VDMA_CR_STOP_FLAGS	0x00011080
#define VDMA_CR_RESET		BIT(2)
#define VDMA_SR_HALTED		BIT(0)

/* v_tc generator (PG016; same registers as the Xilinx xlnx_vtc driver) */
#define XVTC_CTL		0x000
#define XVTC_GASIZE		0x060
#define XVTC_GENC		0x068
#define XVTC_GPOL		0x06c
#define XVTC_GHSIZE		0x070
#define XVTC_GVSIZE		0x074
#define XVTC_GHSYNC		0x078
#define XVTC_GVBHOFF_F0		0x07c
#define XVTC_GVSYNC_F0		0x080
#define XVTC_GVSHOFF_F0		0x084
#define XVTC_CTL_SWRESET	BIT(31)
#define XVTC_CTL_SRC_ALL	(BIT(18) | BIT(17) | BIT(16) | BIT(15) | BIT(14) | BIT(13) | BIT(11) | BIT(10) | \
				 BIT(9) | BIT(8))
#define XVTC_CTL_GE		BIT(2)
#define XVTC_CTL_RU		BIT(1)
#define XVTC_GPOL_ACP		BIT(5)
#define XVTC_GPOL_AVP		BIT(4)
#define XVTC_GPOL_HSP		BIT(3)
#define XVTC_GPOL_VSP		BIT(2)
#define XVTC_GPOL_HBP		BIT(1)
#define XVTC_GPOL_VBP		BIT(0)

/* Digilent axi_dynclk: a thin AXI-Lite wrapper around the MMCM DRP */
#define DYN_CTRL		0x00
#define DYN_STATUS		0x04
#define DYN_CLK_L		0x08
#define DYN_FB_L		0x0C
#define DYN_FB_H_CLK_H		0x10
#define DYN_DIV			0x14
#define DYN_LOCK_L		0x18
#define DYN_FLTR_LOCK_H		0x1C
#define DYN_CLK_BIT_WEDGE	13
#define DYN_CLK_BIT_NOCOUNT	12
#define DYN_ERR_DIVIDER		(BIT(DYN_CLK_BIT_WEDGE) | BIT(DYN_CLK_BIT_NOCOUNT))

#define HLS_BPP_OFFSET		0x10	/* pixel_unpack: 0=24bpp 1=32bpp 2=8bpp */
#define HLS_COEF(i)		(0x10 + 8 * (i))	/* color_convert: 12 coefficients, 1.0 = 256 */

struct panther {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	void __iomem *vdma, *dynclk, *vtc, *cc, *pu;

	/* state of the running mode */
	bool running;
	unsigned int slot;		/* VDMA frame store used for the frame on screen */
	struct hrtimer vblank_timer;
	ktime_t vblank_period;
	/* page flip waiting for the VDMA to reach the new frame store */
	struct drm_pending_vblank_event *flip_event;
	unsigned int flip_slot;
	unsigned int flip_wait;
};

static inline struct panther *to_panther(struct drm_device *drm)
{
	return container_of(drm, struct panther, drm);
}

/* ================================================================================================ pixel clock */

static u32 dyn_divider(u32 divide)
{
	u32 high, low, out;

	if (divide < 1 || divide > 128)
		return DYN_ERR_DIVIDER;
	if (divide == 1)
		return 0x1041;

	high = divide / 2;
	if (divide & 1) {
		low = high + 1;
		out = BIT(DYN_CLK_BIT_WEDGE);
	} else {
		low = high;
		out = 0;
	}
	out |= 0x03F & low;
	out |= 0xFC0 & (high << 6);
	return out;
}

static u32 dyn_count_calc(u32 divide)
{
	u32 d = dyn_divider(divide);

	if (d == DYN_ERR_DIVIDER)
		return 0xFFFFFFFF;
	return (0xFFF & d) | ((d << 10) & 0x00C00000);
}

/*
 * Find MMCM settings (100 MHz reference) that give the 5x serial clock closest to 5 * pixel_khz.
 * Same search as Digilent's driver, in integer kHz. Returns false if no setting exists.
 */
static bool dyn_find_params(unsigned int pixel_khz, u32 *maindiv, u32 *fbmult, u32 *clkdiv, u32 *got_khz)
{
	const u64 ref = 100000;		/* kHz */
	u64 target = 5ULL * pixel_khz, best_err = ~0ULL;
	u32 div, fb, minfb, maxfb;
	bool found = false;

	for (div = 1; div <= 10; div++) {
		minfb = div * 6;
		maxfb = min_t(u32, div * 12, 64);
		for (fb = minfb; fb <= maxfb; fb++) {
			u64 cdiv = div_u64(ref * fb + div_u64(div * target, 2), div * target);
			u64 f, err;

			if (cdiv < 1 || cdiv > 128)
				continue;
			f = div_u64(ref * fb, div * cdiv);
			err = f > target ? f - target : target - f;
			if (err < best_err) {
				best_err = err;
				*maindiv = div;
				*fbmult = fb;
				*clkdiv = cdiv;
				*got_khz = f;
				found = true;
			}
		}
	}
	return found;
}

static int panther_set_pixel_clock(struct panther *p, unsigned int pixel_khz)
{
	u32 maindiv, fbmult, clkdiv, got, clk0l, clkfbl, divclk, lock_l, fltr_lock_h, v;

	if (!dyn_find_params(pixel_khz, &maindiv, &fbmult, &clkdiv, &got))
		return -EINVAL;
	clk0l = dyn_count_calc(clkdiv);
	clkfbl = dyn_count_calc(fbmult);
	divclk = dyn_divider(maindiv);
	if (clk0l == 0xFFFFFFFF || clkfbl == 0xFFFFFFFF || divclk == DYN_ERR_DIVIDER || fbmult < 2 || fbmult > 64)
		return -EINVAL;
	lock_l = (u32)(panther_lock_lookup[fbmult - 1] & 0xFFFFFFFF);
	fltr_lock_h = (u32)((panther_lock_lookup[fbmult - 1] >> 32) & 0xFF);
	fltr_lock_h |= (panther_filter_lookup_low[fbmult - 1] << 16) & 0x03FF0000;

	drm_dbg_kms(&p->drm, "pixel clock %u kHz: serial %u kHz (div %u, fb %u, out %u)\n", pixel_khz, got, maindiv,
		    fbmult, clkdiv);

	writel(clk0l, p->dynclk + DYN_CLK_L);
	writel(clkfbl, p->dynclk + DYN_FB_L);
	writel(0, p->dynclk + DYN_FB_H_CLK_H);
	writel(divclk, p->dynclk + DYN_DIV);
	writel(lock_l, p->dynclk + DYN_LOCK_L);
	writel(fltr_lock_h, p->dynclk + DYN_FLTR_LOCK_H);

	writel(0, p->dynclk + DYN_CTRL);
	if (readl_poll_timeout(p->dynclk + DYN_STATUS, v, !(v & 1), 100, 100000))
		return -ETIMEDOUT;
	writel(1, p->dynclk + DYN_CTRL);
	if (readl_poll_timeout(p->dynclk + DYN_STATUS, v, v & 1, 100, 100000))
		return -ETIMEDOUT;
	return 0;
}

/* ================================================================================================ timing */

static void panther_vtc_set_timing(struct panther *p, const struct drm_display_mode *m)
{
	u32 htotal = m->htotal, vtotal = m->vtotal, hactive = m->hdisplay, vactive = m->vdisplay;
	u32 hsync_start = m->hsync_start, hbp_start = m->hsync_end;
	u32 vsync_start = m->vsync_start, vbp_start = m->vsync_end;
	u32 reg;

	writel(XVTC_CTL_SWRESET, p->vtc + XVTC_CTL);
	reg = readl(p->vtc + XVTC_CTL);
	writel(reg & ~XVTC_CTL_RU, p->vtc + XVTC_CTL);	/* hold register update while writing */

	writel(htotal & 0x1fff, p->vtc + XVTC_GHSIZE);
	reg = vtotal & 0x1fff;
	writel(reg | (reg << 16), p->vtc + XVTC_GVSIZE);
	writel((hactive & 0x1fff) | ((vactive & 0x1fff) << 16), p->vtc + XVTC_GASIZE);
	writel((hsync_start & 0x1fff) | ((hbp_start & 0x1fff) << 16), p->vtc + XVTC_GHSYNC);
	writel((vsync_start & 0x1fff) | ((vbp_start & 0x1fff) << 16), p->vtc + XVTC_GVSYNC_F0);
	writel(readl(p->vtc + XVTC_GENC) & ~BIT(6), p->vtc + XVTC_GENC);	/* progressive */
	writel((hactive & 0x1fff) | ((hactive & 0x1fff) << 16), p->vtc + XVTC_GVBHOFF_F0);
	writel((hsync_start & 0x1fff) | ((hsync_start & 0x1fff) << 16), p->vtc + XVTC_GVSHOFF_F0);

	reg = XVTC_GPOL_ACP | XVTC_GPOL_AVP;
	if (m->flags & DRM_MODE_FLAG_PVSYNC)
		reg |= XVTC_GPOL_VBP | XVTC_GPOL_VSP;
	if (m->flags & DRM_MODE_FLAG_PHSYNC)
		reg |= XVTC_GPOL_HBP | XVTC_GPOL_HSP;
	writel(reg, p->vtc + XVTC_GPOL);

	/* take all generator timing from the generator registers (not the detector) */
	writel(readl(p->vtc + XVTC_CTL) | XVTC_CTL_SRC_ALL, p->vtc + XVTC_CTL);
	writel(readl(p->vtc + XVTC_CTL) | XVTC_CTL_RU, p->vtc + XVTC_CTL);
}

/* ================================================================================================ VDMA */

static int panther_vdma_wait_halted(struct panther *p, bool halted)
{
	u32 v;

	return readl_poll_timeout(p->vdma + VDMA_MM2S_SR, v, !!(v & VDMA_SR_HALTED) == halted, 100, 200000);
}

static void panther_vdma_stop(struct panther *p)
{
	u32 v;

	writel(VDMA_CR_STOP_FLAGS, p->vdma + VDMA_MM2S_CR);
	if (panther_vdma_wait_halted(p, true))
		drm_warn(&p->drm, "VDMA did not halt; resetting it\n");
	writel(VDMA_CR_STOP_FLAGS | VDMA_CR_RESET, p->vdma + VDMA_MM2S_CR);
	readl_poll_timeout(p->vdma + VDMA_MM2S_CR, v, !(v & VDMA_CR_RESET), 100, 100000);
}

static void panther_vdma_set_park(struct panther *p, unsigned int slot)
{
	u32 v = readl(p->vdma + VDMA_PARK_PTR);

	v = (v & ~0x1f) | slot;
	writel(v, p->vdma + VDMA_PARK_PTR);
}

static unsigned int panther_vdma_active(struct panther *p)
{
	return (readl(p->vdma + VDMA_PARK_PTR) >> 16) & 0x1f;
}

static int panther_vdma_start(struct panther *p, const struct drm_display_mode *m, dma_addr_t addr, u32 pitch)
{
	unsigned int i;

	for (i = 0; i < VDMA_NUM_FSTORES; i++)
		writel(lower_32_bits(addr), p->vdma + VDMA_MM2S_START(i));
	writel(m->hdisplay * 4, p->vdma + VDMA_MM2S_HSIZE);
	writel(pitch & 0xffff, p->vdma + VDMA_MM2S_STRIDE);
	writel(VDMA_CR_RUN_FLAGS, p->vdma + VDMA_MM2S_CR);
	if (panther_vdma_wait_halted(p, false))
		return -ETIMEDOUT;
	p->slot = 0;
	panther_vdma_set_park(p, 0);
	writel(m->vdisplay, p->vdma + VDMA_MM2S_VSIZE);	/* writing VSIZE starts the transfer */
	return 0;
}

/* ================================================================================================ pipeline */

static void panther_pixel_pipeline_setup(struct panther *p)
{
	unsigned int i;

	writel(1, p->pu + HLS_BPP_OFFSET);		/* 32 bpp in memory, 24 bpp stream: channel 4 is dropped */
	for (i = 0; i < 12; i++)			/* identity: out_i = in_i (1.0 == 256) */
		writel((i % 4 == 0) ? 256 : 0, p->cc + HLS_COEF(i));
}

static bool panther_mode_ok(const struct drm_display_mode *m)
{
	/* 5x serial clock must be reachable; memory bandwidth limits the pixel clock (32 bit VDMA port) */
	return m->clock >= 25000 && m->clock <= 85000 && m->hdisplay <= 1920 && m->vdisplay <= 1080 &&
	       !(m->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN));
}

static enum drm_mode_status panther_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
						    const struct drm_display_mode *mode)
{
	return panther_mode_ok(mode) ? MODE_OK : MODE_BAD;
}

static void panther_pipe_enable(struct drm_simple_display_pipe *pipe, struct drm_crtc_state *crtc_state,
				struct drm_plane_state *plane_state)
{
	struct panther *p = to_panther(pipe->crtc.dev);
	const struct drm_display_mode *m = &crtc_state->adjusted_mode;
	struct drm_framebuffer *fb = plane_state->fb;
	dma_addr_t addr = drm_fb_dma_get_gem_addr(fb, plane_state, 0);
	int ret;

	/* quiesce, then bring the chain up in the same order PYNQ does: clock, timing/VTC, pixel format, VDMA */
	if (p->running) {
		writel(readl(p->vtc + XVTC_CTL) & ~XVTC_CTL_GE, p->vtc + XVTC_CTL);
		panther_vdma_stop(p);
	}
	ret = panther_set_pixel_clock(p, m->clock);
	if (ret) {
		drm_err(&p->drm, "cannot set pixel clock %d kHz: %d\n", m->clock, ret);
		return;
	}
	panther_vtc_set_timing(p, m);
	panther_pixel_pipeline_setup(p);
	writel(readl(p->vtc + XVTC_CTL) | XVTC_CTL_GE, p->vtc + XVTC_CTL);	/* releases back-pressure on the VDMA */
	ret = panther_vdma_start(p, m, addr, fb->pitches[0]);
	if (ret) {
		drm_err(&p->drm, "VDMA did not start: %d\n", ret);
		return;
	}
	p->vblank_period = ns_to_ktime(div_u64(1000000ULL * m->htotal * m->vtotal, m->clock));
	p->running = true;
	drm_crtc_vblank_on(&pipe->crtc);	/* simple-pipe helpers leave this to the driver */
	drm_dbg_kms(&p->drm, "mode %s up, fb at %pad pitch %u\n", m->name, &addr, fb->pitches[0]);
}

static void panther_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct panther *p = to_panther(pipe->crtc.dev);

	struct drm_crtc *crtc = &pipe->crtc;

	drm_crtc_vblank_off(crtc);
	spin_lock_irq(&p->drm.event_lock);
	if (p->flip_event) {		/* never leave user space waiting for a flip that cannot complete */
		drm_crtc_send_vblank_event(crtc, p->flip_event);
		p->flip_event = NULL;
		drm_crtc_vblank_put(crtc);
	}
	spin_unlock_irq(&p->drm.event_lock);
	if (!p->running)
		return;
	writel(readl(p->vtc + XVTC_CTL) & ~XVTC_CTL_GE, p->vtc + XVTC_CTL);
	panther_vdma_stop(p);
	p->running = false;
}

static void panther_pipe_update(struct drm_simple_display_pipe *pipe, struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_crtc *crtc = &pipe->crtc;
	struct panther *p = to_panther(crtc->dev);
	struct drm_pending_vblank_event *event = crtc->state->event;

	if (p->running && state->fb && old_state->fb && state->fb != old_state->fb) {
		/*
		 * Page flip: put the new buffer in the next frame store and park on it. The VDMA switches at the
		 * next frame boundary; the flip event is sent from the vblank timer once it reports the new store.
		 */
		unsigned int next = (p->slot + 1) % VDMA_NUM_FSTORES;

		writel(lower_32_bits(drm_fb_dma_get_gem_addr(state->fb, state, 0)), p->vdma + VDMA_MM2S_START(next));
		writel(state->fb->pitches[0] & 0xffff, p->vdma + VDMA_MM2S_STRIDE);
		panther_vdma_set_park(p, next);
		p->slot = next;
		if (event) {
			crtc->state->event = NULL;
			spin_lock_irq(&crtc->dev->event_lock);
			WARN_ON(drm_crtc_vblank_get(crtc));
			if (p->flip_event) {	/* a previous flip never completed: release it */
				drm_crtc_send_vblank_event(crtc, p->flip_event);
				drm_crtc_vblank_put(crtc);
			}
			p->flip_event = event;
			p->flip_slot = next;
			p->flip_wait = 0;
			spin_unlock_irq(&crtc->dev->event_lock);
		}
		return;
	}

	/* nothing to program (mode set already did it): just complete any event */
	if (event) {
		crtc->state->event = NULL;
		spin_lock_irq(&crtc->dev->event_lock);
		if (drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, event);
		else
			drm_crtc_send_vblank_event(crtc, event);
		spin_unlock_irq(&crtc->dev->event_lock);
	}
}

static enum hrtimer_restart panther_vblank_tick(struct hrtimer *t)
{
	struct panther *p = container_of(t, struct panther, vblank_timer);
	struct drm_crtc *crtc = &p->pipe.crtc;
	unsigned long flags;

	hrtimer_forward_now(t, p->vblank_period);
	drm_crtc_handle_vblank(crtc);

	spin_lock_irqsave(&p->drm.event_lock, flags);
	if (p->flip_event) {
		/* complete when the VDMA shows the new store, or give up after a few frames */
		if (panther_vdma_active(p) == p->flip_slot || ++p->flip_wait > 4) {
			drm_crtc_send_vblank_event(crtc, p->flip_event);
			p->flip_event = NULL;
			drm_crtc_vblank_put(crtc);
		}
	}
	spin_unlock_irqrestore(&p->drm.event_lock, flags);
	return HRTIMER_RESTART;
}

static int panther_pipe_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct panther *p = to_panther(pipe->crtc.dev);

	if (!p->vblank_period)
		p->vblank_period = ms_to_ktime(33);
	hrtimer_start(&p->vblank_timer, p->vblank_period, HRTIMER_MODE_REL);
	return 0;
}

static void panther_pipe_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	hrtimer_cancel(&to_panther(pipe->crtc.dev)->vblank_timer);
}

static const struct drm_simple_display_pipe_funcs panther_pipe_funcs = {
	.mode_valid = panther_pipe_mode_valid,
	.enable = panther_pipe_enable,
	.disable = panther_pipe_disable,
	.update = panther_pipe_update,
	.enable_vblank = panther_pipe_enable_vblank,
	.disable_vblank = panther_pipe_disable_vblank,
};

/* ================================================================================================ connector */

/* No DDC on the PYNQ-Z2 HDMI output, so there is no EDID: offer a fixed list. The first entry is preferred. */
static const struct drm_display_mode panther_modes[] = {
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED, 74250, 1920, 2008, 2052, 2200, 0,
		   1080, 1084, 1089, 1125, 0, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 74250, 1280, 1390, 1430, 1650, 0,
		   720, 725, 730, 750, 0, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* CVT reduced blanking, 71.00 and 72.00 MHz: the same memory bandwidth as 1080p30 */
	{ DRM_MODE("1366x768", DRM_MODE_TYPE_DRIVER, 72000, 1366, 1414, 1446, 1500, 0,
		   768, 771, 777, 800, 0, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	{ DRM_MODE("1280x800", DRM_MODE_TYPE_DRIVER, 71000, 1280, 1328, 1360, 1440, 0,
		   800, 803, 809, 823, 0, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	{ DRM_MODE("1024x768", DRM_MODE_TYPE_DRIVER, 65000, 1024, 1048, 1184, 1344, 0,
		   768, 771, 777, 806, 0, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	{ DRM_MODE("800x600", DRM_MODE_TYPE_DRIVER, 40000, 800, 840, 968, 1056, 0,
		   600, 601, 605, 628, 0, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	{ DRM_MODE("640x480", DRM_MODE_TYPE_DRIVER, 25175, 640, 656, 752, 800, 0,
		   480, 490, 492, 525, 0, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
};

static int panther_conn_get_modes(struct drm_connector *connector)
{
	int i, n = 0;

	for (i = 0; i < ARRAY_SIZE(panther_modes); i++) {
		struct drm_display_mode *m = drm_mode_duplicate(connector->dev, &panther_modes[i]);

		if (!m)
			break;
		drm_mode_set_name(m);
		drm_mode_probed_add(connector, m);
		n++;
	}
	return n;
}

static const struct drm_connector_helper_funcs panther_conn_helper_funcs = {
	.get_modes = panther_conn_get_modes,
};

static const struct drm_connector_funcs panther_conn_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/* ================================================================================================ device */

static const struct drm_mode_config_funcs panther_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const u32 panther_formats[] = { DRM_FORMAT_XRGB8888 };

DEFINE_DRM_GEM_DMA_FOPS(panther_fops);

static const struct drm_driver panther_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &panther_fops,
	DRM_GEM_DMA_DRIVER_OPS,
	.name = "panther",
	.desc = "PYNQ-Z2 base overlay HDMI output",
	.date = "20261107",
	.major = 0,
	.minor = 1,
};

static bool panther_pl_configured(void)
{
	void __iomem *devcfg = ioremap(DEVCFG_BASE, 0x100);
	bool done;

	if (!devcfg)
		return false;
	done = readl(devcfg + DEVCFG_INT_STS) & DEVCFG_INT_STS_PCFG_DONE;
	iounmap(devcfg);
	return done;
}

static void __iomem *panther_map(struct device *dev, ulong base)
{
	void __iomem *r = devm_ioremap(dev, base, 0x10000);

	if (!r)
		dev_err(dev, "cannot map 0x%lx\n", base);
	return r;
}

static bool panther_bound;	/* set by a successful probe; decides whether the module stays loaded */

static int panther_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct panther *p;
	struct drm_device *drm;
	int ret;

	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	if (!force && !panther_pl_configured()) {
		dev_err(dev, "the PL is not configured: load the base overlay first\n");
		return -ENODEV;
	}

	p = devm_drm_dev_alloc(dev, &panther_driver, struct panther, drm);
	if (IS_ERR(p))
		return PTR_ERR(p);
	drm = &p->drm;
	platform_set_drvdata(pdev, p);

	p->vdma = panther_map(dev, vdma_base);
	p->dynclk = panther_map(dev, dynclk_base);
	p->vtc = panther_map(dev, vtc_base);
	p->cc = panther_map(dev, cc_base);
	p->pu = panther_map(dev, pu_base);
	if (!p->vdma || !p->dynclk || !p->vtc || !p->cc || !p->pu)
		return -ENOMEM;

	hrtimer_init(&p->vblank_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	p->vblank_timer.function = panther_vblank_tick;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = 640;
	drm->mode_config.min_height = 480;
	drm->mode_config.max_width = 1920;
	drm->mode_config.max_height = 1080;
	drm->mode_config.funcs = &panther_mode_config_funcs;
	drm->mode_config.preferred_depth = 24;

	ret = drm_connector_init(drm, &p->connector, &panther_conn_funcs, DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		return ret;
	drm_connector_helper_add(&p->connector, &panther_conn_helper_funcs);
	p->connector.status = connector_status_connected;

	ret = drm_simple_display_pipe_init(drm, &p->pipe, &panther_pipe_funcs, panther_formats,
					   ARRAY_SIZE(panther_formats), NULL, &p->connector);
	if (ret)
		return ret;

	ret = drm_vblank_init(drm, 1);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;
	panther_bound = true;
	dev_info(dev, "HDMI output registered (1920x1080@30 preferred, XRGB8888)\n");
	return 0;
}

static void panther_remove(struct platform_device *pdev)
{
	struct panther *p = platform_get_drvdata(pdev);

	panther_bound = false;
	drm_dev_unregister(&p->drm);
	drm_atomic_helper_shutdown(&p->drm);
	hrtimer_cancel(&p->vblank_timer);
}

static const struct of_device_id panther_of_match[] = {
	{ .compatible = "panther,panther-drm" },
	{ }
};
MODULE_DEVICE_TABLE(of, panther_of_match);

static struct platform_driver panther_platform_driver = {
	.driver = { .name = DRIVER_NAME, .of_match_table = panther_of_match },
	.probe = panther_probe,
	.remove_new = panther_remove,
};

static struct of_changeset panther_ocs;
static bool panther_ocs_applied;

/*
 * The display is not described in the boot devicetree (the base overlay is loaded at run time), so the module adds a
 * minimal node, which makes the kernel create the platform device that our driver binds to. Having an OF node also
 * matters for user space: libdrm (modetest, Xorg's device scan, compositors) reads OF_COMPATIBLE_* from the uevent
 * of a platform DRM device and crashes when it is missing.
 */
static int panther_create_of_node(void)
{
	struct device_node *np;
	int ret = -ENOMEM;

	of_changeset_init(&panther_ocs);
	np = of_changeset_create_node(&panther_ocs, of_root, "panther-drm");
	if (!np)
		goto fail;
	ret = of_changeset_add_prop_string(&panther_ocs, np, "compatible", "panther,panther-drm");
	if (ret)
		goto fail;
	ret = of_changeset_apply(&panther_ocs);
	if (ret)
		goto fail;
	panther_ocs_applied = true;
	return 0;
fail:
	of_changeset_destroy(&panther_ocs);
	return ret;
}

static void panther_destroy_of_node(void)
{
	if (panther_ocs_applied) {
		of_changeset_revert(&panther_ocs);	/* removes the node and with it the platform device */
		of_changeset_destroy(&panther_ocs);
		panther_ocs_applied = false;
	}
}

static int __init panther_init(void)
{
	int ret;

	ret = platform_driver_register(&panther_platform_driver);
	if (ret)
		return ret;
	ret = panther_create_of_node();		/* probe runs synchronously from here */
	if (ret) {
		platform_driver_unregister(&panther_platform_driver);
		return ret;
	}
	/* the probe result decides whether the module stays: no PL, no display */
	if (!panther_bound) {
		panther_destroy_of_node();
		platform_driver_unregister(&panther_platform_driver);
		return -ENODEV;
	}
	return 0;
}

static void __exit panther_exit(void)
{
	panther_destroy_of_node();
	platform_driver_unregister(&panther_platform_driver);
}

module_init(panther_init);
module_exit(panther_exit);

MODULE_DESCRIPTION("DRM driver for the PYNQ-Z2 base overlay HDMI output");
MODULE_LICENSE("GPL");

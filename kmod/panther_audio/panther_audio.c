// SPDX-License-Identifier: GPL-2.0
/*
 * panther_audio - EXPERIMENTAL ALSA sound card for the headphone / line-in / microphone codec of the PYNQ-Z2 base overlay.
 *
 * The base overlay's audio block ("audio_codec_ctrl") is as simple as audio hardware gets: five registers.
 *
 *     0x00 RX left   0x04 RX right   0x08 TX left   0x0C TX right   0x10 STATUS (bit 0 = a new sample pair is due; write 1 to clear)
 *
 * There is no FIFO, no DMA and no interrupt, so software must read and write one stereo sample every 1/48000 s = 20.8 us.
 * PYNQ's own Python driver does this with a busy loop. This module does the same inside a kernel thread and presents the
 * result as a normal ALSA card (aplay, arecord, any sound server). The price is that one CPU core is busy while a stream is
 * open; with no stream open the thread sleeps and costs nothing.
 *
 * The codec itself (Analog Devices ADAU1761) is configured over I2C (the Zynq's PS I2C1, routed to the codec through the FPGA
 * fabric); the register sequence is the one PYNQ uses. Sample format: 24-bit samples in 32-bit words (S24_LE), 48 kHz, stereo.
 *
 * Safety: like panther_drm, probe refuses to touch the PL unless the Zynq reports it as configured.
 */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/wait.h>

#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

static ulong audio_base = 0x43C00000;
static int i2c_bus = 1;			/* /dev/i2c-1 = PS I2C1 (EMIO), the codec's control bus */
static uint codec_addr = 0x3B;
static bool force;
module_param(audio_base, ulong, 0444);
module_param(i2c_bus, int, 0444);
module_param(codec_addr, uint, 0444);
module_param(force, bool, 0444);

#define DEVCFG_BASE		0xF8007000UL
#define DEVCFG_INT_STS		0x0C
#define DEVCFG_INT_STS_PCFG_DONE BIT(2)

#define REG_RX_L	0x00
#define REG_RX_R	0x04
#define REG_TX_L	0x08
#define REG_TX_R	0x0C
#define REG_STATUS	0x10

/* ADAU1761 registers, low byte (the full address is 0x40xx) */
#define R_CLOCK_CONTROL		0x00
#define R_PLL_CONTROL		0x02
#define R_REC_MIXER_L0		0x0A
#define R_REC_MIXER_L1		0x0B
#define R_REC_MIXER_R0		0x0C
#define R_REC_MIXER_R1		0x0D
#define R_LEFT_DIFF_VOL		0x0E
#define R_RIGHT_DIFF_VOL	0x0F
#define R_MIC_BIAS		0x10
#define R_ALC_CONTROL_3		0x14
#define R_SERIAL_PORT_0		0x15
#define R_ADC_CONTROL		0x19
#define R_PLAY_MIXER_L0		0x1C
#define R_PLAY_MIXER_L1		0x1D
#define R_PLAY_MIXER_R0		0x1E
#define R_PLAY_MIXER_R1		0x1F
#define R_HP_L_VOL		0x23
#define R_HP_R_VOL		0x24
#define R_PLAY_POWER		0x29
#define R_DAC_CONTROL_0		0x2A
#define R_SERIAL_IN_ROUTE	0xF2
#define R_SERIAL_OUT_ROUTE	0xF3
#define R_DSP_ENABLE		0xF5
#define R_DSP_RUN		0xF6
#define R_CLOCK_ENABLE_0	0xF9
#define R_CLOCK_ENABLE_1	0xFA

struct pa {
	struct snd_card *card;
	struct snd_pcm *pcm;
	void __iomem *regs;
	struct i2c_adapter *adap;

	struct task_struct *thread;
	wait_queue_head_t wq;
	struct snd_pcm_substream *sub[2];	/* [SNDRV_PCM_STREAM_PLAYBACK], [SNDRV_PCM_STREAM_CAPTURE] */
	bool run[2];
	unsigned int pos[2];			/* position in frames inside the ring buffer */
	unsigned int acc[2];			/* frames since the last period boundary */

	u8 hp_volume;				/* 0..63 */
	u8 capture_source;			/* 0 = line in, 1 = microphone */
};

/* ============================================================================================ codec over I2C */

static int codec_write(struct pa *pa, u8 reg, u8 val)
{
	u8 buf[3] = { 0x40, reg, val };
	struct i2c_msg m = { .addr = codec_addr, .flags = 0, .len = 3, .buf = buf };

	return i2c_transfer(pa->adap, &m, 1) == 1 ? 0 : -EIO;
}

static int codec_pll_init(struct pa *pa)
{
	/* PLL: lock to the 10 MHz master clock supplied by the FPGA, produce 48 kHz-family clocks */
	static const u8 pll[8] = { 0x40, 0x02, 0x02, 0x71, 0x02, 0x3C, 0x21, 0x03 };
	u8 addr[2] = { 0x40, 0x02 }, rd[6];
	struct i2c_msg w = { .addr = codec_addr, .flags = 0, .len = 8, .buf = (u8 *)pll };
	struct i2c_msg ra = { .addr = codec_addr, .flags = 0, .len = 2, .buf = addr };
	struct i2c_msg rr = { .addr = codec_addr, .flags = I2C_M_RD, .len = 6, .buf = rd };
	struct i2c_msg rm[2] = { ra, rr };
	int i, ret;

	ret = codec_write(pa, R_CLOCK_CONTROL, 0x0E);		/* core clock off while the PLL is set */
	if (ret)
		return ret;
	if (i2c_transfer(pa->adap, &w, 1) != 1)
		return -EIO;
	for (i = 0; i < 100; i++) {				/* wait for lock (bit 1 of the last PLL byte) */
		if (i2c_transfer(pa->adap, rm, 2) == 2 && (rd[5] & 0x02))
			return codec_write(pa, R_CLOCK_CONTROL, 0x0F);	/* core clock from the PLL */
		msleep(2);
	}
	return -ETIMEDOUT;
}

static int codec_select_source(struct pa *pa, int mic)
{
	int ret = 0;

	if (!mic) {	/* line in */
		ret |= codec_write(pa, R_REC_MIXER_L0, 0x01);
		ret |= codec_write(pa, R_REC_MIXER_L1, 0x07);
		ret |= codec_write(pa, R_REC_MIXER_R0, 0x01);
		ret |= codec_write(pa, R_REC_MIXER_R1, 0x07);
	} else {	/* microphone */
		ret |= codec_write(pa, R_REC_MIXER_L0, 0x01);
		ret |= codec_write(pa, R_REC_MIXER_L1, 0x10);
		ret |= codec_write(pa, R_LEFT_DIFF_VOL, 0xB3);
		ret |= codec_write(pa, R_REC_MIXER_R0, 0x01);
		ret |= codec_write(pa, R_REC_MIXER_R1, 0x10);
		ret |= codec_write(pa, R_RIGHT_DIFF_VOL, 0xB3);
	}
	return ret ? -EIO : 0;
}

static int codec_set_volume(struct pa *pa, u8 vol)
{
	u8 v = (vol << 2) | 0x3;	/* bit 1 = unmute, bit 0 = headphone amplifier enable */

	return codec_write(pa, R_HP_L_VOL, v) | codec_write(pa, R_HP_R_VOL, v);
}

static int codec_init(struct pa *pa)
{
	static const u8 seq[][2] = {
		{ R_REC_MIXER_L0, 0x00 }, { R_REC_MIXER_R0, 0x00 },
		{ R_LEFT_DIFF_VOL, 0xB3 }, { R_RIGHT_DIFF_VOL, 0xB3 },
		{ R_MIC_BIAS, 0x01 }, { R_ALC_CONTROL_3, 0x20 },
		{ R_SERIAL_PORT_0, 0x00 },			/* codec is I2S slave: the FPGA drives bit and frame clocks */
		{ R_ADC_CONTROL, 0x33 },
		{ R_PLAY_MIXER_L0, 0x21 }, { R_PLAY_MIXER_R0, 0x41 },	/* DAC into the playback mixers, unmuted */
		{ R_PLAY_MIXER_L1, 0x00 }, { R_PLAY_MIXER_R1, 0x00 },
		{ R_PLAY_POWER, 0x03 }, { R_DAC_CONTROL_0, 0x03 },
		{ R_SERIAL_IN_ROUTE, 0x01 }, { R_SERIAL_OUT_ROUTE, 0x01 },
		{ R_DSP_ENABLE, 0x01 }, { R_DSP_RUN, 0x01 },
		{ R_CLOCK_ENABLE_0, 0x7F }, { R_CLOCK_ENABLE_1, 0x03 },
	};
	int i, ret = codec_pll_init(pa);

	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(seq); i++) {
		ret = codec_write(pa, seq[i][0], seq[i][1]);
		if (ret)
			return ret;
	}
	ret = codec_set_volume(pa, pa->hp_volume);
	if (ret)
		return ret;
	return codec_select_source(pa, pa->capture_source);
}

/* ============================================================================================ the sample pump */

/*
 * One thread serves both directions. Per stereo frame: wait for the status flag, clear it, move one sample pair each way.
 * The thread runs at the lowest nice value so that user space (X) has the other core; it deliberately does NOT use a real-time
 * policy, because the kernel would throttle a real-time thread that never sleeps and silence the stream for 50 ms each second.
 */
static int pa_thread(void *data)
{
	struct pa *pa = data;

	set_user_nice(current, -20);
	while (!kthread_should_stop()) {
		wait_event_interruptible(pa->wq, kthread_should_stop() || pa->run[0] || pa->run[1]);
		while (!kthread_should_stop() && (pa->run[0] || pa->run[1])) {
			unsigned int spins = 0;
			bool done[2] = { false, false };
			int d;

			while (!readl(pa->regs + REG_STATUS)) {		/* wait for the next sample time */
				cpu_relax();
				if (++spins > 4000000) {		/* 20 ms with nothing: the codec clock is not running */
					spins = 0;
					if (kthread_should_stop() || !(pa->run[0] || pa->run[1]))
						break;
					cond_resched();
				}
			}
			writel(1, pa->regs + REG_STATUS);

			for (d = 0; d < 2; d++) {
				struct snd_pcm_substream *s = pa->sub[d];
				u32 *buf;

				if (!pa->run[d] || !s || !s->runtime)
					continue;
				buf = (u32 *)s->runtime->dma_area + pa->pos[d] * 2;
				if (d == SNDRV_PCM_STREAM_PLAYBACK) {
					writel(buf[0], pa->regs + REG_TX_L);
					writel(buf[1], pa->regs + REG_TX_R);
				} else {
					buf[0] = sign_extend32(readl(pa->regs + REG_RX_L), 23);
					buf[1] = sign_extend32(readl(pa->regs + REG_RX_R), 23);
				}
				if (++pa->pos[d] >= s->runtime->buffer_size)
					pa->pos[d] = 0;
				if (++pa->acc[d] >= s->runtime->period_size) {
					pa->acc[d] = 0;
					done[d] = true;
				}
			}
			for (d = 0; d < 2; d++)
				if (done[d])
					snd_pcm_period_elapsed(pa->sub[d]);
		}
	}
	return 0;
}

/* ============================================================================================ PCM operations */

static const struct snd_pcm_hardware pa_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S24_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 64 * 1024,
	.period_bytes_min = 1024,
	.period_bytes_max = 16 * 1024,
	.periods_min = 2,
	.periods_max = 32,
};

static int pa_open(struct snd_pcm_substream *s)
{
	struct pa *pa = snd_pcm_substream_chip(s);

	s->runtime->hw = pa_hw;
	pa->sub[s->stream] = s;
	return 0;
}

static int pa_close(struct snd_pcm_substream *s)
{
	struct pa *pa = snd_pcm_substream_chip(s);

	pa->run[s->stream] = false;
	pa->sub[s->stream] = NULL;
	return 0;
}

static int pa_prepare(struct snd_pcm_substream *s)
{
	struct pa *pa = snd_pcm_substream_chip(s);

	pa->pos[s->stream] = 0;
	pa->acc[s->stream] = 0;
	return 0;
}

static int pa_trigger(struct snd_pcm_substream *s, int cmd)
{
	struct pa *pa = snd_pcm_substream_chip(s);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		pa->run[s->stream] = true;
		wake_up(&pa->wq);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		pa->run[s->stream] = false;
		return 0;
	}
	return -EINVAL;
}

static snd_pcm_uframes_t pa_pointer(struct snd_pcm_substream *s)
{
	struct pa *pa = snd_pcm_substream_chip(s);

	return READ_ONCE(pa->pos[s->stream]);
}

static const struct snd_pcm_ops pa_ops = {
	.open = pa_open,
	.close = pa_close,
	.prepare = pa_prepare,
	.trigger = pa_trigger,
	.pointer = pa_pointer,
};

/* ============================================================================================ mixer controls */

static int vol_info(struct snd_kcontrol *k, struct snd_ctl_elem_info *u)
{
	u->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	u->count = 1;
	u->value.integer.min = 0;
	u->value.integer.max = 63;
	return 0;
}

static int vol_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *u)
{
	struct pa *pa = snd_kcontrol_chip(k);

	u->value.integer.value[0] = pa->hp_volume;
	return 0;
}

static int vol_put(struct snd_kcontrol *k, struct snd_ctl_elem_value *u)
{
	struct pa *pa = snd_kcontrol_chip(k);
	long v = clamp_val(u->value.integer.value[0], 0, 63);
	int changed = v != pa->hp_volume;

	if (changed) {
		pa->hp_volume = v;
		codec_set_volume(pa, v);
	}
	return changed;
}

static int src_info(struct snd_kcontrol *k, struct snd_ctl_elem_info *u)
{
	return snd_ctl_enum_info(u, 1, 2, (const char *const[]){ "Line In", "Mic" });
}

static int src_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *u)
{
	u->value.enumerated.item[0] = ((struct pa *)snd_kcontrol_chip(k))->capture_source;
	return 0;
}

static int src_put(struct snd_kcontrol *k, struct snd_ctl_elem_value *u)
{
	struct pa *pa = snd_kcontrol_chip(k);
	unsigned int v = u->value.enumerated.item[0];
	int changed = v != pa->capture_source;

	if (v > 1)
		return -EINVAL;
	if (changed) {
		pa->capture_source = v;
		codec_select_source(pa, v);
	}
	return changed;
}

static const struct snd_kcontrol_new pa_controls[] = {
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Headphone Playback Volume",
	  .info = vol_info, .get = vol_get, .put = vol_put },
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Capture Source",
	  .info = src_info, .get = src_get, .put = src_put },
};

/* ============================================================================================ module */

static struct platform_device *pa_pdev;
static struct pa *pa_dev;

static bool pa_pl_configured(void)
{
	void __iomem *devcfg = ioremap(DEVCFG_BASE, 0x100);
	bool done;

	if (!devcfg)
		return false;
	done = readl(devcfg + DEVCFG_INT_STS) & DEVCFG_INT_STS_PCFG_DONE;
	iounmap(devcfg);
	return done;
}

static int __init pa_init(void)
{
	struct pa *pa;
	int ret, i;

	if (!force && !pa_pl_configured()) {
		pr_err("panther_audio: the PL is not configured: load the base overlay first\n");
		return -ENODEV;
	}
	pa = kzalloc(sizeof(*pa), GFP_KERNEL);
	if (!pa)
		return -ENOMEM;
	pa->hp_volume = 57;		/* PYNQ's default level (0xE5 >> 2) */
	init_waitqueue_head(&pa->wq);
	pa->regs = ioremap(audio_base, 0x1000);
	if (!pa->regs) {
		ret = -ENOMEM;
		goto err_free;
	}
	pa->adap = i2c_get_adapter(i2c_bus);
	if (!pa->adap) {
		pr_err("panther_audio: I2C bus %d not found\n", i2c_bus);
		ret = -ENODEV;
		goto err_unmap;
	}
	ret = codec_init(pa);
	if (ret) {
		pr_err("panther_audio: the ADAU1761 codec did not answer on I2C bus %d, address 0x%02x (%d)\n", i2c_bus, codec_addr, ret);
		goto err_i2c;
	}

	pa_pdev = platform_device_register_simple("panther-audio", PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(pa_pdev)) {
		ret = PTR_ERR(pa_pdev);
		goto err_i2c;
	}
	ret = snd_card_new(&pa_pdev->dev, SNDRV_DEFAULT_IDX1, "panther", THIS_MODULE, 0, &pa->card);
	if (ret)
		goto err_pdev;
	strscpy(pa->card->driver, "panther_audio", sizeof(pa->card->driver));
	strscpy(pa->card->shortname, "PYNQ-Z2 audio", sizeof(pa->card->shortname));
	strscpy(pa->card->longname, "PYNQ-Z2 ADAU1761 via base overlay (polled)", sizeof(pa->card->longname));
	ret = snd_pcm_new(pa->card, "ADAU1761", 0, 1, 1, &pa->pcm);
	if (ret)
		goto err_card;
	pa->pcm->private_data = pa;
	strscpy(pa->pcm->name, "ADAU1761", sizeof(pa->pcm->name));
	snd_pcm_set_ops(pa->pcm, SNDRV_PCM_STREAM_PLAYBACK, &pa_ops);
	snd_pcm_set_ops(pa->pcm, SNDRV_PCM_STREAM_CAPTURE, &pa_ops);
	snd_pcm_set_managed_buffer_all(pa->pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 64 * 1024, 64 * 1024);
	for (i = 0; i < ARRAY_SIZE(pa_controls); i++) {
		ret = snd_ctl_add(pa->card, snd_ctl_new1(&pa_controls[i], pa));
		if (ret)
			goto err_card;
	}
	pa->thread = kthread_run(pa_thread, pa, "panther-audio");
	if (IS_ERR(pa->thread)) {
		ret = PTR_ERR(pa->thread);
		goto err_card;
	}
	ret = snd_card_register(pa->card);
	if (ret)
		goto err_thread;
	pa_dev = pa;
	pr_info("panther_audio: sound card registered (48 kHz stereo S24_LE, polled)\n");
	return 0;
err_thread:
	kthread_stop(pa->thread);
err_card:
	snd_card_free(pa->card);
err_pdev:
	platform_device_unregister(pa_pdev);
err_i2c:
	i2c_put_adapter(pa->adap);
err_unmap:
	iounmap(pa->regs);
err_free:
	kfree(pa);
	return ret;
}

static void __exit pa_exit(void)
{
	struct pa *pa = pa_dev;

	pa->run[0] = pa->run[1] = false;
	kthread_stop(pa->thread);
	snd_card_free(pa->card);
	platform_device_unregister(pa_pdev);
	i2c_put_adapter(pa->adap);
	iounmap(pa->regs);
	kfree(pa);
}

module_init(pa_init);
module_exit(pa_exit);

MODULE_DESCRIPTION("Experimental ALSA driver for the PYNQ-Z2 base overlay audio codec (polled)");
MODULE_LICENSE("GPL");

# Chapter 8 - Sound on a board with no sound driver

> **Companion reading (official PYNQ documentation):** [PYNQ audio library](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/audio.html) and the notebooks `getting_started/6_base_overlay_audio` and `base/audio` (PYNQ's own, Python-driven way to use the same codec). Use `sudo panther-mode pynq` before running them.

> **Status: experimental, but working.** Measured on the board: a 5-second test tone played in 4,981 ms (real time to within 0.4%) and was
> heard on the headphone jack; recording delivered exactly 96,000 frames in 2 seconds. During a stream the polling thread uses 99.9% of one core.
> Not yet judged: recording quality, behaviour under heavy CPU load, and long-term glitch rate.

## 8.1 Two kinds of "HDMI audio" that this board does not have

HDMI can carry digital audio inside the video signal. This board cannot: the HDMI transmitter in the base overlay is a DVI encoder
(Chapter 2), which has no audio packets. Sound must come from the board's separate **analog codec** and its three 3.5 mm
jacks (headphone out, line in, microphone in). So the plain answer to "can the HDMI monitor play sound?" is **no, not with this overlay**. The rest of this chapter is about
the codec.

## 8.2 Digital audio in one page

* **Sampling.** Sound is a continuous pressure wave. An *analog-to-digital converter* (ADC) measures it at regular instants - **48,000 times a second** here - and stores each measurement as an integer. A *DAC* does the reverse.
* **Stereo, 24 bits.** A *frame* holds one measurement per channel. Here: two channels, 24 bits each (a range of about +/- 8 million), so a frame is 6 bytes of information.
* **I2S.** The codec chip and its controller exchange samples over a three-wire serial link: a *bit clock*, a *left/right word clock* (the frame clock, 48 kHz), and *data*. One side is the *master* that generates the clocks. In this design the FPGA is the master and the codec the slave.
* **Control.** The codec has hundreds of configuration registers (volumes, mixers, power, clock PLL) that are written over a separate slow link, **I2C**.

## 8.3 What the base overlay gives us

The block at `0x43C00000` (`audio_codec_ctrl`) implements the I2S link and nothing more:

| Offset | Register |
|---|---|
| `0x00`, `0x04` | most recent *received* left and right sample |
| `0x08`, `0x0C` | *transmit* left and right sample (sent at the next frame) |
| `0x10` | status: bit 0 = "a new frame is due"; write 1 to clear |

There is **no FIFO** (a buffer that could hold several samples), **no DMA** (hardware that moves samples to memory) and **no interrupt**. The
software must write the next sample before the next frame time, every 1/48000 s = **20.8 microseconds**, and read each incoming sample before it is overwritten.

I2C for the codec is the Zynq's second I2C controller, routed to the codec's pins through the FPGA (`/dev/i2c-1`, device address `0x3B`). This part is easy; Linux's I2C layer does it.

## 8.4 Why polling is a last resort

Consider the usual approaches for getting data to a device:

| Approach | CPU cost | Needs |
|---|---|---|
| **DMA + interrupt per buffer** (what real sound cards do) | almost none | a DMA engine and interrupt in hardware |
| **Interrupt per sample** | 48,000 interrupts/s; each costs several microseconds -> a large fraction of a core, and jitter spoils the sound | an interrupt per frame |
| **Timer per sample** | the same, and timers are not precise enough | nothing |
| **Busy-wait (poll)** | **one whole core**, but timing is exact | nothing |

The base overlay offers none of the first two. The honest solution under the "no new hardware" rule is a **polling loop**, which is what PYNQ's own Python/C driver does.
The kernel version runs the loop in a dedicated **kernel thread** that exists only while a stream is open.

**Think about it.** The board has two cores. If the polling thread owns one, what does that leave for X, the window manager and the program producing the audio? How would you
measure whether glitches occur? What FPGA change (one paragraph, no Verilog) would remove the problem?

## 8.5 The driver in outline (`kmod/panther_audio/panther_audio.c`)

**Registering a sound card.** ALSA, the Linux sound architecture, models a *card* (a device) with one or more *PCM devices* (a stream interface) each with a
*playback* and a *capture* substream. The driver describes what a stream supports (`struct snd_pcm_hardware`: 48 kHz, 2 channels, 24-bit samples stored in 32 bits = format `S24_LE`, buffer sizes), and implements a few callbacks:

| Callback | What it does here |
|---|---|
| `open` / `close` | remember the substream; mark it not running |
| `prepare` | reset the position counters |
| `trigger` | START: set the "running" flag and wake the thread; STOP: clear it |
| `pointer` | report how far through the ring buffer the hardware has got |

The ring buffer itself is ordinary kernel memory, allocated by ALSA ("managed buffer"); the application writes into it, the thread reads from it.

**The pump.** The thread, per frame:

```c
while (!readl(regs + REG_STATUS)) cpu_relax();   /* wait for the next sample time */
writel(1, regs + REG_STATUS);                    /* acknowledge */
writel(play_buf[pos*2],   regs + REG_TX_L);      /* send one stereo sample ... */
writel(play_buf[pos*2+1], regs + REG_TX_R);
cap_buf[...] = sign_extend32(readl(regs + REG_RX_L), 23);   /* ... and take one in */
```

and every *period* (a chunk of frames, chosen by the application) it calls `snd_pcm_period_elapsed()`, which tells ALSA that the application may refill the part of the buffer that was just consumed.

**Why not a real-time thread?** A real-time (`SCHED_FIFO`) thread that never sleeps looks like a runaway; Linux throttles such threads (by default to 95% of the time), so every second the sound would stop for 50 ms. The thread instead runs
at normal priority with the strongest `nice` value (-20), which keeps it ahead of everything else but still schedulable.

**Mixer controls** are two: "Headphone Playback Volume" (0-63) and "Capture Source" (line in / microphone), which write the codec registers over I2C.

**Codec start-up** is the sequence PYNQ uses (`pynq/lib/_pynq/_audio/audio_adau1761.cpp`): put the codec's clock into a safe state; program its PLL to lock to the 10 MHz clock the FPGA supplies; poll until lock; enable the clock, the DAC and ADC, the mixers and the serial port.

## 8.6 Using it

```sh
sudo systemctl start panther-audio          # or: sudo modprobe panther_audio
aplay -l                                     # lists "PYNQ-Z2 audio"
speaker-test -D hw:panther -c 2 -r 48000 -F S24_LE -t sine        # a test tone on the headphone jack
arecord -D hw:panther -f S24_LE -r 48000 -c 2 -d 5 test.wav       # record 5 seconds from the selected input
amixer -c panther sset 'Headphone Playback Volume' 40
amixer -c panther sset 'Capture Source' Mic
```

While a stream is open, `top` shows the `panther-audio` thread at about 100% of one core. When nothing is playing or recording it sleeps.

If the card does not appear: `dmesg | grep panther_audio`. The usual cause is that the codec did not answer on I2C; see Chapter 9's troubleshooting table.

## 8.7 Limits to keep in mind

* The format is fixed: 48 kHz, stereo, `S24_LE`. Use ALSA's `plug` device (`-D plughw:panther`) or a sound server to convert other formats.
* A long preemption of the thread (for example, a burst of heavy kernel work on that core) drops samples as a click.
* Playback and capture share one thread, so both together cost one core, not two.
* There is no hardware mixing: only one application can open the stream at a time unless a sound server (PulseAudio/PipeWire) mixes in software.

## 8.8 Summary

* HDMI audio is impossible here; the analog codec is the sound path.
* The base overlay's audio block has one sample per register and no buffering, so software must service it every 20.8 us.
* The driver is a small ALSA card with a polling kernel thread, which costs a core while sound plays; the proper fix is a (small) change to the FPGA design.

# Learning the hardware: the official PYNQ documentation

This project adds a desktop to a PYNQ-Z2; it does **not** replace PYNQ. The board's FPGA design (the *base overlay*), the Python
library, the Jupyter notebooks and the SD-card image are all PYNQ's, and PYNQ's own documentation is the authority on how to use them.
If you are new to the board, **start with the pages below** and come back here when you want the desktop, the drivers or the install.

All links are for **PYNQ v3.1**, the version this project targets. (The same pages exist for newer releases: replace `v3.1` by `latest` in the address.)

## 1. Start here

| I want to... | Read |
|---|---|
| set the board up for the first time (jumpers, SD card, power, network) | [PYNQ-Z2 setup](https://pynq.readthedocs.io/en/v3.1/getting_started/pynq_z2_setup.html) |
| see the overall picture of PYNQ | [Getting started](https://pynq.readthedocs.io/en/v3.1/getting_started.html) |
| learn to use Jupyter notebooks on the board | [Jupyter notebooks](https://pynq.readthedocs.io/en/v3.1/getting_started/jupyter_notebooks.html) |
| understand the Python environment on the board | [Python environment](https://pynq.readthedocs.io/en/v3.1/getting_started/python_environment.html) |
| download or rebuild the SD-card image | [PYNQ SD card](https://pynq.readthedocs.io/en/v3.1/pynq_sd_card.html) and the [image downloads](http://www.pynq.io/boards.html) |
| ask a question or look for an answer | the [PYNQ forum](https://discuss.pynq.io/), the [FAQs](https://pynq.readthedocs.io/en/v3.1/faqs.html), the [source on GitHub](https://github.com/Xilinx/PYNQ) |

## 2. What is on the board, and how to use it from Python

| Topic | Read |
|---|---|
| **The PYNQ-Z2 base overlay**: its LEDs, buttons, switches, HDMI in/out, audio, Arduino, Raspberry Pi and Pmod connectors | [PYNQ-Z2 overlay](https://pynq.readthedocs.io/en/v3.1/pynq_overlays/pynqz2.html) |
| loading an overlay | [Loading an overlay](https://pynq.readthedocs.io/en/v3.1/pynq_overlays/loading_an_overlay.html) |
| the library: all of it | [PYNQ libraries](https://pynq.readthedocs.io/en/v3.1/pynq_libraries.html) |
| &nbsp;&nbsp;HDMI video in and out | [video](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/video.html) |
| &nbsp;&nbsp;the headphone / microphone codec | [audio](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/audio.html) |
| &nbsp;&nbsp;GPIO in the FPGA (LEDs, buttons, switches) and in the processor | [AxiGPIO](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/axigpio.html), [PS GPIO](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/psgpio.html) |
| &nbsp;&nbsp;Pmod, Arduino and Raspberry Pi connectors | [Pmod](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/pmod.html), [Arduino](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/arduino.html), [RPi](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/rpi.html) |
| &nbsp;&nbsp;the logic analyser / pattern generator | [logictools](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/logictools.html) |
| &nbsp;&nbsp;MicroBlaze processors in the overlay | [PYNQ MicroBlaze subsystem](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/pynq_microblaze_subsystem.html) |
| the Python API, class by class | [pynq package](https://pynq.readthedocs.io/en/v3.1/pynq_package.html) |

## 3. Talking to the FPGA from Python (the mechanisms behind all of the above)

| Concept | Read |
|---|---|
| `Overlay`: download a design to the FPGA | [Overlay](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/overlay.html), [PL](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/pl.html) |
| reading and writing registers | [MMIO](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/mmio.html) |
| memory the FPGA can read and write | [allocate](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/allocate.html), [DMA](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/dma.html) |
| interrupts | [Interrupt](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/interrupt.html) |
| designing your **own** overlay | [Overlay design methodology](https://pynq.readthedocs.io/en/v3.1/overlay_design_methodology.html), [an overlay tutorial](https://pynq.readthedocs.io/en/v3.1/overlay_design_methodology/overlay_tutorial.html) |

The terms *processing system (PS)*, *programmable logic (PL)*, *bitstream*, *overlay* and *AXI* are explained from scratch in [book chapter 1](book/01-hardware.md).
Chapter 5 of the book shows what PYNQ's own video code does and how this project's kernel driver does the same job.

## 4. Hardware references

* The board itself: the [PYNQ-Z2 page of its manufacturer, TUL](https://www.tulembedded.com/FPGA/ProductsPYNQ-Z2.html) (schematics and reference manual).
* The chip: the [Zynq-7000 Technical Reference Manual, UG585](https://docs.amd.com/v/u/en-US/ug585-Zynq-7000-TRM).
* The PYNQ-Z2 base overlay's source (block diagram scripts, constraints): [`boards/Pynq-Z2`](https://github.com/Xilinx/PYNQ/tree/v3.1/boards/Pynq-Z2) in the PYNQ repository.

## 5. The official tutorials are already on your board

The PYNQ image ships its own tutorial notebooks. They work with no internet. Open the board's Jupyter (`http://pynq-panther.local:9090`, password `xilinx`) or use
the desktop's right-click menu, **PYNQ help**:

| Folder | What it teaches |
|---|---|
| `getting_started/` | `1_jupyter_notebooks`, `2_python_environment`, `3_jupyter_notebooks_advanced_features`, `4_base_overlay_iop`, `5_base_overlay_video`, `6_base_overlay_audio` |
| `base/board` | LEDs, buttons, switches, RGB LEDs |
| `base/video`, `base/audio` | HDMI in and out; the codec |
| `base/arduino`, `base/pmod`, `base/rpi`, `base/microblaze`, `base/trace` | the connectors and the MicroBlaze processors |

## 6. Using PYNQ's tutorials next to this desktop

PYNQ's notebooks and this project's desktop share the same hardware. The danger: **loading an overlay reprograms the whole FPGA**, and a desktop
that is using the FPGA freezes the board when that happens. Almost every PYNQ tutorial starts with `BaseOverlay("base.bit")`.

**The installer therefore adds a guard to PYNQ** (`panther_pynq_guard`, loaded into PYNQ's Python automatically). While the desktop is running:

| What the notebook asks for | What happens |
|---|---|
| `BaseOverlay("base.bit")`: the design the FPGA already holds (68 of the 77 tutorial notebooks on the board) | **PYNQ skips the reprogramming** (and the PS clock writes and device shutdown that go with it) and creates the overlay objects from the live hardware, like `Overlay(..., download=False)`. The notebook runs unchanged, the desktop keeps running. A one-line note appears in the notebook's output. |
| a **different** design (`LogicToolsOverlay("logictools.bit")`: 6 notebooks) | the call **stops with an explanation** instead of freezing the board. Follow it with `sudo panther-mode pynq` (below), or start Python with `PANTHER_PYNQ_GUARD=stop` to let the guard stop the desktop for you. |
| a notebook that also **drives HDMI or audio** (10 notebooks: `base/video/*`, `base/audio/*`, `getting_started/5_base_overlay_video`, `6_base_overlay_audio`, ...) | the overlay loads (same design), but the notebook then controls the *same video and audio blocks the desktop uses*: use `sudo panther-mode pynq` first. |

Two things to know about the "skip": PYNQ normally gives you a **freshly reset** design each time an overlay is created. With the desktop running the design is *not* reset,
so state left by an earlier notebook (a running MicroBlaze program, a configured IO block) is still there. When you want the fresh start, run `sudo panther-mode pynq` first.
And the guard only acts while the desktop's drivers are loaded; with the desktop off, PYNQ behaves exactly as stock.

For everything the guard does not cover, a helper switches the board cleanly:

```sh
sudo panther-mode pynq      # stop the desktop, sound card and GPIO devices; unload their drivers (PYNQ is now completely stock)
                            #   ... run PYNQ notebooks as the official documentation describes ...
sudo panther-mode desktop   # reload the base overlay (as at boot) and bring the desktop back
panther-mode status         # what is running
```

(Jupyter itself keeps running in both modes. You can run `panther-mode` in a desktop terminal or over SSH. Stopping the desktop also closes any browser window you opened on it, so use SSH or another computer's browser to keep working in the notebook.)

**The guard in numbers** (measured on the board): with the guard skipping, a stock `BaseOverlay("base.bit")` made **0** PS clock writes, **0** device shutdowns and **0** firmware writes (stock PYNQ makes 4, 1 and 1). `tests/pynq_guard_selftest.sh` repeats this measurement safely, with the desktop stopped.
Turn the guard off with `PANTHER_PYNQ_GUARD=off` (stock behaviour; a notebook can then freeze the desktop); `./install.sh --uninstall` removes it.

| PYNQ topic | With the guard |
|---|---|
| LEDs, buttons, switches (`base/board`), Arduino, Pmod, Raspberry Pi, MicroBlaze, Grove peripherals (58 notebooks) | run unchanged next to the desktop. (The LEDs and buttons are also Linux devices here, so PYNQ and Linux write the same registers; both keep working.) |
| HDMI video (`base/video`, `5_base_overlay_video`) | `panther-mode pynq` first: the same video blocks and the same HDMI OUT connector |
| audio (`base/audio`, `6_base_overlay_audio`) | `panther-mode pynq` first: the same codec block as the `panther_audio` sound card |
| logictools and other overlays | `panther-mode pynq` first (or `PANTHER_PYNQ_GUARD=stop`) |
| plain Python notebooks (1 and 2 of `getting_started`) | nothing needed |

## 7. What this project adds to stock PYNQ

| Stock PYNQ | With pynq-panther |
|---|---|
| headless; use it through a browser on another computer | a desktop on the board's own HDMI output, with keyboard and mouse |
| HDMI out only through Python (`HDMIOut`) | a kernel graphics driver (`/dev/dri/card1`); any X program can use the display |
| LEDs, buttons, switches only through Python | also Linux devices: `/sys/class/leds`, keyboard-style events (and PYNQ's own tutorials still work next to them) |
| sound only through Python | an (experimental) ALSA sound card |
| hostname `pynq` | `Pynq-Panther`, announced as `pynq-panther.local` |

The installer changes nothing in PYNQ itself: PYNQ's files, Jupyter and the base overlay are left as they are, and `./install.sh --uninstall` removes this project's additions.

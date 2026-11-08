# From Pixels to Desktop: a guided tour of the pynq-panther graphics stack

A hands-on text for undergraduate computer-science and computer-engineering students. It explains, from the transistors up,
what happens when a small FPGA board turns on, loads a hardware design, and puts a graphical desktop with a working
mouse and keyboard on a monitor. The running example is the project in this repository.

| Chapter | Title | You will learn |
|---|---|---|
| [0](00-preface.md) | Preface | What the project does, how to use this book, what you need |
| [1](01-hardware.md) | The board: a computer made of two halves | Zynq SoC, processing system vs. programmable logic, AXI, the memory map, the "base overlay" |
| [2](02-video.md) | How a picture becomes a signal | Pixels, timing, the pixel clock, TMDS/HDMI, bandwidth arithmetic |
| [3](03-linux-graphics.md) | The Linux graphics stack | DRM/KMS objects, buffers, modes, vblank, X.org, shadow buffers |
| [4](04-kernel-modules.md) | Kernel modules for newcomers | What a module is, why they are version-locked, building one, the device model |
| [5](05-driver-walkthrough.md) | Reading `panther_drm.c` | The driver, function by function |
| [6](06-userspace.md) | Booting into a desktop | systemd, the Rust supervisor, Xorg configuration, graceful fallback |
| [7](07-input-and-gpio.md) | Mice, keyboards, buttons and LEDs | USB HID, evdev, libinput, devicetree overlays |
| [8](08-audio.md) | Sound on a board with no sound driver | The audio IP, the ADAU1761 codec, I2S, why polling is a last resort |
| [9](09-install-guide.md) | Installing, step by step | One command, and what every step of it does; troubleshooting; removal |
| [10](10-debugging-tales.md) | Debugging tales | Thirteen real bugs from this project and how each was found |
| [11](11-exercises.md) | Exercises and further reading | Labs, projects, questions |
| [12](12-developing-on-the-board.md) | Developing on the board itself | Browser, editors, a measured resource budget and why VS Code was dropped |
| [13](13-names-and-looks.md) | Names and looks | How `pynq-panther.local` works (mDNS), and how one colour scheme crosses five toolkits |

**Companion material.** This book explains *this project*. For the board, the base overlay and PYNQ's Python library, the official
[PYNQ documentation](https://pynq.readthedocs.io/en/v3.1/) is the reference; [PYNQ-RESOURCES.md](../PYNQ-RESOURCES.md) maps its pages to the topics here
(each chapter below also names the official pages that go with it).

Reading order: chapters 1-3 are background and can be read away from the board. Chapters 4-8 are the project itself.
Chapter 9 is the practical guide you will follow in the lab. Chapter 10 is the most useful one for learning how to debug.

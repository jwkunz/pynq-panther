# Chapter 7 - Mice, keyboards, buttons and LEDs

> **Companion reading (official PYNQ documentation):** [the PYNQ-Z2 base overlay](https://pynq.readthedocs.io/en/v3.1/pynq_overlays/pynqz2.html) (LEDs, buttons, switches from Python), [AxiGPIO](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/axigpio.html), [PS GPIO](https://pynq.readthedocs.io/en/v3.1/pynq_libraries/psgpio.html), and the notebook `base/board` on the board. These notebooks load `base.bit`, which the installer's PYNQ guard lets run next to the desktop (chapter 6, section 6.9); PYNQ and Linux then write the same registers, which is harmless for LEDs and buttons.

## 7.1 Input in Linux: evdev

Every input device - keyboard, mouse, touchpad, button - ends up as a file `/dev/input/eventN`. Reading it yields **events**:
a type (`EV_KEY` key or button, `EV_REL` relative motion, `EV_ABS` absolute position, `EV_SW` switch), a code (which key), and a value (pressed or released, how far). This
uniform interface is **evdev**. A driver's only job is to translate its device into events. The desktop on top never needs to know whether a
key came from a USB keyboard or a GPIO pin.

## 7.2 A USB mouse, end to end

What happens when you plug in a mouse:

1. **USB host controller** - the Zynq's USB port is a ChipIdea controller (`ci_hdrc`) with an external ULPI PHY chip; the kernel log shows `EHCI Host Controller`. It detects the device and reads its *descriptors*.
2. **USB core** - finds a driver for the device's *class*. Mice and keyboards say "I am a HID device" (Human Interface Device).
3. **usbhid / hid-generic** - parses the *report descriptor* (a tiny program-like description of the report layout: "3 bits of buttons, then two signed bytes of motion") and creates an input device.
4. **evdev** - exposes it as `/dev/input/eventN`; `udev` creates the node and tags it as a mouse or keyboard.
5. **Xorg + libinput** - Xorg, started with `AutoAddDevices`, asks udev for input devices and loads the **libinput** driver for each; libinput applies acceleration, scrolling and so on and passes the result to X as pointer and key events.
6. **fluxbox / xterm** - receive the events as clicks and key presses.

Nothing in this project had to be written for it: the kernel already had USB HID and evdev, and the image already had X's libinput driver. This
is the benefit of exposing hardware through standard interfaces: *everything above* works automatically.

**Try it.** `cat /proc/bus/input/devices` lists every input device, and for each the "Handlers" it is attached to. Run `sudo evtest` (install with `apt`) to see raw events as you press keys.

## 7.3 GPIO: the board's buttons, switches and LEDs

The base overlay has four small **AXI GPIO** blocks (switches, buttons, LEDs, RGB LEDs). A GPIO block is the simplest memory-mapped device: a *data
register* whose bits mirror the pins and a *direction register* (tri-state) that says which pins are inputs.

The Linux kernel *already* contains a driver for this block (`gpio-xilinx`), and generic drivers that turn GPIO lines into inputs and LEDs
(`gpio-keys-polled`, `gpio-leds`). The only missing information is **where the blocks are**. That is exactly what a devicetree supplies.

### 7.3.1 The overlay

`config/panther-io.dts` describes the four blocks and the devices built on them:

```
pz2_buttons: gpio@41210000 {
	compatible = "xlnx,xps-gpio-1.00.a";
	reg = <0x41210000 0x10000>;            /* where it is */
	#gpio-cells = <2>;  gpio-controller;   /* "I provide GPIO lines" */
	xlnx,gpio-width = <4>;                 /* four of them */
	xlnx,all-inputs = <1>;                 /* ... all inputs */
};
pz2-keys {
	compatible = "gpio-keys-polled";  poll-interval = <20>;     /* look every 20 ms */
	button-0 { label = "BTN0"; gpios = <&pz2_buttons 0 0>; linux,code = <114>; };   /* KEY_VOLUMEDOWN */
	...
};
```

The `&pz2_buttons` is a *phandle*: a reference from one node to another, with `0 0` meaning "line 0, active-high". The same style makes the LEDs: each
`gpio-leds` child names a GPIO line and becomes a directory under `/sys/class/leds`.

### 7.3.2 Loading an overlay at run time

The blocks exist only after `base.bit` is loaded, so the overlay cannot be in the boot devicetree. It is compiled with `dtc` (the devicetree compiler)
into a `.dtbo` and written to **configfs**, a virtual filesystem for configuring the kernel:

```sh
mkdir /sys/kernel/config/device-tree/overlays/panther-io
cat panther-io.dtbo > /sys/kernel/config/device-tree/overlays/panther-io/dtbo
```

The kernel merges the nodes into the live tree and creates the devices, whose drivers then probe. The script `scripts/panther-io-load.sh` does this, after checking
that the PL is configured; `panther-io.service` runs it after `bootpy.service`.

### 7.3.3 What you get

* Buttons become keyboard keys: `BTN0..3` -> `KEY_VOLUMEDOWN`, `KEY_VOLUMEUP`, `KEY_MUTE`, `KEY_PLAYPAUSE` (we avoid `KEY_POWER`, which systemd would obey by powering the machine off). The two switches become `EV_SW` events.
* `ls /sys/class/leds` shows `pynqz2:green:ld0` ... `ld3` and the red/green/blue channels of the two RGB LEDs. `echo 1 | sudo tee /sys/class/leds/pynqz2:green:ld0/brightness` lights one.
* LED *triggers* connect an LED to a kernel event: `echo heartbeat | sudo tee /sys/class/leds/pynqz2:green:ld3/trigger` makes it blink like a heartbeat.

**Try it.** Flip a switch and run `sudo evtest`, pick the `pz2-keys` device and watch `SW_LINEOUT_INSERT` events. (Tested: the register read-back shows the
switch bits and LED writes reach the hardware.)

### 7.3.4 Polling versus interrupts

We chose to *poll* the buttons every 20 ms rather than use interrupts. An interrupt would be more efficient but needs the GPIO block's interrupt signal wired to the CPU, and in
the base overlay that wiring goes to PYNQ's own interrupt controller. Polling at 50 Hz costs almost no CPU and is fast enough for a human finger (a press lasts about 100 ms). A
good example of choosing the simpler design when the requirement allows.

### 7.3.5 Sharing with PYNQ's Python

PYNQ's `Overlay` objects also control these LEDs and buttons by writing the same registers through `/dev/mem`. The kernel driver and Python do not coordinate; the kernel driver
keeps its own copy of the output register and writes the whole register when *it* changes an LED. If you use both, expect to occasionally overwrite each other. Choose one owner per device.

## 7.4 Summary

* evdev gives every input device one interface; the desktop sees keyboards, mice and GPIO buttons identically.
* A USB mouse works because layered drivers (USB host, HID, evdev, libinput) were all already present.
* GPIO needed *no new code*, only a devicetree overlay loaded at run time, because the drivers existed and only the addresses were missing.

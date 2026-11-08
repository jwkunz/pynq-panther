# Chapter 11 - Exercises and further reading

## Warm-up questions

1. A 1280x1024@60 mode needs a pixel clock of 108 MHz. Compute the VDMA's memory bandwidth at 32 bits per pixel. Does it fit the base overlay's port? What would you have to change (in hardware or in the format) to make it fit?
2. Why does the pixel clock need to be exactly what the mode specifies, but the driver is allowed to be 0.05% off? What would a monitor do with a clock 5% off?
3. List every place in this project where code checks that the PL is configured before touching it. For each, say what the failure would look like without the check.
4. The supervisor exits with status 0 even when it fails. Argue for and against this design.
5. What is the difference between a *platform driver*, a *devicetree overlay* and an *OF changeset*? Give a use of each from this repository.

## Lab exercises (on the board)

**Lab 1 - Register archaeology.** Using the Python snippet of chapter 10, dump the VDMA status register (`0x43000004`), the video timing controller control register (`0x43C20000`) and the clock status (`0x43C10004`) (a) with the desktop running, (b) after `sudo systemctl stop panther-desktop`. Explain every difference you see. (Bit meanings are in the Xilinx product guides PG020 and PG016.)

**Lab 2 - Mode hunting.** Use `cvt` and `xrandr --newmode` / `--addmode` / `--output ... --mode` to add a mode that is not in the driver's table. Find the smallest and largest pixel clock the driver accepts, and check with a register dump that the VDMA and timing registers changed consistently. Does `xrandr` accept a mode above 85 MHz? Why not?

**Lab 3 - Measure the framebuffer.** Run `tests/fb_bench`. Explain why reading the scan-out buffer is so much slower than writing it. Modify the program to time reading a buffer allocated with `malloc` for comparison.

**Lab 4 - Flip rate.** Run `tests/kms_probe /dev/dri/card1 10`. The reported flip rate is far below 30 per second. Is the driver the bottleneck? (Hint: how long does the program's own `fill()` take for a 1080p frame? Replace it with a `memcpy` from a prepared buffer and measure again.)

**Lab 5 - Break it on purpose.** Put the wrong `cc_base` address on the `insmod` command line (`insmod panther_drm.ko cc_base=0x43C99000`), and describe what happens. (Do this only on a board you are willing to power-cycle; you are touching an address with nothing behind it.) Then explain why the module's `force` parameter exists and why its default is off.

**Lab 6 - Your own LED trigger.** Write a one-line shell loop that makes the four LEDs a binary counter of the number of keys pressed (use `evtest` or `/dev/input` events). Then replace the loop by a `gpio-leds` *trigger*: which one fits? (`ls /sys/class/leds/pynqz2:green:ld0/trigger` lists them.)

**Lab 7 - Fail safe.** Make Xorg fail on purpose (e.g. set `kmsdev` to a nonexistent file in a copy of the config, or `max_restarts = 0` plus a bad `session`). Observe what `panther-session` logs and verify that Jupyter at `http://pynq-panther.local:9090` still works. Then restore.

## Project ideas

* **Hardware cursor**: the base overlay has no cursor plane. Sketch the FPGA block that would add one. How would the driver expose it? (Look at KMS cursor planes.)
* **Interrupts instead of a timer**: find out where the VDMA's `mm2s_introut` goes in the base overlay (`base.hwh`), and what it would take to deliver it as the vblank interrupt.
* **EDID over DDC**: the HDMI transmitter side has no DDC. Does the hardware have a spare I2C you could wire a connector to? How would the driver read modes from it?
* **Wayland**: replace Xorg by a Wayland compositor such as `cage` or `weston` with the pixman renderer. Compare memory use and responsiveness.
* **A second display driver in Rust**: Linux 6.6 has the beginnings of Rust-for-Linux. What does the kernel need (`CONFIG_RUST`, a rebuilt kernel) and what would a Rust DRM driver for this hardware look like?
* **Audio**: see chapter 8; replace the polling loop by a design that needs no busy waiting (FPGA change: a FIFO and DMA).

## Further reading

* *Xilinx Zynq-7000 Technical Reference Manual* (UG585): the PS, AXI ports, the device-configuration block.
* *AXI Video DMA Product Guide* (PG020), *Video Timing Controller Product Guide* (PG016), *XAPP888* (MMCM reconfiguration).
* Linux kernel documentation: `Documentation/gpu/drm-kms.rst`, `drm-mm.rst`, `drm-kms-helpers.rst` and `Documentation/devicetree/`.
* *Linux Device Drivers*, 3rd ed. (Corbet, Rubini, Kroah-Hartman), for the module and device-model concepts (some APIs are dated).
* The `xlnx_vtc.c`, `xlnx_pl_disp.c` files in Xilinx's `linux-xlnx` repository: a professional driver for related hardware; compare it with ours.
* PYNQ's own `pynq/lib/video` and `pynq/lib/_pynq/_video` sources: how the same hardware is driven from user space.
* The libdrm test programs (`modetest`, `kms-universal-planes`) and the `drm-info` tool.

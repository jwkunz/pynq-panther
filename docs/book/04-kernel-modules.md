# Chapter 4 - Kernel modules for newcomers

## 4.1 What a kernel module is

The Linux kernel can be extended while it runs. A **loadable kernel module** is a compiled object (`.ko`, "kernel object")
that the kernel links into itself on `insmod` or `modprobe` and can later remove with `rmmod`. Drivers are typically modules,
so a kernel image need not contain every driver for every device. A module runs with *full kernel privilege*: a bug in it can crash the
whole machine, and an unchecked pointer dereference is not a segmentation fault but a kernel *oops*.

A module has an entry point and an exit point, registered with two macros:

```c
static int __init panther_init(void) { /* register with the kernel */ }
static void __exit panther_exit(void) { /* undo it */ }
module_init(panther_init);
module_exit(panther_exit);
MODULE_LICENSE("GPL");
```

## 4.2 Why modules are tied to one kernel

The kernel has no stable internal interface. Function signatures, structure layouts and even the names of helpers change from
one version to the next. Three mechanisms catch a mismatch:

* **vermagic** - a string in every module recording the kernel version and key configuration options (`6.6.10-xilinx-v2024.1 SMP preempt mod_unload modversions ARMv7 p2v8`). The kernel refuses a module whose vermagic does not match its own.
* **modversions** - a checksum (CRC) of every exported function's signature stored with the module; if a function changed, the load fails rather than crashing later.
* **the build configuration** - if the module is compiled with different options than the kernel (for example, a different
  structure layout because a debug option is on), it would misbehave silently. The build must use the *kernel's own `.config` and headers*.

So a module must be **built against the exact kernel it will run on**. This repository therefore builds on the board itself.

## 4.3 Building out of tree

The kernel's own build system (*kbuild*) is invoked on a tiny Makefile in the module's directory:

```make
obj-m += panther_drm.o                              # "build panther_drm.ko from panther_drm.c"
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) CC=gcc-12 ARCH=arm modules
```

`-C $(KDIR)` says "run the kernel's build system from the kernel tree", `M=` points it back at our directory. The kernel tree
at `$(KDIR)` must contain the kernel's headers, its configuration, and a few *generated* files.

### The two surprises on this board

1. **The compiler.** The PYNQ kernel was built with GCC 12. The board's own `gcc` is version 11, which does not know two
   options the kernel's build passes (`-mstack-protector-guard=tls` and `-ftrivial-auto-var-init=zero`) and stops at once.
   `install.sh` therefore installs `gcc-12` and builds with `CC=gcc-12`. Using a compiler different from the kernel's prints a warning; here it
   is harmless.
2. **Incomplete headers.** `/lib/modules/<release>/build` on the image lacks the generated headers (for instance
   `arch/arm/include/generated/asm/rwonce.h`), which the kernel's prepare step produces, and it lacks the kernel's root `Kbuild` file.
   The usual `make modules_prepare` therefore never finishes: its final step falls back to reading the top-level `Makefile` and re-invokes
   itself without end (Tale 8). That step only regenerates headers the image already ships, so `install.sh` deletes that single line from its
   *private copy* of the kernel `Makefile` and runs the normal `make modules_prepare`, which then takes well under a minute for the tools and headers.
   The copy lives in `/var/lib/panther/kbuild`; the system's own tree is never modified.
   It only does so once per kernel.

The story of finding these two problems is in chapter 10.

## 4.4 The device model and platform drivers

How does the kernel decide that a driver should handle a device? Through the **device model**: devices and drivers are
registered on a **bus**, and the bus's *match* function pairs them. Common buses are PCI, USB and I2C. Devices that are simply
"part of the chip at a known address" sit on the **platform bus**.

On ARM the list of platform devices normally comes from the **devicetree**, a data structure describing the hardware (addresses,
interrupts, clocks) that the bootloader hands to the kernel. A node such as

```
panther-drm { compatible = "panther,panther-drm"; };
```

creates a platform device; a platform *driver* whose `of_match_table` contains the same `compatible` string is *bound* to it
and its `probe()` function is called. `probe()` allocates resources and registers the device with its subsystem (here, DRM).

### Our situation: hardware that appears later

The boot devicetree describes the board as it is at power-up, before the base overlay is loaded into the PL. Our HDMI
blocks therefore are **not in it**. We have two options: rebuild the boot devicetree (invasive), or add nodes at run time. The kernel supports the latter two ways:

* an **overlay** (`.dtbo`) loaded through *configfs* - used for the GPIO blocks (chapter 7); and
* an **OF changeset** created in kernel code - used by `panther_drm`, which builds the one-node tree above in its init function. It is the code equivalent of an overlay, and has the advantage that the module is self-contained.

Why does the display driver need a devicetree node at all? Because user-space libraries inspect it: libdrm reads the device's
`OF_COMPATIBLE_*` entries and, in the version on the image, **crashes with a null-pointer error if they are missing** (chapter 10). Giving the platform device an OF node costs nothing and makes the device look like all the others.

## 4.5 Staying safe

Our driver's `probe()` begins with a guard:

```c
if (!force && !panther_pl_configured()) {
    dev_err(dev, "the PL is not configured: load the base overlay first\n");
    return -ENODEV;
}
```

`panther_pl_configured()` reads the Zynq device-configuration block's interrupt-status register and tests the `PCFG_DONE` bit,
the same flag the kernel's FPGA manager waits for. The bit tells us a bitstream has finished loading. Without it we would
touch addresses in an empty PL and risk freezing the board. (Chapter 10 tells how we first read the wrong register.)

**Think about it.** The check passes whenever *some* bitstream is loaded, not necessarily the base overlay. What could a
stricter check look at? What would the cost of a false positive be?

## 4.6 Debugging a module

* `dmesg` shows the kernel log; `dev_info`, `dev_err` and `drm_dbg_kms` write to it.
* `echo 0x36 | sudo tee /sys/module/drm/parameters/debug` enables DRM's debug categories (KMS, atomic, vblank); disable by writing 0.
* `modinfo panther_drm.ko` shows the vermagic, parameters and dependencies.
* A kernel oops prints a register dump and a call trace; the first line naming the module tells you where to start.
* Keep a second terminal (or SSH session) open; if the board hangs, you can still read logs after rebooting only if they were saved, so log to the console or a file early.

## 4.7 Summary

* A module extends a running kernel, but only the exact kernel it was built against.
* Build with the kernel's `.config`, headers and compiler; on this board that meant gcc-12 and a private, prepared copy of the headers.
* Platform drivers bind to devicetree nodes; nodes for late-appearing hardware can be added at run time by overlay or changeset.
* Guard every access to hardware that may not be present.

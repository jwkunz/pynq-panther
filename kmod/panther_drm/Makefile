# Out-of-tree build.  On the board:   make        (uses ~/kbuild, a prepared copy of the kernel headers, and gcc-12)
#   make KDIR=/path/to/prepared/kernel/tree CC=gcc-12
KDIR ?= $(HOME)/kbuild
CC   = gcc-12
obj-m += panther_drm.o

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) CC=$(CC) ARCH=arm modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) CC=$(CC) ARCH=arm clean

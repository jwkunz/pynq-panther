// fb_bench - how fast can the CPU fill / copy into a scan-out buffer of panther_drm? (it is write-combined, uncached memory)
//   gcc -O2 -o fb_bench fb_bench.c $(pkg-config --cflags --libs libdrm) && sudo ./fb_bench [/dev/dri/card1]
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char **argv)
{
	int fd = open(argc > 1 ? argv[1] : "/dev/dri/card1", O_RDWR);
	struct drm_mode_create_dumb c = { .width = 1920, .height = 1080, .bpp = 32 };
	struct drm_mode_map_dumb m = { 0 };
	if (fd < 0 || drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) { perror("dumb"); return 1; }
	m.handle = c.handle;
	drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m);
	uint8_t *fb = mmap(0, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
	uint8_t *src = malloc(c.size);
	memset(src, 0x55, c.size);
	int n = 10; double t;
	t = now(); for (int i = 0; i < n; i++) memcpy(fb, src, c.size);
	printf("memcpy into scan-out buffer: %6.1f MB/s  (full 1080p frame: %.0f ms)\n", n * c.size / (now() - t) / 1e6, (now() - t) / n * 1e3);
	t = now(); for (int i = 0; i < n; i++) memset(fb, i, c.size);
	printf("memset scan-out buffer:      %6.1f MB/s\n", n * c.size / (now() - t) / 1e6);
	uint32_t sum = 0; int nr = 2;
	t = now(); for (int i = 0; i < nr; i++) for (size_t j = 0; j < c.size; j += 4) sum += *(volatile uint32_t *)(fb + j);
	printf("read scan-out buffer:        %6.1f MB/s  (%u)  <- why X uses a shadow copy\n", nr * c.size / (now() - t) / 1e6, sum);
	t = now(); for (int i = 0; i < n; i++) memcpy(src, src + 4096, c.size - 4096);
	printf("memcpy in normal RAM:        %6.1f MB/s\n", n * (c.size - 4096) / (now() - t) / 1e6);
	return 0;
}

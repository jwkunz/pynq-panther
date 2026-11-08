// kms_probe - exercise panther_drm without any other software: list the connector's modes, set the preferred mode with a
// colour-bar buffer, then page-flip between two buffers for a few seconds and report the achieved flip rate.
//   gcc -O2 -o kms_probe kms_probe.c $(pkg-config --cflags --libs libdrm)
//   sudo ./kms_probe [/dev/dri/card1] [seconds]
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct buf { uint32_t handle, fb, pitch; uint64_t size; uint32_t *map; };

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static int make_buf(int fd, struct buf *b, int w, int h)
{
	struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = 32 };
	struct drm_mode_map_dumb m = { 0 };
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) { perror("create dumb"); return -1; }
	b->handle = c.handle; b->pitch = c.pitch; b->size = c.size;
	if (drmModeAddFB(fd, w, h, 24, 32, b->pitch, b->handle, &b->fb)) { perror("addfb"); return -1; }
	m.handle = b->handle;
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) { perror("map dumb"); return -1; }
	b->map = mmap(0, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
	if (b->map == MAP_FAILED) { perror("mmap"); return -1; }
	return 0;
}

static void fill(struct buf *b, int w, int h, int phase)
{
	static const uint32_t bars[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000 };
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint32_t c = bars[(x * 8 / w + phase) & 7];
			if (y > h - 40) c = ((x + phase * 40) / 40 & 1) ? 0xffffff : 0x000000;	// moving strip: shows flips
			b->map[y * (b->pitch / 4) + x] = c;
		}
}

static int flips_done;
static void flip_cb(int fd, unsigned seq, unsigned sec, unsigned usec, void *data) { (void)fd; (void)seq; (void)sec; (void)usec; (void)data; flips_done++; }

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/dri/card1";
	double secs = argc > 2 ? atof(argv[2]) : 5;
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror(path); return 1; }
	drmVersionPtr v = drmGetVersion(fd);
	printf("driver %s %d.%d (%s)\n", v->name, v->version_major, v->version_minor, v->desc);
	drmFreeVersion(v);
	drmModeRes *res = drmModeGetResources(fd);
	if (!res) { perror("getresources"); return 1; }
	printf("%d crtc, %d connector, %d encoder\n", res->count_crtcs, res->count_connectors, res->count_encoders);
	drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[0]);
	printf("connector %u status %d modes %d\n", conn->connector_id, conn->connection, conn->count_modes);
	for (int i = 0; i < conn->count_modes; i++)
		printf("  %-10s %3uHz clock %u  %ux%u\n", conn->modes[i].name, conn->modes[i].vrefresh, conn->modes[i].clock,
		       conn->modes[i].hdisplay, conn->modes[i].vdisplay);
	if (!conn->count_modes) return 2;
	drmModeModeInfo mode = conn->modes[0];
	struct buf b[2] = { 0 };
	for (int i = 0; i < 2; i++) { if (make_buf(fd, &b[i], mode.hdisplay, mode.vdisplay)) return 3; fill(&b[i], mode.hdisplay, mode.vdisplay, i); }
	if (drmModeSetCrtc(fd, res->crtcs[0], b[0].fb, 0, 0, &conn->connector_id, 1, &mode)) { perror("setcrtc"); return 4; }
	printf("mode %s set; flipping for %.0f s\n", mode.name, secs);
	double t0 = now(); int cur = 0, pending = 0, issued = 0, phase = 2;
	drmEventContext ev = { .version = 2, .page_flip_handler = flip_cb };
	while (now() - t0 < secs) {
		if (!pending) {
			cur ^= 1; fill(&b[cur], mode.hdisplay, mode.vdisplay, phase++);
			if (drmModePageFlip(fd, res->crtcs[0], b[cur].fb, DRM_MODE_PAGE_FLIP_EVENT, NULL)) { perror("pageflip"); return 5; }
			pending = 1; issued++;
		}
		struct pollfd p = { .fd = fd, .events = POLLIN };
		if (poll(&p, 1, 1000) > 0) { int before = flips_done; drmHandleEvent(fd, &ev); if (flips_done > before) pending = 0; }
		else { printf("timeout waiting for a flip event\n"); return 6; }
	}
	printf("%d flips in %.2f s = %.1f flips/s (%d issued)\n", flips_done, now() - t0, flips_done / (now() - t0), issued);
	printf("OK\n");
	return 0;
}

/*
 * st7789.c - ST7789 SPI display driver implementation (Raspberry Pi)
 *
 * The init command sequence is derived from the verified reference
 * implementation (lv_gba_emu/port/rpi/st7789.c). GPIO drives CS/DC/RST/BLK
 * (SPI runs with SPI_NO_CS); pixel data goes out over spidev in chunks.
 *
 * SPDX-License-Identifier: MIT
 */
#include "st7789.h"
#include "pi_gpio.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* ST7789 command set (subset used here) */
#define ST_CMD_SWRESET 0x01
#define ST_CMD_SLPOUT 0x11
#define ST_CMD_INVOFF 0x20
#define ST_CMD_INVON 0x21
#define ST_CMD_DISPON 0x29
#define ST_CMD_CASET 0x2A
#define ST_CMD_RASET 0x2B
#define ST_CMD_RAMWR 0x2C
#define ST_CMD_MADCTL 0x36
#define ST_CMD_COLMOD 0x3A
#define ST_CMD_RAMCTRL 0xB0

/* MADCTL bits */
#define MADCTL_MY 0x80
#define MADCTL_MX 0x40
#define MADCTL_MV 0x20
#define MADCTL_RGB 0x00

#define DEFAULT_CHUNK 32768u

struct st7789 {
	int rst_pin;
	int cs_pin;
	int dc_pin;
	int blk_pin;

	int spi_fd;
	uint32_t spi_hz;

	int width;  /* effective width after rotation */
	int height; /* effective height after rotation */
	int x_offset;
	int y_offset;
	st7789_rotation_t rotation;

	uint32_t chunk_bytes;
	int little_endian;

	uint8_t *linebuf; /* scratch for fill (one row) */
};

/* ---- low-level helpers ---- */

/* Read the kernel's spidev bufsiz (max bytes per ioctl transfer). Returns the
 * value, or a conservative 4096 if it can't be read. */
static uint32_t
read_spidev_bufsiz(void)
{
	FILE *f = fopen("/sys/module/spidev/parameters/bufsiz", "r");
	if(!f) return 4096u;
	long v = 0;
	int ok = (fscanf(f, "%ld", &v) == 1);
	fclose(f);
	return (ok && v > 0) ? (uint32_t)v : 4096u;
}

static void
delay_ms(unsigned int ms)
{
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

/* Raw SPI write, split into <=chunk_bytes transfers. Returns 0 / <0. */
static int
spi_write(st7789_t *d, const uint8_t *buf, size_t len)
{
	/* Re-assert the SPI mode before every transfer to guard against EGL/GBM
	 * or other kernel/firmware paths that may reset the spidev controller
	 * state. Use hardcoded 0x40 for SPI_NO_CS (kernel >=6.x value). */
	static uint32_t force_mode = 0x40;
	ioctl(d->spi_fd, SPI_IOC_WR_MODE32, &force_mode);

	size_t off = 0;
	while(off < len) {
		size_t n = len - off;
		if(n > d->chunk_bytes) n = d->chunk_bytes;

		struct spi_ioc_transfer tr;
		memset(&tr, 0, sizeof(tr));
		tr.tx_buf = (unsigned long)(buf + off);
		tr.rx_buf = 0;
		tr.len = (uint32_t)n;
		tr.speed_hz = d->spi_hz;
		tr.bits_per_word = 8;

		if(ioctl(d->spi_fd, SPI_IOC_MESSAGE(1), &tr) < 1) return -1;
		off += n;
	}
	return 0;
}

static int
write_cmd(st7789_t *d, uint8_t cmd)
{
	pi_gpio_set_value(d->cs_pin, 0);
	pi_gpio_set_value(d->dc_pin, 0); /* command */
	int r = spi_write(d, &cmd, 1);
	pi_gpio_set_value(d->cs_pin, 1);
	return r;
}

static int
write_data(st7789_t *d, const uint8_t *data, size_t len)
{
	pi_gpio_set_value(d->cs_pin, 0);
	pi_gpio_set_value(d->dc_pin, 1); /* data */
	int r = spi_write(d, data, len);
	pi_gpio_set_value(d->cs_pin, 1);
	return r;
}

static int
write_data8(st7789_t *d, uint8_t b)
{
	return write_data(d, &b, 1);
}

static uint8_t
madctl_for_rotation(st7789_rotation_t r)
{
	switch(r) {
	case ST7789_ROTATE_0: return MADCTL_RGB;
	case ST7789_ROTATE_90: return (MADCTL_MV | MADCTL_MY) | MADCTL_RGB;
	case ST7789_ROTATE_180: return (MADCTL_MX | MADCTL_MY) | MADCTL_RGB;
	case ST7789_ROTATE_270: return (MADCTL_MV | MADCTL_MX) | MADCTL_RGB;
	default: return MADCTL_RGB;
	}
}

/* Set the GRAM address window [x0,y0]..[x1,y1] inclusive, then issue RAMWR. */
static int
set_window(st7789_t *d, int x0, int y0, int x1, int y1)
{
	x0 += d->x_offset;
	x1 += d->x_offset;
	y0 += d->y_offset;
	y1 += d->y_offset;

	uint8_t buf[4];

	if(write_cmd(d, ST_CMD_CASET) < 0) return -1;
	buf[0] = (uint8_t)(x0 >> 8);
	buf[1] = (uint8_t)x0;
	buf[2] = (uint8_t)(x1 >> 8);
	buf[3] = (uint8_t)x1;
	if(write_data(d, buf, 4) < 0) return -1;

	if(write_cmd(d, ST_CMD_RASET) < 0) return -1;
	buf[0] = (uint8_t)(y0 >> 8);
	buf[1] = (uint8_t)y0;
	buf[2] = (uint8_t)(y1 >> 8);
	buf[3] = (uint8_t)y1;
	if(write_data(d, buf, 4) < 0) return -1;

	return write_cmd(d, ST_CMD_RAMWR);
}

/* ---- init sequence (from verified reference) ---- */

static int
panel_init(st7789_t *d, int invert)
{
	if(d->rst_pin >= 0) {
		pi_gpio_set_value(d->rst_pin, 1);
		delay_ms(5);
		pi_gpio_set_value(d->rst_pin, 0);
		delay_ms(20);
		pi_gpio_set_value(d->rst_pin, 1);
		delay_ms(150);
	} else {
		if(write_cmd(d, ST_CMD_SWRESET) < 0) return -1;
		delay_ms(150);
	}

	if(write_cmd(d, ST_CMD_SLPOUT) < 0) /* sleep out */
		return -1;
	delay_ms(120);

	if(write_cmd(d, ST_CMD_MADCTL) < 0) return -1;
	if(write_data8(d, madctl_for_rotation(d->rotation)) < 0) return -1;

	if(write_cmd(d, ST_CMD_COLMOD) < 0) /* 16-bit/pixel */
		return -1;
	if(write_data8(d, 0x05) < 0) return -1;

	if(d->little_endian) {
		/* RAMCTRL: set little-endian so RGB565 bytes go out LSB-first */
		uint8_t rc[2] = {0x00, 0xF8};
		if(write_cmd(d, ST_CMD_RAMCTRL) < 0) return -1;
		if(write_data(d, rc, 2) < 0) return -1;
	}

	/* Porch / power / gamma: values proven on the reference panel. */
	static const uint8_t seq_b2[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
	write_cmd(d, 0xB2);
	write_data(d, seq_b2, sizeof(seq_b2));
	write_cmd(d, 0xB7);
	write_data8(d, 0x35);
	write_cmd(d, 0xBB);
	write_data8(d, 0x32);
	write_cmd(d, 0xC2);
	write_data8(d, 0x01);
	write_cmd(d, 0xC3);
	write_data8(d, 0x15);
	write_cmd(d, 0xC4);
	write_data8(d, 0x20);
	write_cmd(d, 0xC6);
	write_data8(d, 0x0F);
	write_cmd(d, 0xD0);
	{
		static const uint8_t s[] = {0xA4, 0xA1};
		write_data(d, s, sizeof(s));
	}
	{
		static const uint8_t gpos[] = {0xD0, 0x08, 0x0E, 0x09, 0x09, 0x05, 0x31, 0x33, 0x48, 0x17, 0x14, 0x15, 0x31, 0x34};
		static const uint8_t gneg[] = {0xD0, 0x08, 0x0E, 0x09, 0x09, 0x15, 0x31, 0x33, 0x48, 0x17, 0x14, 0x15, 0x31, 0x34};
		write_cmd(d, 0xE0);
		write_data(d, gpos, sizeof(gpos));
		write_cmd(d, 0xE1);
		write_data(d, gneg, sizeof(gneg));
	}

	if(write_cmd(d, invert ? ST_CMD_INVON : ST_CMD_INVOFF) < 0) return -1;
	if(write_cmd(d, ST_CMD_DISPON) < 0) return -1;

	return 0;
}

/* ---- public API ---- */

void
st7789_config_default(st7789_config_t *cfg)
{
	if(!cfg) return;
	memset(cfg, 0, sizeof(*cfg));
	cfg->rst_pin = 27;
	cfg->cs_pin = 8;
	cfg->dc_pin = 25;
	cfg->blk_pin = 24;
	cfg->spi_dev = "/dev/spidev0.0";
	/*
	 * Request 80MHz -> quantized to core_freq/6 = 66.7MHz (core_freq=400).
	 * This is the highest divisor step verified glitch-free on the panel via
	 * continuous-refresh testing (see docs/06 and pattern.c). 100MHz (400/4)
	 * shows shimmer/instability; 66.7MHz is rock-steady.
	 */
	cfg->spi_hz = 80000000u; // request 80MHz -> HW quantizes to 400/6 = 66.7MHz
	cfg->width = 320;
	cfg->height = 240;
	cfg->rotation = ST7789_ROTATE_90;
	cfg->x_offset = 0;
	cfg->y_offset = 0;
	cfg->chunk_bytes = DEFAULT_CHUNK;
	cfg->little_endian = 1;
	cfg->invert = 1;
}

st7789_t *
st7789_open(const st7789_config_t *cfg)
{
	if(!cfg || cfg->width <= 0 || cfg->height <= 0 || !cfg->spi_dev) {
		errno = EINVAL;
		return NULL;
	}

	st7789_t *d = calloc(1, sizeof(*d));
	if(!d) return NULL;

	d->rst_pin = cfg->rst_pin;
	d->cs_pin = cfg->cs_pin;
	d->dc_pin = cfg->dc_pin;
	d->blk_pin = cfg->blk_pin;
	d->spi_hz = cfg->spi_hz ? cfg->spi_hz : 80000000u;
	d->width = cfg->width;
	d->height = cfg->height;
	d->x_offset = cfg->x_offset;
	d->y_offset = cfg->y_offset;
	d->rotation = cfg->rotation;
	d->chunk_bytes = cfg->chunk_bytes ? cfg->chunk_bytes : DEFAULT_CHUNK;
	d->little_endian = cfg->little_endian;
	d->spi_fd = -1;

	if(pi_gpio_init() < 0) goto fail;

	/* GPIO directions */
	pi_gpio_set_mode(d->cs_pin, PI_GPIO_OUTPUT);
	pi_gpio_set_mode(d->dc_pin, PI_GPIO_OUTPUT);
	pi_gpio_set_value(d->cs_pin, 1);
	if(d->rst_pin >= 0) pi_gpio_set_mode(d->rst_pin, PI_GPIO_OUTPUT);
	if(d->blk_pin >= 0) {
		pi_gpio_set_mode(d->blk_pin, PI_GPIO_OUTPUT);
		pi_gpio_set_value(d->blk_pin, 1); /* backlight on */
	}

	/* SPI setup */
	d->spi_fd = open(cfg->spi_dev, O_RDWR);
	if(d->spi_fd < 0) {
		perror("st7789: open spidev");
		goto fail;
	}
	{
		/* Use SPI_IOC_WR_MODE32 and raw bit values to avoid sysroot/kernel
		 * header mismatch. On kernel >=6.x SPI_NO_CS = bit6 = 0x40;
		 * older headers have it as 0x04 which is SPI_CS_HIGH on new kernels.
		 * Explicitly set MODE0 | NO_CS using the runtime kernel value. */
		uint32_t mode32 = 0x40; /* SPI_NO_CS, kernel >=6.x value */
		uint8_t bits = 8;
		if(ioctl(d->spi_fd, SPI_IOC_WR_MODE32, &mode32) < 0) {
			uint8_t mode8 = 0x40;
			if(ioctl(d->spi_fd, SPI_IOC_WR_MODE, &mode8) < 0) perror("st7789: SPI_IOC_WR_MODE");
		}
		if(ioctl(d->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) perror("st7789: SPI_IOC_WR_BITS_PER_WORD");
		if(ioctl(d->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &d->spi_hz) < 0) perror("st7789: SPI_IOC_WR_MAX_SPEED_HZ");
	}

	/* Clamp chunk to the kernel's spidev bufsiz: a transfer larger than bufsiz
	 * fails with EMSGSIZE. Without this, large frames silently fail on systems
	 * where bufsiz is left at the 4096 default. */
	{
		uint32_t bufsiz = read_spidev_bufsiz();
		if(d->chunk_bytes > bufsiz) {
			fprintf(stderr,
			        "st7789: chunk %u > spidev bufsiz %u; clamping to %u "
			        "(raise it via 'spidev.bufsiz=...' boot cmdline for best perf)\n",
			        d->chunk_bytes, bufsiz, bufsiz);
			d->chunk_bytes = bufsiz;
		}
	}

	if(panel_init(d, cfg->invert) < 0) goto fail;

	/* scratch line buffer for fills */
	d->linebuf = malloc((size_t)d->width * 2);
	if(!d->linebuf) goto fail;

	st7789_fill(d, 0x0000);
	return d;

fail: {
	int e = errno;
	if(d->spi_fd >= 0) close(d->spi_fd);
	free(d->linebuf);
	free(d);
	errno = e;
	return NULL;
}
}

void
st7789_close(st7789_t *d)
{
	if(!d) return;
	if(d->blk_pin >= 0) pi_gpio_set_value(d->blk_pin, 0);
	if(d->spi_fd >= 0) close(d->spi_fd);
	free(d->linebuf);
	free(d);
}

int
st7789_get_width(const st7789_t *d)
{
	return d ? d->width : 0;
}
int
st7789_get_height(const st7789_t *d)
{
	return d ? d->height : 0;
}

int
st7789_flush(st7789_t *d, const uint16_t *rgb565)
{
	if(!d || !rgb565) return -1;

	/* Re-assert SPI hardware config + RAMCTRL before every flush.
	 *
	 * Root cause: on Raspberry Pi with kernel 6.x, the VC4 V3D GPU driver
	 * (vc4/v3d) powers up the V3D peripheral and reconfigures PLLs/clocks via
	 * bcm2835_pll_set_rate during eglInitialize / eglMakeCurrent. This triggers
	 * bcm2835_spi_set_cs / bcm2835_spi_reset_hw in the spi-bcm2835 driver,
	 * which resets the SPI CS register (SPI_CS) including the clock polarity
	 * and the DOEN/DOUT bits. The spidev file descriptor retains a stale view
	 * of the speed and mode, so subsequent SPI_IOC_MESSAGE calls go through
	 * with the hardware in a partially reset state: wrong clock phase or byte
	 * order -> wrong colors on the panel.
	 *
	 * Fix: re-issue SPI_IOC_WR_MODE32 + SPI_IOC_WR_MAX_SPEED_HZ before every
	 * flush to restore the exact state we need, and re-send RAMCTRL to the
	 * panel so its LSB-first pixel interpretation is active. This is <1μs of
	 * overhead vs ~15ms of SPI pixel data, so the cost is negligible.
	 *
	 * Note: SPI_NO_CS = bit6 = 0x40 on kernel >=6.x (old headers had 0x04
	 * which is SPI_CS_HIGH). We use the raw value to avoid cross-compile
	 * sysroot/kernel header mismatch. */
	{
		uint32_t mode32 = 0x40; /* SPI_NO_CS, kernel>=6.x */
		uint8_t bits = 8;
		ioctl(d->spi_fd, SPI_IOC_WR_MODE32, &mode32);
		ioctl(d->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
		ioctl(d->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &d->spi_hz);
	}
	if(d->little_endian) {
		uint8_t rc[2] = {0x00, 0xF8};
		write_cmd(d, ST_CMD_RAMCTRL);
		write_data(d, rc, 2);
	}

	if(set_window(d, 0, 0, d->width - 1, d->height - 1) < 0) return -1;
	return write_data(d, (const uint8_t *)rgb565, (size_t)d->width * d->height * 2);
}

int
st7789_flush_area(st7789_t *d, int x, int y, int w, int h, const uint16_t *rgb565)
{
	if(!d || !rgb565 || w <= 0 || h <= 0) return -1;
	if(x < 0 || y < 0 || x + w > d->width || y + h > d->height) return -1;
	if(set_window(d, x, y, x + w - 1, y + h - 1) < 0) return -1;
	return write_data(d, (const uint8_t *)rgb565, (size_t)w * h * 2);
}

int
st7789_fill(st7789_t *d, uint16_t color)
{
	if(!d) return -1;
	if(set_window(d, 0, 0, d->width - 1, d->height - 1) < 0) return -1;

	uint16_t *row = (uint16_t *)d->linebuf;
	for(int i = 0; i < d->width; i++) row[i] = color;

	pi_gpio_set_value(d->cs_pin, 0);
	pi_gpio_set_value(d->dc_pin, 1);
	int ret = 0;
	for(int y = 0; y < d->height; y++) {
		if(spi_write(d, d->linebuf, (size_t)d->width * 2) < 0) {
			ret = -1;
			break;
		}
	}
	pi_gpio_set_value(d->cs_pin, 1);
	return ret;
}

void
st7789_set_backlight(st7789_t *d, int on)
{
	if(d && d->blk_pin >= 0) pi_gpio_set_value(d->blk_pin, on ? 1 : 0);
}

int
st7789_tune_system(uint32_t want_bufsiz)
{
	int rc = 0;

	/* CPU governor -> performance (key optimization, see docs/06) */
	for(int cpu = 0;; cpu++) {
		char path[128];
		snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);
		FILE *f = fopen(path, "w");
		if(!f) {
			if(cpu == 0) rc = -1; /* couldn't set even cpu0 */
			break;                /* no more CPUs */
		}
		if(fputs("performance", f) < 0) rc = -1;
		fclose(f);
	}

	/* spidev bufsiz is a module load-time param; can't change at runtime via
	 * sysfs. Report if the current value is smaller than requested so the
	 * integrator can add spidev.bufsiz=... to the boot cmdline. */
	if(want_bufsiz) {
		FILE *f = fopen("/sys/module/spidev/parameters/bufsiz", "r");
		if(f) {
			long cur = 0;
			if(fscanf(f, "%ld", &cur) == 1 && (uint32_t)cur < want_bufsiz) {
				fprintf(stderr,
				        "st7789: spidev bufsiz=%ld < %u; add 'spidev.bufsiz=%u' to "
				        "boot cmdline for large transfers\n",
				        cur, want_bufsiz, want_bufsiz);
				rc = -1;
			}
			fclose(f);
		}
	}

	return rc;
}

/*
 * Firmware loader for Samsung I9100 and I9250
 * Copyright (C) 2012 Alexander Tarasikov <alexander.tarasikov@gmail.com>
 *
 * based on the incomplete C++ implementation which is
 * Copyright (C) 2012 Sergey Gridasov <grindars@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <getopt.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

//for timeval
#include <sys/time.h>

//for mmap
#include <sys/mman.h>
#include <sys/stat.h>

#include "modem_prj.h"

#if 0
TODO:

1. I9100/I9250 detection (probe)
2. I9250 firmware offsets
3. integrate with libsamsung-ipc @ replicant/FSO

#endif

/*
 * IO helper functions
 */

#define DEBUG 1

#ifndef SILENT
	#define LOG_TAG "xmm6260-sec"
	#define _p(fmt, x...) \
		do {\
			printf("[" LOG_TAG "]: " fmt "\n", ##x); \
		} while (0)
#else
	#define _p(fmt, x...) do {} while (0)
#endif

#ifdef DEBUG
	#define _d(fmt, x...) _p("D/" fmt, ##x)
#else
	#define _d(fmt, x...) do {} while (0)
#endif

#define _e(fmt, x...) _p("E/" fmt, ##x)
#define _i(fmt, x...) _p("I/" fmt, ##x)

static int c_ioctl(int fd, unsigned long code, void* data) {
	int ret;

	if (!data) {
		ret = ioctl(fd, code);
	}
	else {
		ret = ioctl(fd, code, data);
	}

	if (ret < 0) {
		_e("ioctl fd=%d code=%lx failed: %s", fd, code, strerror(errno));
	}
	else {
		_d("ioctl fd=%d code=%lx OK", fd, code);
	}

	return ret;
}

static inline int read_select(int fd, unsigned timeout) {
	struct timeval tv = {
		tv.tv_sec = timeout / 1000,
		tv.tv_usec = 1000 * (timeout % 1000),
	};

	fd_set read_set;
	FD_ZERO(&read_set);
	FD_SET(fd, &read_set);

	return select(fd + 1, &read_set, 0, 0, &tv);
}

static inline int receive(int fd, void *buf, size_t size) {
	int ret;
	if ((ret = read_select(fd, 0)) < 0) {
		_e("%s: failed to select the fd %d", __func__, fd);
		return ret;
	}
	else {
		_d("%s: selected fd %d for %d", __func__, ret, fd);
	}

	return read(fd, buf, size);
}

static int expect_data(int fd, void *data, size_t size) {
	int ret;
	char buf[size];
	if ((ret = receive(fd, buf, size)) < 0) {
		_e("failed to receive data");
		return ret;
	}
	ret = memcmp(buf, data, size);
	
	if (ret != 0) {
		_d("received %02x", buf[0]);
	}

	return ret;
}

/*
 * I9100 specific implementation
 */
#define MODEM_DEVICE(x) ("/dev/" #x)
#define LINK_PM MODEM_DEVICE(link_pm)
#define MODEM_DEV MODEM_DEVICE(modem_br)
#define BOOT_DEV MODEM_DEVICE(umts_boot0)
#define IPC_DEV MODEM_DEVICE(umts_ipc0)
#define RFS_DEV MODEM_DEVICE(umts_rfs0)

#define RADIO_IMAGE "/dev/block/mmcblk0p8"
#define NVDATA_IMAGE "/efs/nv_data.bin"

#define I9100_EHCI_PATH "/sys/devices/platform/s5p-ehci/ehci_power"


#define RADIO_MAP_SIZE (16 << 20)

/*
 * Components of the Samsung XMM6260 firmware
 */
enum xmm6260_image {
	PSI,
	EBL,
	SECURE_IMAGE,
	FIRMWARE,
	NVDATA,
};

/*
 * Locations of the firmware components in the Samsung firmware
 */
static struct xmm6260_offset {
	size_t offset;
	size_t length;
} i9100_radio_parts[] = {
	[PSI] = {
		.offset = 0,
		.length = 0xf000,
	},
	[EBL] = {
		.offset = 0xf000,
		.length = 0x19000,
	},
	[SECURE_IMAGE] = {
		.offset = 0x9ff800,
		.length = 0x800,
	},
	[FIRMWARE] = {
		.offset = 0x28000,
		.length = 0x9d8000,
	},
	[NVDATA] = {
		.offset = 0x6406e00,
		.length = 2 << 20,
	}
};

/*
 * Bootloader control interface definitions
 */

enum xmm6260_boot_cmd {
	SetPortConf        = 0x86,

	ReqSecStart        = 0x204,
	ReqSecEnd          = 0x205,
	ReqForceHwReset    = 0x208,

	ReqFlashSetAddress = 0x802,
	ReqFlashWriteBlock = 0x804
};

#define XMM_PSI_MAGIC 0x30

typedef struct {
	uint8_t magic;
	uint16_t length;
	uint8_t padding;
} __attribute__((packed)) psi_header_t;

static int link_fd;
static int boot_fd;

static int radio_fd;
static char *radio_data;
struct stat radio_stat;

static int i9100_ehci_setpower(bool enabled) {
	int ret;
	
	_d("%s: enabled=%d", __func__, enabled);
	
	int ehci_fd = open(I9100_EHCI_PATH, O_RDWR);
	if (ehci_fd < 0) {
		_e("failed to open EHCI fd");
		goto fail;
	}
	else {
		_d("opened EHCI %s: fd=%d", I9100_EHCI_PATH, ehci_fd);
	}

	ret = write(ehci_fd, enabled ? "1" : "0", 1);

	//must write exactly one byte
	if (ret <= 0) {
		_e("failed to set EHCI power");
	}
	else {
		_d("set EHCI power");
	}

fail:
	if (ehci_fd >= 0) {
		close(ehci_fd);
	}

	return ret;
}

static int i9100_link_set_active(bool enabled) {
	unsigned status = enabled;
	int ret;
	unsigned long ioctl_code;

	ioctl_code = IOCTL_LINK_CONTROL_ENABLE;
	ret = c_ioctl(link_fd, ioctl_code, &status);

	if (ret < 0) {
		_d("failed to set link state to %d", enabled);
		goto fail;
	}

	ioctl_code = IOCTL_LINK_CONTROL_ACTIVE;
	ret = c_ioctl(link_fd, ioctl_code, &status);

	if (ret < 0) {
		_d("failed to set link active to %d", enabled);
		goto fail;
	}

	return 0;
fail:
	return ret;
}

static int i9100_wait_link_ready(void) {
	int ret;

	while (1) {
		ret = c_ioctl(link_fd, IOCTL_LINK_CONNECTED, 0);
		if (ret < 0) {
			goto fail;
		}

		if (ret == 1) {
			break;
		}

		usleep(50 * 1000);
	}
	
	return 0;

fail:
	return ret;
}

static int xmm6260_setpower(bool enabled) {
	if (enabled) {
		return c_ioctl(boot_fd, IOCTL_MODEM_ON, 0);
	}
	else {
		return c_ioctl(boot_fd, IOCTL_MODEM_OFF, 0);
	}
	return -1;
}

static unsigned char calculateCRC(void* data,
	size_t offset, size_t length)
{
	unsigned char crc = 0;
	unsigned char *ptr = (unsigned char*)(data + offset);
	
	while (length--) {
		crc ^= *ptr++;
	}

	return crc;
}

static int send_PSI(int fd) {
	size_t length = i9100_radio_parts[PSI].length;
	size_t offset = i9100_radio_parts[PSI].offset;

	psi_header_t hdr = {
		.magic = XMM_PSI_MAGIC,
		.length = length,
	};
	int ret = -1;
	
	if ((ret = write(fd, &hdr, sizeof(hdr))) != sizeof(hdr)) {
		_d("%s: failed to write header, ret %d", __func__, ret);
		goto fail;
	}

	size_t start = offset;
	size_t end = length + start;

	//dump some image bytes
	unsigned *u = (unsigned*)(radio_data + start);
	_d("PSI image [%08x %08x %08x %08x]", u[0], u[1], u[2], u[3]);

	while (start < end) {
		size_t written = write(fd, radio_data + start, end - offset);
		if (written < 0) {
			_d("failed to write PSI chunk");
			goto fail;
		}
		start += written;
	}

	unsigned char crc = calculateCRC(radio_data, offset, length);
	
	if ((ret = write(fd, &crc, 1)) < 1) {
		_d("failed to write CRC");
		goto fail;
	}

	for (int i = 0; i < 22; i++) {
		char ack;
		if (receive(fd, &ack, 1) < 1) {
			_d("failed to read ACK byte %d", i);
			goto fail;
		}
		_d("%02x ", ack);
	}

	unsigned char ack = 0x1;
	if ((ret = expect_data(fd, &ack, 1)) < 0) {
		_d("failed to wait for first ACK");
		goto fail;
	}

	if ((ret = expect_data(fd, &ack, 1)) < 0) {
		_d("failed to wait for second ACK");
		goto fail;
	}

	return 0;

fail:
	return ret;
}

int main(int argc, char** argv) {
	radio_fd = open(RADIO_IMAGE, O_RDONLY);
	if (radio_fd < 0) {
		_e("failed to open radio firmware");
		goto fail;
	}
	else {
		_d("opened radio image %s, fd=%d", RADIO_IMAGE, radio_fd);
	}

	if (fstat(radio_fd, &radio_stat) < 0) {
		_e("failed to stat radio image, error %s", strerror(errno));
		goto fail;
	}

	radio_data = mmap(0, RADIO_MAP_SIZE, PROT_READ, MAP_SHARED,
		radio_fd, 0);
	if (radio_data == MAP_FAILED) {
		_e("failed to mmap radio image, error %s", strerror(errno));
		goto fail;
	}

	boot_fd = open(BOOT_DEV, O_RDWR);
	if (boot_fd < 0) {
		_e("failed to open boot device");
		goto fail;
	}
	else {
		_d("opened boot device %s, fd=%d", BOOT_DEV, boot_fd);
	}

	link_fd = open(LINK_PM, O_RDWR);
	if (link_fd < 0) {
		_e("failed to open link device");
		goto fail;
	}
	else {
		_d("opened link device %s, fd=%d", LINK_PM, link_fd);
	}

	/*
	 * Disable the hardware to ensure consistent state
	 */
	
	if (xmm6260_setpower(false) < 0) {
		_e("failed to disable xmm6260 power");
	}
	else {
		_d("disabled xmm6260 power");
	}

	if (i9100_link_set_active(false) < 0) {
		_e("failed to disable I9100 HSIC link");
	}
	else {
		_d("disabled I9100 HSIC link");
	}

	if (i9100_ehci_setpower(false) < 0) {
		_e("failed to disable I9100 EHCI");
	}
	else {
		_d("disabled I9100 EHCI");
	}

	/*
	 * Now, initialize the hardware
	 */

	if (i9100_link_set_active(true) < 0) {
		_e("failed to enable I9100 HSIC link");
	}
	else {
		_d("enabled I9100 HSIC link");
	}

	if (i9100_ehci_setpower(true) < 0) {
		_e("failed to enable I9100 EHCI");
		goto fail;
	}
	else {
		_d("enabled I9100 EHCI");
	}

	if (xmm6260_setpower(true) < 0) {
		_e("failed to enable xmm6260 power");
		goto fail;
	}
	else {
		_d("enabled xmm6260 power");
	}

	if (i9100_wait_link_ready() < 0) {
		_e("failed to wait for link to get ready");
		goto fail;
	}
	else {
		_d("link ready");
	}
	
	usleep(500 * 1000);

	/*
	 * Now, actually load the firmware
	 */
	if (write(boot_fd, "ATAT", 4) != 4) {
		_e("failed to write ATAT to boot socket");
		goto fail;
	}
	else {
		_d("written ATAT to boot socket, waiting for ACK");
	}
	
	usleep(500 * 1000);

	char buf[2];
	if (receive(boot_fd, buf, 1) < 0) {
		_e("failed to receive bootloader ACK");
		goto fail;
	}
	if (receive(boot_fd, buf + 1, 1) < 0) {
		_e("failed to receive chip IP ACK");
		goto fail;
	}
	_i("receive ID: [%02x %02x]", buf[0], buf[1]);

	if (send_PSI(boot_fd) < 0) {
		_e("failed to upload PSI");
		goto fail;
	}

	buf[0] = 0x00;
	buf[1] = 0xAA;
	if (expect_data(boot_fd, buf, 2) < 0) {
		_e("failed to receive PSI ACK");
		goto fail;
	}

	/*
	 * send PSI
	 * ack 0x00 0xaa
	 * send EBL
	 * send Secure
	 * reboot
	 */

fail:
	if (radio_data != MAP_FAILED) {
		munmap(radio_data, RADIO_MAP_SIZE);
	}

	if (link_fd >= 0) {
		close(link_fd);
	}

	if (radio_fd >= 0) {
		close(radio_fd);
	}

	if (boot_fd >= 0) {
		close(boot_fd);
	}

	return 0;
}
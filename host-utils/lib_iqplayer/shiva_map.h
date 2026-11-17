/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 NXP
 *
 * shiva_map.h - map LA9310 host memory regions (scratch, IQFLOOD, PCIe BARs)
 * into userspace.
 *
 * The regions are mapped through /dev/shiva<N> (la9310shiva modinfo misc
 * device), so the host tools work on kernels built with
 * CONFIG_STRICT_DEVMEM=y. The mmap offset is the host physical address
 * reported by IOCTL_LA93XX_MODINFO_GET.
 *
 * Drivers without the la9310shiva modinfo mmap patch refuse some regions
 * on /dev/shiva<N> (older NXP drivers: everything but scratch; RFNM driver
 * with patch 0004: the PCIe BARs). Such a region is mapped through /dev/mem
 * instead and a note is printed once. That fallback only works on kernels
 * without CONFIG_STRICT_DEVMEM: STRICT_DEVMEM refuses RAM (scratch, IQFLOOD)
 * and CONFIG_IO_STRICT_DEVMEM also refuses the PCIe BARs, because
 * la9310shiva claims them.
 */
#ifndef SHIVA_MAP_H
#define SHIVA_MAP_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#ifndef SHIVA_MAP_DEV
#define SHIVA_MAP_DEV "/dev/shiva0"
#endif

static inline void *shiva_map_region(const char *name, uint64_t phys,
				     size_t size)
{
	static int warned;
	void *p = MAP_FAILED;
	int fd, err = 0;

	fd = open(SHIVA_MAP_DEV, O_RDWR);
	if (fd >= 0) {
		p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			 (off_t)phys);
		err = errno;
		close(fd);
		if (p != MAP_FAILED)
			return p;
	} else {
		err = errno;
	}

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd >= 0) {
		p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			 (off_t)phys);
		close(fd);
	}
	if (p == MAP_FAILED) {
		fprintf(stderr,
			"map %s 0x%llx size 0x%zx failed: %s %s, /dev/mem %s\n"
			"  la9310shiva needs the modinfo mmap patch "
			"(doc/patches/la93xx_host_sw or la9310-driver/0004)\n",
			name, (unsigned long long)phys, size, SHIVA_MAP_DEV,
			strerror(err), strerror(errno));
		return MAP_FAILED;
	}
	if (!warned) {
		fprintf(stderr,
			"note: %s not mappable via %s (%s), using /dev/mem"
			" (fails with CONFIG_STRICT_DEVMEM)\n",
			name, SHIVA_MAP_DEV, strerror(err));
		warned = 1;
	}
	return p;
}

#endif /* SHIVA_MAP_H */

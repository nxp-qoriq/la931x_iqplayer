/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 NXP
 *
 * memtool - read/write/zero physical memory via /dev/mem.
 *
 * Supports the same command-line syntax as the NXP BSP memtool utility
 * plus a zero-fill extension used by load-rfnm.sh to clear the VSPA
 * DDR proxy region before the firmware starts.
 *
 * Read syntax:
 *   memtool [-32] <phys_addr> [count]
 *     Print <count> 32-bit words (default 1) starting at <phys_addr>,
 *     4 words per line.  -32 / -16 / -8 are accepted and ignored.
 *
 * Write syntax:
 *   memtool [-32] <phys_addr>=<value>
 *     Write one 32-bit word at <phys_addr>.
 *
 * Zero syntax (extra, no standard memtool equivalent):
 *   memtool -a <phys_addr> -z <size>
 *     Zero <size> bytes starting at <phys_addr> via /dev/mem.
 *     <size> must be a multiple of 4.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>

#define PAGE_SIZE   4096UL
#define PAGE_MASK   (~(PAGE_SIZE - 1))

static void usage(void)
{
    fprintf(stderr,
        "Usage:\n"
        "  Read memory:  memtool [-32] <phys_addr> [count]\n"
        "  Write memory: memtool [-32] <phys_addr>=<value>\n"
        "  Zero memory:  memtool -a <phys_addr> -z <size>\n"
        "\n"
        "  phys_addr and value may be hex (0x...) or decimal.\n"
        "  count is in 32-bit words (default 1).\n"
        "  size for -z must be > 0 and a multiple of 4.\n");
}

/* Map <size> bytes at page-aligned base covering <addr>.
 * base_offset is subtracted from addr before computing the mmap file offset.
 * For /dev/mem pass base_offset=0 (file offset == physical address).
 * For /dev/shiva0 (iqflood carveout) pass base_offset=carveout_phys_base
 * so the mmap offset is relative to the start of the device's mapping window.
 * Returns mmap pointer (MAP_FAILED on error), sets *offset_out to the
 * byte offset of <addr> within the returned mapping. */
static void *devmem_map(int fd, uint64_t addr, size_t size, uint64_t *offset_out,
                        uint64_t base_offset, int prot)
{
    uint64_t file_addr = addr - base_offset;
    uint64_t map_base  = file_addr & PAGE_MASK;
    uint64_t map_end   = (file_addr + size + PAGE_SIZE - 1) & PAGE_MASK;
    size_t   map_size  = (size_t)(map_end - map_base);
    void *ptr = mmap(NULL, map_size, prot, MAP_SHARED,
                     fd, (off_t)map_base);
    if (ptr == MAP_FAILED)
        return MAP_FAILED;
    *offset_out = file_addr - map_base;
    return ptr;
}

int main(int argc, char *argv[])
{
    int i = 1;
    const char *g_dev = "/dev/mem";
    uint64_t g_base = 0;

    /* Parse optional -d <device> to override /dev/mem (e.g. /dev/shiva0) */
    if (i < argc && strcmp(argv[i], "-d") == 0) {
        if (i + 1 >= argc) { usage(); return 1; }
        g_dev = argv[i + 1];
        i += 2;
    }

    /* Parse optional -b <base> for carveout-relative mmap offset.
     * When using /dev/shiva0 the driver maps from offset 0 == carveout base,
     * so subtract the carveout physical base before computing the page offset.
     * Example: -d /dev/shiva0 -b 0x96400000 */
    if (i < argc && strcmp(argv[i], "-b") == 0) {
        if (i + 1 >= argc) { usage(); return 1; }
        g_base = strtoull(argv[i + 1], NULL, 0);
        i += 2;
    }

    /* Skip optional -32 / -16 / -8 width flags */
    if (i < argc && (strcmp(argv[i], "-32") == 0 ||
                     strcmp(argv[i], "-16") == 0 ||
                     strcmp(argv[i], "-8")  == 0))
        i++;

    /* Zero-fill mode: memtool -a <addr> -z <size> */
    if (i < argc && strcmp(argv[i], "-a") == 0) {
        if (i + 3 >= argc || strcmp(argv[i + 2], "-z") != 0) {
            fprintf(stderr, "memtool: -a requires: -a <addr> -z <size>\n");
            usage();
            return 1;
        }
        uint64_t addr = strtoull(argv[i + 1], NULL, 0);
        long long size_ll = strtoll(argv[i + 3], NULL, 0);
        if (size_ll <= 0 || (size_ll % 4) != 0) {
            fprintf(stderr, "memtool: -z size must be > 0 and a multiple of 4\n");
            return 1;
        }
        size_t zsize = (size_t)size_ll;

        /* Use devmem_map() + memset() for the zero-fill path.
         * pwrite() is not implemented by all device drivers (e.g. /dev/shiva0
         * returns EINVAL); mmap + memset works for any device that supports
         * MAP_SHARED mmap, which all iqflood carveout drivers do. */
        int fd = open(g_dev, O_RDWR | O_SYNC);
        if (fd < 0) {
            fprintf(stderr, "memtool: open %s: %s\n", g_dev, strerror(errno));
            return 1;
        }
        uint64_t zoffset;
        void *zptr = devmem_map(fd, addr, zsize, &zoffset, g_base,
                                PROT_READ | PROT_WRITE);
        if (zptr == MAP_FAILED) {
            fprintf(stderr, "memtool: mmap %s at 0x%llx size %zu: %s\n",
                    g_dev, (unsigned long long)addr, zsize, strerror(errno));
            close(fd);
            return 1;
        }
        memset((char *)zptr + zoffset, 0, zsize);
        uint64_t zmap_base = (addr - g_base) & PAGE_MASK;
        uint64_t zmap_end  = (addr - g_base + zsize + PAGE_SIZE - 1) & PAGE_MASK;
        munmap(zptr, (size_t)(zmap_end - zmap_base));
        close(fd);
        printf("memtool: zeroed %zu bytes at phys=0x%llx\n",
               zsize, (unsigned long long)addr);
        return 0;
    }

    /* Read / write mode */
    if (i >= argc) {
        usage();
        return 1;
    }

    /* Check for write: <addr>=<val> */
    char *eq = strchr(argv[i], '=');
    if (eq) {
        *eq = '\0';
        uint64_t addr = strtoull(argv[i], NULL, 0);
        uint32_t val  = (uint32_t)strtoul(eq + 1, NULL, 0);

        int fd = open(g_dev, O_RDWR | O_SYNC);
        if (fd < 0) {
            fprintf(stderr, "memtool: open %s: %s\n", g_dev, strerror(errno));
            return 1;
        }
        uint64_t offset;
        void *ptr = devmem_map(fd, addr, 4, &offset, g_base,
                               PROT_READ | PROT_WRITE);
        if (ptr == MAP_FAILED) {
            fprintf(stderr, "memtool: mmap %s at 0x%llx: %s\n", g_dev,
                    (unsigned long long)addr, strerror(errno));
            close(fd);
            return 1;
        }
        *((volatile uint32_t *)((char *)ptr + offset)) = val;
        uint64_t map_base = (addr - g_base) & PAGE_MASK;
        uint64_t map_end  = (addr - g_base + 4 + PAGE_SIZE - 1) & PAGE_MASK;
        munmap(ptr, (size_t)(map_end - map_base));
        close(fd);
        printf("memtool: wrote 0x%08X at phys=0x%llx\n",
               val, (unsigned long long)addr);
        return 0;
    }

    /* Read: <addr> [count] */
    uint64_t addr  = strtoull(argv[i], NULL, 0);
    uint32_t count = (i + 1 < argc) ? (uint32_t)strtoul(argv[i + 1], NULL, 0) : 1;
    if (count == 0)
        count = 1;
    size_t rsize = (size_t)count * 4;

    int fd = open(g_dev, O_RDONLY | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "memtool: open %s: %s\n", g_dev, strerror(errno));
        return 1;
    }
    uint64_t offset;
    void *ptr = devmem_map(fd, addr, rsize, &offset, g_base, PROT_READ);
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "memtool: mmap %s at 0x%llx: %s\n", g_dev,
                (unsigned long long)addr, strerror(errno));
        close(fd);
        return 1;
    }

    volatile uint32_t *p = (volatile uint32_t *)((char *)ptr + offset);
    for (uint32_t j = 0; j < count; j++) {
        if ((j % 4) == 0)
            printf("0x%llX: ", (unsigned long long)(addr + j * 4));
        printf(" %08X", p[j]);
        if ((j % 4) == 3 || j == count - 1)
            printf("\n");
    }

    uint64_t map_base = (addr - g_base) & PAGE_MASK;
    uint64_t map_end  = (addr - g_base + rsize + PAGE_SIZE - 1) & PAGE_MASK;
    munmap(ptr, (size_t)(map_end - map_base));
    close(fd);
    return 0;
}

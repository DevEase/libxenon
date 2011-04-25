/* modified to fit in libxenon */

/* ata.c - ATA disk access.  */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2007  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <time/time.h>
#include "ata.h"

//No printf
//#define printf(...)

struct xenon_ata_device ata;
struct xenon_ata_device atapi;

static inline uint16_t bswap16(uint16_t t) {
    return ((t & 0xFF) << 8) | ((t & 0xFF00) >> 8);
}

static inline void
xenon_ata_regset(struct xenon_ata_device *dev, int reg, uint8_t val) {
    *(volatile uint8_t*)(dev->ioaddress + reg) = val;
}

static inline uint8_t
xenon_ata_regget(struct xenon_ata_device *dev, int reg) {
    return *(volatile uint8_t*)(dev->ioaddress + reg);
}

static inline void
xenon_ata_regset2(struct xenon_ata_device *dev, int reg, int val) {
    *(volatile uint8_t*)(dev->ioaddress2 + reg) = val;
}

static inline int
xenon_ata_regget2(struct xenon_ata_device *dev, int reg) {
    return *(volatile uint8_t*)(dev->ioaddress2 + reg);
}

static inline uint32_t
xenon_ata_read_data(struct xenon_ata_device *dev, int reg) {
    return *(volatile uint32_t*)(dev->ioaddress + reg);
}

static inline void
xenon_ata_write_data(struct xenon_ata_device *dev, int reg, uint32_t val) {
    *(volatile uint32_t*)(dev->ioaddress + reg) = val;
}

static inline void
xenon_ata_wait_busy(struct xenon_ata_device *dev) {
    while ((xenon_ata_regget(dev, XENON_ATA_REG_STATUS) & 0x80));
}

static inline void
xenon_ata_wait_drq(struct xenon_ata_device *dev) {
    while (!(xenon_ata_regget(dev, XENON_ATA_REG_STATUS) & 0x08));
}

static inline void
xenon_ata_wait_ready(struct xenon_ata_device *dev) {
    while ((xenon_ata_regget(dev, XENON_ATA_REG_STATUS) & 0xC0)!=0x40);
}

static inline void
xenon_ata_wait() {
	udelay(50);
}

static int
xenon_ata_pio_read(struct xenon_ata_device *dev, char *buf, int size) {
    uint32_t *buf32 = (uint32_t *) buf;
    unsigned int i;

    if (xenon_ata_regget(dev, XENON_ATA_REG_STATUS) & 1)
        return xenon_ata_regget(dev, XENON_ATA_REG_ERROR);

    /* Wait until the data is available.  */
    xenon_ata_wait_drq(dev);

    /* Read in the data, word by word.  */
    for (i = 0; i < size / 4; i++)
        buf32[i] = xenon_ata_read_data(dev, XENON_ATA_REG_DATA);

    if (xenon_ata_regget(dev, XENON_ATA_REG_STATUS) & 1)
        return xenon_ata_regget(dev, XENON_ATA_REG_ERROR);

    return 0;
}

static int
xenon_ata_pio_write(struct xenon_ata_device *dev, char *buf, int size) {
    uint32_t *buf32 = (uint32_t *) buf;
    unsigned int i;

    /* Wait until the device is ready to write.  */
    xenon_ata_wait_drq(dev);

    /* Write the data, word by word.  */
    for (i = 0; i < size / 4; i++)
        xenon_ata_write_data(dev, XENON_ATA_REG_DATA, buf32[i]);

    if (xenon_ata_regget(dev, XENON_ATA_REG_STATUS) & 1)
        return xenon_ata_regget(dev, XENON_ATA_REG_ERROR);

    return 0;
}

static void
xenon_ata_dumpinfo(struct xenon_ata_device *dev, char *info) {
    char text[41];
	char data[0x80];
	int i;
	
	for(i=0;i<sizeof(data);i+=2){
		data[i]=info[i+1];
		data[i+1]=info[i];
	}

    /* The device information was read, dump it for debugging.  */
    strncpy(text, data + 20, 20);
    text[20] = 0;
    printf("  * Serial: %s\n", text);
    strncpy(text, data + 46, 8);
    text[8] = 0;
    printf("  * Firmware: %s\n", text);
    strncpy(text, data + 54, 40);
    text[40] = 0;
    printf("  * Model: %s\n", text);

	if (!dev->atapi){
		printf("  * Addressing mode: %d\n", dev->addressing_mode);
		printf("  * #cylinders: %d\n", (int) dev->cylinders);
		printf("  * #heads: %d\n", (int) dev->heads);
		printf("  * #sectors: %d\n", (int) dev->size);
	}
}

static void
xenon_ata_setlba(struct xenon_ata_device *dev, int sector, int size) {
    xenon_ata_regset(dev, XENON_ATA_REG_SECTORS, size);
    xenon_ata_regset(dev, XENON_ATA_REG_LBALOW, sector & 0xFF);
    xenon_ata_regset(dev, XENON_ATA_REG_LBAMID, (sector >> 8) & 0xFF);
    xenon_ata_regset(dev, XENON_ATA_REG_LBAHIGH, (sector >> 16) & 0xFF);
}

static int
xenon_ata_setaddress(struct xenon_ata_device *dev, int sector, int size) {
    xenon_ata_wait_busy(dev);

    switch (dev->addressing_mode) {
        case XENON_ATA_CHS:
        {
            unsigned int cylinder;
            unsigned int head;
            unsigned int sect;

            /* Calculate the sector, cylinder and head to use.  */
            sect = ((uint32_t) sector % dev->sectors_per_track) + 1;
            cylinder = (((uint32_t) sector / dev->sectors_per_track)
                    / dev->heads);
            head = ((uint32_t) sector / dev->sectors_per_track) % dev->heads;

            if (sect > dev->sectors_per_track
                    || cylinder > dev->cylinders
                    || head > dev->heads) {
                printf("sector %d can not be addressed "
                        "using CHS addressing", sector);
                return -1;
            }

            xenon_ata_regset(dev, XENON_ATA_REG_SECTNUM, sect);
            xenon_ata_regset(dev, XENON_ATA_REG_CYLLSB, cylinder & 0xFF);
            xenon_ata_regset(dev, XENON_ATA_REG_CYLMSB, cylinder >> 8);
            xenon_ata_regset(dev, XENON_ATA_REG_DISK, head);

            break;
        }

        case XENON_ATA_LBA:
            if (size == 256)
                size = 0;
            xenon_ata_setlba(dev, sector, size);
            xenon_ata_regset(dev, XENON_ATA_REG_DISK,
                    0xE0 | ((sector >> 24) & 0x0F));
            break;

        case XENON_ATA_LBA48:
            if (size == 65536)
                size = 0;

            /* Set "Previous".  */
            xenon_ata_setlba(dev, sector >> 24, size >> 8);
            /* Set "Current".  */
            xenon_ata_setlba(dev, sector, size);
            xenon_ata_regset(dev, XENON_ATA_REG_DISK, 0xE0);

            break;

        default:
            return -1;
    }
    return 0;
}

static int
xenon_ata_read_sectors(struct bdev *bdev, void *buf, lba_t start_sector, int sector_size) {
    unsigned int sect;
    struct xenon_ata_device *dev = bdev->ctx;

    start_sector += bdev->offset;

    xenon_ata_setaddress(dev, start_sector, sector_size);

    /* Read sectors.  */
    xenon_ata_regset(dev, XENON_ATA_REG_CMD, XENON_ATA_CMD_READ_SECTORS_EXT);
    xenon_ata_wait_ready(dev);
    for (sect = 0; sect < sector_size; sect++) {
        if (xenon_ata_pio_read(dev, buf, XENON_DISK_SECTOR_SIZE)) {
            printf("ATA read error\n");
            return -1;
        }
        buf += XENON_DISK_SECTOR_SIZE;
    }

    return sect;
}

static int
xenon_ata_write_sectors(struct bdev *bdev, void *buf, lba_t start_sector, int sector_size) {
    unsigned int sect;
    struct xenon_ata_device *dev = bdev->ctx;

    start_sector += bdev->offset;

    xenon_ata_setaddress(dev, start_sector, sector_size);

    /* Read sectors.  */
    xenon_ata_regset(dev, XENON_ATA_REG_CMD, XENON_ATA_CMD_WRITE_SECTORS_EXT);
    xenon_ata_wait_ready(dev);
    for (sect = 0; sect < sector_size; sect++) {
        if (xenon_ata_pio_write(dev, buf, XENON_DISK_SECTOR_SIZE)) {
            printf("ATA write error\n");
            return -1;
        }
        buf += XENON_DISK_SECTOR_SIZE;
    }

    return sect;
}

static int
xenon_atapi_identify(struct xenon_ata_device *dev) {

    char info[XENON_DISK_SECTOR_SIZE];
    xenon_ata_wait_busy(dev);

    xenon_ata_regset(dev, XENON_ATA_REG_DISK, 0xE0);
    xenon_ata_regset(dev, XENON_ATA_REG_CMD,
            XENON_ATA_CMD_IDENTIFY_PACKET_DEVICE);
    xenon_ata_wait_ready(dev);

    xenon_ata_pio_read(dev, info, 256);

    dev->atapi = 1;

    xenon_ata_dumpinfo(dev, info);

    return 0;
}

static int
xenon_atapi_packet(struct xenon_ata_device *dev, char *packet) {
    xenon_ata_regset(dev, XENON_ATA_REG_DISK, 0);
    xenon_ata_regset(dev, XENON_ATA_REG_FEATURES, 0);
    xenon_ata_regset(dev, XENON_ATA_REG_SECTORS, 0);
    xenon_ata_regset(dev, XENON_ATA_REG_LBAHIGH, 0xFF);
    xenon_ata_regset(dev, XENON_ATA_REG_LBAMID, 0xFF);
    xenon_ata_regset(dev, XENON_ATA_REG_CMD, XENON_ATA_CMD_PACKET);
    xenon_ata_wait_ready(dev);

    xenon_ata_pio_write(dev, packet, 12);

    return 0;
}

static int
xenon_atapi_read_sectors(struct bdev *bdev,
        void *buf, lba_t start_sector, int sector_size) {
    struct xenon_ata_device *dev = bdev->ctx;
    struct xenon_atapi_read readcmd;

    readcmd.code = 0xA8;
    readcmd.lba = start_sector;
    readcmd.length = sector_size;

    xenon_atapi_packet(dev, (char *) &readcmd);
    xenon_ata_wait();
    xenon_ata_pio_read(dev, buf, XENON_CDROM_SECTOR_SIZE);

    return sector_size;
}

static int
xenon_ata_identify(struct xenon_ata_device *dev) {
    char info[XENON_DISK_SECTOR_SIZE];
    uint16_t *info16 = (uint16_t *) info;
    xenon_ata_wait_busy(dev);

    xenon_ata_regset(dev, XENON_ATA_REG_DISK, 0xE0);
    xenon_ata_regset(dev, XENON_ATA_REG_CMD, XENON_ATA_CMD_IDENTIFY_DEVICE);
    xenon_ata_wait_ready(dev);

    int val = xenon_ata_pio_read(dev, info, XENON_DISK_SECTOR_SIZE);
    if (val & 4)
        return xenon_atapi_identify(dev);
    else if (val)
        return -1;

    /* Now it is certain that this is not an ATAPI device.  */
    dev->atapi = 0;

    /* CHS is always supported.  */
    dev->addressing_mode = XENON_ATA_CHS;
    /* Check if LBA is supported.  */
    if (bswap16(info16[49]) & (1 << 9)) {
        /* Check if LBA48 is supported.  */
        if (bswap16(info16[83]) & (1 << 10))
            dev->addressing_mode = XENON_ATA_LBA48;
        else
            dev->addressing_mode = XENON_ATA_LBA;
    }

    /* Determine the amount of sectors.  */
    if (dev->addressing_mode != XENON_ATA_LBA48)
        dev->size = __builtin_bswap32(*((uint32_t *) & info16[60]));
    else
        dev->size = __builtin_bswap32(*((uint32_t *) & info16[100]));

    /* Read CHS information.  */
    dev->cylinders = bswap16(info16[1]);
    dev->heads = bswap16(info16[3]);
    dev->sectors_per_track = bswap16(info16[6]);

    xenon_ata_dumpinfo(dev, info);

    return 0;
}

int
xenon_ata_init1(struct xenon_ata_device *dev, uint32_t ioaddress, uint32_t ioaddress2) {

    memset(dev, 0, sizeof (struct xenon_ata_device));
    dev->ioaddress = ioaddress;
    dev->ioaddress2 = ioaddress2;

    /* Try to detect if the port is in use by writing to it,
       waiting for a while and reading it again.  If the value
       was preserved, there is a device connected.  */
    xenon_ata_regset(dev, XENON_ATA_REG_DISK, 0);
    xenon_ata_wait();
    xenon_ata_regset(dev, XENON_ATA_REG_SECTORS, 0x5A);
    xenon_ata_wait();
    if (xenon_ata_regget(dev, XENON_ATA_REG_SECTORS) != 0x5A) {
        printf("no ata device connected.\n");
        return -1;
    }

    /* Detect if the device is present by issuing a reset.  */
    xenon_ata_regset2(dev, XENON_ATA_REG2_CONTROL, 6);
    xenon_ata_wait();
    xenon_ata_regset2(dev, XENON_ATA_REG2_CONTROL, 2);
    xenon_ata_wait();
    xenon_ata_regset(dev, XENON_ATA_REG_DISK, 0);
    xenon_ata_wait();

#if 1
    /* Enable for ATAPI .  */
    if (xenon_ata_regget(dev, XENON_ATA_REG_CYLLSB) != 0x14
            || xenon_ata_regget(dev, XENON_ATA_REG_CYLMSB) != 0xeb)
#endif
        if (xenon_ata_regget(dev, XENON_ATA_REG_STATUS) == 0
                || (xenon_ata_regget(dev, XENON_ATA_REG_CYLLSB) != 0
                && xenon_ata_regget(dev, XENON_ATA_REG_CYLMSB) != 0
                && xenon_ata_regget(dev, XENON_ATA_REG_CYLLSB) != 0x3c
                && xenon_ata_regget(dev, XENON_ATA_REG_CYLLSB) != 0xc3)) {
            printf("no ata device presented.\n");
            return -1;
        }

    printf("SATA device at %08lx\n", dev->ioaddress);
    xenon_ata_identify(dev);

    return 0;
}

struct bdev_ops xenon_ata_ops ={
    .read = xenon_ata_read_sectors
};

int
xenon_ata_init() {
    //preinit (start from scratch)
    *(volatile uint32_t*)0xd0110060 = __builtin_bswap32(0x112400);
    *(volatile uint32_t*)0xd0110080 = __builtin_bswap32(0x407231BE);
    *(volatile uint32_t*)0xea001318 &= __builtin_bswap32(0xFFFFFFF0);
	mdelay(100);
    
    struct xenon_ata_device *dev = &ata;
    memset(dev, 0, sizeof (struct xenon_ata_device));
    int err = xenon_ata_init1(dev, 0xea001300, 0xea001320);
    if (!err) dev->bdev = register_bdev(dev, &xenon_ata_ops, "sda");
    return err;
}

struct bdev_ops xenon_atapi_ops ={
    .read = xenon_atapi_read_sectors
};

int
xenon_atapi_init() {
    //preinit (start from scratch)
    *(volatile uint32_t*)0xd0108060 = __builtin_bswap32(0x112400);
    *(volatile uint32_t*)0xd0108080 = __builtin_bswap32(0x407231BE);
    *(volatile uint32_t*)0xea001218 &= __builtin_bswap32(0xFFFFFFF0);
	mdelay(100);
	
    struct xenon_ata_device *dev = &atapi;
	memset(dev, 0, sizeof (struct xenon_ata_device));
    int err = xenon_ata_init1(dev, 0xea001200, 0xea001220);
    if (!err) dev->bdev = register_bdev(dev, &xenon_atapi_ops, "dvd");
    return err;
}

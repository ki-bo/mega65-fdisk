/*
  Extremely simplified FDISK + FORMAT utility for the MEGA65.
  This program is designed to be compilable both for the MEGA65
  using CC65, and also for UNIX-like operating systems for testing.
  All hardware dependent features will be in fdisk_hal_mega65.c and
  fdisk_hal_unix.c, respectively. I.e., this file contains only the
  hardware independent logic.

  This program gets the size of the SD card, and then calculates an
  appropriate MBR, DOS Boot Sector, FS Information Sector, FATs and
  root directory, and puts them in place.

  Also creates the MEGA65 system partitions for
  installed services, and for task switching.

  XXX - Should initialise the configuration sector in the system partition,
  so that on first use, you don't get the "CONFIGURATION INVALID" message
  from the Hypervisor.

*/

#include <stdio.h>
#include <string.h>
#ifndef __CC65__
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif

#include "fdisk_hal.h"
#include "fdisk_memory.h"
#include "fdisk_screen.h"
#include "fdisk_fat32.h"
#include "ascii.h"

unsigned char slot_magic[16] = { 0x4d, 0x45, 0x47, 0x41, 0x36, 0x35, 0x42, 0x49, 0x54, 0x53, 0x54, 0x52, 0x45, 0x41, 0x4d,
  0x30 };

unsigned char have_rom = 0, have_sdfiles = 0;
uint8_t hardware_model_id = 0xff;
unsigned long slot_size = 8L * 1048576L;

#define MAX_SLOT 8
typedef struct {
  char version[32];
  unsigned char file_count;
  unsigned long file_offset;
} mega65slotT;
mega65slotT mega65slot[MAX_SLOT];

// When set, it enters batch mode
unsigned char dont_confirm = 0;

uint8_t sector_buffer[512];

void clear_sector_buffer(void)
{
#ifndef __CC65__DONTUSE
  int i;
  for (i = 0; i < 512; i++)
    sector_buffer[i] = 0;
#else
  lfill((uint32_t)sector_buffer, 0, 512);
#endif
}

/* Build a master boot record that has the single partition we need in
   the correct place, and with the size of the partition set correctly.
*/
void build_mbr(const uint32_t sys_partition_start, const uint32_t sys_partition_sectors, const uint32_t fat_partition_start,
    const uint32_t fat_partition_sectors)
{
  clear_sector_buffer();

  // Set disk signature (fixed value)
  sector_buffer[0x1b8] = 0x83;
  sector_buffer[0x1b9] = 0x7d;
  sector_buffer[0x1ba] = 0xcb;
  sector_buffer[0x1bb] = 0xa6;

  // We have to put the FAT partition first for people running bitstreams from
  // the microSD card, as otherwise the N4/N4DDR boards don't find the FAT partition

  // MEGA65 System Partition entry
  //  sector_buffer[0x1ce]=0x00;  // Not bootable by DOS
  //  sector_buffer[0x1cf]=0x00;  // 3 bytes CHS starting point
  //  sector_buffer[0x1d0]=0x00;
  //  sector_buffer[0x1d1]=0x00;
  sector_buffer[0x1d2] = 0x41; // Partition type (MEGA65 System Partition)
  //  sector_buffer[0x1d3]=0x00;  // 3 bytes CHS end point - SHOULD CHANGE WITH DISK SIZE
  //  sector_buffer[0x1d4]=0x00;
  //  sector_buffer[0x1d5]=0x00;
  // LBA starting sector of partition (usually @ 0x0800 = sector 2,048 = 1MB)
  sector_buffer[0x1d6] = (sys_partition_start >> 0) & 0xff;
  sector_buffer[0x1d7] = (sys_partition_start >> 8) & 0xff;
  sector_buffer[0x1d8] = (sys_partition_start >> 16) & 0xff;
  sector_buffer[0x1d9] = (sys_partition_start >> 24) & 0xff;
  // LBA size of partition in sectors
  sector_buffer[0x1da] = (sys_partition_sectors >> 0) & 0xff;
  sector_buffer[0x1db] = (sys_partition_sectors >> 8) & 0xff;
  sector_buffer[0x1dc] = (sys_partition_sectors >> 16) & 0xff;
  sector_buffer[0x1dd] = (sys_partition_sectors >> 24) & 0xff;

  // FAT32 Partition entry
  //  sector_buffer[0x1be]=0x00;  // Not bootable by DOS
  //  sector_buffer[0x1bf]=0x00;  // 3 bytes CHS starting point
  //  sector_buffer[0x1c0]=0x00;
  //  sector_buffer[0x1c1]=0x00;
  sector_buffer[0x1c2] = 0x0c; // Partition type (VFAT32)
  //  sector_buffer[0x1c3]=0x00;  // 3 bytes CHS end point - SHOULD CHANGE WITH DISK SIZE
  //  sector_buffer[0x1c4]=0x00;
  //  sector_buffer[0x1c5]=0x00;
  // LBA starting sector of FAT32 partition
  sector_buffer[0x1c6] = (fat_partition_start >> 0) & 0xff;
  sector_buffer[0x1c7] = (fat_partition_start >> 8) & 0xff;
  sector_buffer[0x1c8] = (fat_partition_start >> 16) & 0xff;
  sector_buffer[0x1c9] = (fat_partition_start >> 24) & 0xff;
  // LBA size of partition in sectors
  sector_buffer[0x1ca] = (fat_partition_sectors >> 0) & 0xff;
  sector_buffer[0x1cb] = (fat_partition_sectors >> 8) & 0xff;
  sector_buffer[0x1cc] = (fat_partition_sectors >> 16) & 0xff;
  sector_buffer[0x1cd] = (fat_partition_sectors >> 24) & 0xff;

  // MBR signature
  sector_buffer[0x1fe] = 0x55;
  sector_buffer[0x1ff] = 0xaa;
}

uint8_t boot_bytes[258] = {
  // Jump to boot code, required by most version of DOS
  0xeb, 0x58, 0x90,

  // OEM String: MEGA65r1
  0x4d, 0x45, 0x47, 0x41, 0x36, 0x35, 0x72, 0x31,

  // BIOS Parameter block.  We patch certain
  // values in here.
  0x00, 0x02,                   // Sector size = 512 bytes
  0x08,                         // Sectors per cluster
  /* 0x0e */ 0x38, 0x02,        // Number of reserved sectors (0x238 = 568)
  /* 0x10 */ 0x02,              // Number of FATs
  0x00, 0x00,                   // Max directory entries for FAT12/16 (0 for FAT32)
  /* offset 0x13 */ 0x00, 0x00, // Total logical sectors (0 for FAT32)
  0xf8,                         // Disk type (0xF8 = hard disk)
  0x00, 0x00,                   // Sectors per FAT for FAT12/16 (0 for FAT32)
  /* offset 0x18 */ 0x00, 0x00, // Sectors per track (0 for LBA only)
  0x00, 0x00,                   // Number of heads for CHS drives, zero for LBA
  0x00, 0x00, 0x00, 0x00,       // 32-bit Number of hidden sectors before partition. Should be 0 if logical sectors == 0

  /* 0x20 */ 0x00, 0xe8, 0x0f, 0x00,              // 32-bit total logical sectors
  /* 0x24 */ 0xf8, 0x03, 0x00, 0x00,              // Sectors per FAT
  /* 0x28 */ 0x00, 0x00,                          // Drive description
  /* 0x2a */ 0x00, 0x00,                          // Version 0.0
  /* 0x2c */ 0x02, 0x00, 0x00, 0x00,              // Number of first cluster
  /* 0x30 */ 0x01, 0x00,                          // Logical sector of FS Information sector
  /* 0x32 */ 0x06, 0x00,                          // Sector number of backup-copy of boot sector
  /* 0x34 */ 0x00, 0x00, 0x00, 0x00,              // Filler bytes
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Filler bytes
  /* 0x40 */ 0x80,                                // Physical drive number
  /* 0x41 */ 0x00,                                // FAT12/16 use only
  /* 0x42 */ 0x29,                                // 0x29 == Extended Boot Signature
  /* 0x43 */ 0x6d, 0x66, 0x62, 0x61,              // Volume ID "mfba"
  /* 0x47 */ 0x4d, 0x2e, 0x45, 0x2e, 0x47,        // 11 byte volume label
  0x2e, 0x41, 0x2e, 0x20, 0x36, 0x35,
  /* 0x52 */ 0x46, 0x41, 0x54, 0x33, 0x32, 0x20, 0x20, 0x20, // "FAT32   "
  // Boot loader code starts here
  0x0e, 0x1f, 0xbe, 0x77, 0x7c, 0xac, 0x22, 0xc0, 0x74, 0x0b, 0x56, 0xb4, 0x0e, 0xbb, 0x07, 0x00, 0xcd, 0x10, 0x5e, 0xeb,
  0xf0, 0x32, 0xe4, 0xcd, 0x16, 0xcd, 0x19, 0xeb, 0xfe,
  // From here on is the non-bootable error message
  // 0x82 - 0x69 =
  0x4d, 0x45, 0x47, 0x41, 0x36, 0x35, 0x20,

  // 9-character name of operating system
  'H', 'Y', 'P', 'P', 'O', 'B', 'O', 'O', 'T',

  0x20, 0x56, 0x30, 0x30, 0x2e, 0x31, 0x31, 0x0d, 0x0a, 0x0d, 0x3f, 0x4e, 0x4f, 0x20, 0x34, 0x35, 0x47, 0x53, 0x30, 0x32,
  0x2c, 0x20, 0x34, 0x35, 0x31, 0x30, 0x2c, 0x20, 0x36, 0x35, 0x5b, 0x63, 0x65, 0x5d, 0x30, 0x32, 0x2c, 0x20, 0x36, 0x35,
  0x31, 0x30, 0x20, 0x4f, 0x52, 0x20, 0x38, 0x35, 0x31, 0x30, 0x20, 0x50, 0x52, 0x4f, 0x43, 0x45, 0x53, 0x53, 0x4f, 0x52,
  0x20, 0x20, 0x45, 0x52, 0x52, 0x4f, 0x52, 0x0d, 0x0a, 0x49, 0x4e, 0x53, 0x45, 0x52, 0x54, 0x20, 0x44, 0x49, 0x53, 0x4b,
  0x20, 0x49, 0x4e, 0x20, 0x52, 0x45, 0x41, 0x4c, 0x20, 0x43, 0x4f, 0x4d, 0x50, 0x55, 0x54, 0x45, 0x52, 0x20, 0x41, 0x4e,
  0x44, 0x20, 0x54, 0x52, 0x59, 0x20, 0x41, 0x47, 0x41, 0x49, 0x4e, 0x2e, 0x0a, 0x0a, 0x52, 0x45, 0x41, 0x44, 0x59, 0x2e,
  0x0d, 0x0a

};

void build_dosbootsector(uint32_t data_sectors, uint32_t fs_sectors_per_fat)
{
  uint16_t i;

  clear_sector_buffer();

  // Start with template, and then modify relevant fields */
  lcopy((unsigned long)boot_bytes, (unsigned long)sector_buffer, sizeof(boot_bytes));

  // 0x20-0x23 = 32-bit number of data sectors in file system
  for (i = 0; i < 4; i++)
    sector_buffer[0x20 + i] = ((data_sectors) >> (i * 8)) & 0xff;

  // 0x24-0x27 = 32-bit number of sectors per fat
  for (i = 0; i < 4; i++)
    sector_buffer[0x24 + i] = ((fs_sectors_per_fat) >> (i * 8)) & 0xff;

  // 0x43-0x46 = 32-bit volume ID (random bytes)
  // 0x47-0x51 = 11 byte volume string

  // Boot sector signature
  sector_buffer[510] = 0x55;
  sector_buffer[511] = 0xaa;
}

void build_fs_information_sector(const uint32_t fs_clusters)
{
  uint8_t i;

  clear_sector_buffer();

  sector_buffer[0] = 0x52;
  sector_buffer[1] = 0x52;
  sector_buffer[2] = 0x61;
  sector_buffer[3] = 0x41;

  sector_buffer[0x1e4] = 0x72;
  sector_buffer[0x1e5] = 0x72;
  sector_buffer[0x1e6] = 0x41;
  sector_buffer[0x1e7] = 0x61;

  // Last free cluster = (cluster count - 1)
#ifndef __CC65__
  fprintf(stderr, "Writing fs_clusters (0x%x) as ", fs_clusters);
#endif
  for (i = 0; i < 4; i++) {
    // Number of free clusters
    sector_buffer[0x1e8 + i] = ((fs_clusters - 3) >> (i * 8)) & 0xff;
#ifndef __CC65__
    fprintf(stderr, "%02x ", sector_buffer[0x1e8 + i]);
#endif
  }
#ifndef __CC65__
  fprintf(stderr, "\n");
#endif

  // First free cluster = 2
  sector_buffer[0x1ec] = 0x02 + 1; // OSX newfs/fsck puts 3 here instead?

  // Boot sector signature
  sector_buffer[510] = 0x55;
  sector_buffer[511] = 0xaa;
}

uint8_t fat_bytes[12] = { 0xf8, 0xff, 0xff, 0x0f, 0xff, 0xff, 0xff, 0x0f, 0xf8, 0xff, 0xff, 0x0f };

void build_empty_fat()
{
  int i;
  clear_sector_buffer();
  for (i = 0; i < 12; i++)
    sector_buffer[i] = fat_bytes[i];
}

uint8_t dir_bytes[15] = { 8, 0, 0, 0x53, 0xae, 0x93, 0x4a, 0x93, 0x4a, 0, 0, 0x53, 0xae, 0x93, 0x4a };

void build_root_dir(const uint8_t volume_name[11])
{
  int i;
  clear_sector_buffer();
  for (i = 0; i < 11; i++)
    sector_buffer[i] = volume_name[i];
  for (i = 0; i < 15; i++)
    sector_buffer[11 + i] = dir_bytes[i];
}

uint32_t sdcard_sectors;

uint32_t sys_partition_start, sys_partition_sectors;
uint32_t fat_partition_start, fat_partition_sectors;

uint32_t sys_partition_freeze_dir;
uint16_t freeze_dir_sectors;
uint32_t sys_partition_service_dir;
uint16_t service_dir_sectors;

// Calculate clusters for file system, and FAT size
uint32_t fs_clusters = 0;
uint32_t reserved_sectors = 568; // not sure why we use this value
uint32_t rootdir_sector = 0;
uint32_t fat_sectors = 0;
uint32_t fat1_sector = 0;
uint32_t fat2_sector = 0;
uint32_t fs_data_sectors = 0;
uint8_t sectors_per_cluster = 8; // 4KB clusters
uint8_t volume_name[11] = "M.E.G.A.65!";

// Work out maximum number of clusters we can accommodate
uint32_t sectors_required;
uint32_t fat_available_sectors;

void sector_buffer_write_uint16(const uint16_t offset, const uint32_t value)
{
  sector_buffer[offset + 0] = (value >> 0) & 0xff;
  sector_buffer[offset + 1] = (value >> 8) & 0xff;
}

void sector_buffer_write_uint32(const uint16_t offset, const uint32_t value)
{
  sector_buffer[offset + 0] = (value >> 0) & 0xff;
  sector_buffer[offset + 1] = (value >> 8) & 0xff;
  sector_buffer[offset + 2] = (value >> 16) & 0xff;
  sector_buffer[offset + 3] = (value >> 24) & 0xff;
}

uint8_t sys_part_magic[] = { 'M', 'E', 'G', 'A', '6', '5', 'S', 'Y', 'S', '0', '0' };

void build_mega65_sys_sector(const uint32_t sys_partition_sectors)
{
  /*
    System partition has frozen program and system service areas, including
    directories for each.

    We work out how many of each we can have (equal number of each), based
    on the size we require them to be.

    The size of each is subject to change, so is left flexible.  One thing
    that is not resolved, is whether to allow including a D81 image in either.

    For now, we will allow 384KB RAM (including 128K "ROM" area) + 32KB colour RAM
    + 32KB IO regs (including 4KB thumbnail).
    That all up means 448KB per frozen program slot.  We'll just make them 512KB for
    good measure.

    For some services at least, we intend to allow for a substantial part of
    memory to be preserved, so we need to have a mechanism that indicates what
    parts of which memory areas require preservation, and whether IO should be
    preserved or not.

    Simple apporach is KB start and end ranges for the four regions = 8 bytes.
    This can go in the directory entries, which have 64 bytes for information.
    We will also likely put the hardware/access permission flags in there
    somewhere, too.

    Anyway, so we need to divide the available space by (512KB + 128 bytes).
    Two types of area means simplest approach with equal slots for both means
    dividing space by (512KB + 128 bytes)*2= ~1025KB.
  */
  uint16_t i;
  uint32_t slot_size = 512 * 1024 / 512; // slot_size units is sectors
  // Take 1MB from partition size, for reserved space when
  // calculating what can fit.
  uint32_t reserved_sectors = 1024 * 1024 / 512;
  uint32_t slot_count = (sys_partition_sectors - reserved_sectors) / (slot_size * 2 + 1);
  uint16_t dir_size;

  // Limit number of freeze slots to 16 bit counters
  if (slot_count >= 0xffff)
    slot_count = 0xffff;

  dir_size = 1 + (slot_count / 4);

  freeze_dir_sectors = dir_size;
  service_dir_sectors = dir_size;

  // Freeze directory begins at 1MB
  sys_partition_freeze_dir = reserved_sectors;
  // System service directory begins after that
  sys_partition_service_dir = sys_partition_freeze_dir + slot_size * slot_count;

#ifdef __CC65__
  write_line("      Freeze and OS Service slots.", 0);
  screen_decimal(screen_line_address - 79, slot_count);
#else
  fprintf(stdout, " %5d Freeze and OS Service slots\n", slot_count);
#endif

  // Clear sector
  clear_sector_buffer();

  // Write magic bytes
  for (i = 0; i < 11; i++)
    sector_buffer[i] = sys_part_magic[i];

  // $010-$013 = Start of freeze program area
  sector_buffer_write_uint32(0x10, 0);
  // $014-$017 = Size of freeze program area
  sector_buffer_write_uint32(0x14, slot_size * slot_count + dir_size);
  // $018-$01b = Size of each freeze program slot
  sector_buffer_write_uint32(0x18, slot_size);
  // $01c-$01d = Number of freeze slots
  sector_buffer_write_uint16(0x1c, slot_count);
  // $01e-$01f = Number of sectors in freeze slot directory
  sector_buffer_write_uint16(0x1e, dir_size);

  // $020-$023 = Start of freeze program area
  sector_buffer_write_uint32(0x20, slot_size * slot_count + dir_size);
  // $024-$027 = Size of service program area
  sector_buffer_write_uint32(0x24, slot_size * slot_count + dir_size);
  // $028-$02b = Size of each service slot
  sector_buffer_write_uint32(0x28, slot_size);
  // $02c-$02d = Number of service slots
  sector_buffer_write_uint16(0x2c, slot_count);
  // $02e-$02f = Number of sectors in service slot directory
  sector_buffer_write_uint16(0x2e, dir_size);

  // Now make sector numbers relative to start of disk for later use
  sys_partition_freeze_dir += sys_partition_start;
  sys_partition_service_dir += sys_partition_start;

  return;
}

void build_mega65_sys_config_sector(void)
{
  /*
    Create default valid system configuration sector
  */

  // Clear sector
  clear_sector_buffer();

  // Structure version bytes
  sector_buffer[0x000] = 0x01;
  sector_buffer[0x001] = 0x01;
  // PAL=$00, NTSC=$80
  sector_buffer[0x002] = 0x80;
  // Enable audio amp, mono output
  // XXX - Is mono output now controlled only via audio mixer?
  sector_buffer[0x003] = 0x41;
  // Use SD card for floppies
  sector_buffer[0x004] = 0x00;
  // Enable use of Amiga mouses automatically
  sector_buffer[0x005] = 0x01;
  // Pick an ethernet address 0x006 - 0x00b
  // XXX - Should do a better job of this!
  sector_buffer[0x006] = 0x41;
  sector_buffer[0x007] = 0x41;
  sector_buffer[0x008] = 0x41;
  sector_buffer[0x009] = 0x41;
  sector_buffer[0x00A] = 0x41;
  sector_buffer[0x00B] = 0x41;
  // Set name of default disk image
#ifdef __CC65__
  lcopy((unsigned long)"mega65.d81", (unsigned long)&sector_buffer[0x10], 10);
#else
  bcopy("mega65.d81", &sector_buffer[0x10], 10);
#endif

  // DMAgic to new version (F011B) by default
  sector_buffer[0x020] = 0x01;

  return;
}

void show_partition_entry(const char i)
{
  char j;
#ifdef __CC65__
  char report[80] = "$$* : Start=%%%/%%/%%%% or $$$$$$$$ / End=%%%/%%/%%%% or $$$$$$$$";
#endif

  int offset = 0x1be + (i << 4);

  char active = sector_buffer[offset + 0];
  char shead = sector_buffer[offset + 1];
  char ssector = sector_buffer[offset + 2] & 0x1f;
  int scylinder = ((sector_buffer[offset + 2] << 2) & 0x300) + sector_buffer[offset + 3];
  char id = sector_buffer[offset + 4];
  char ehead = sector_buffer[offset + 5];
  char esector = sector_buffer[offset + 6] & 0x1f;
  int ecylinder = ((sector_buffer[offset + 6] << 2) & 0x300) + sector_buffer[offset + 7];
  uint32_t lba_start, lba_end;

  for (j = 0; j < 4; j++)
    ((char *)&lba_start)[j] = sector_buffer[offset + 8 + j];
  for (j = 0; j < 4; j++)
    ((char *)&lba_end)[j] = sector_buffer[offset + 12 + j];

#ifdef __CC65__
  format_hex((int)report + 0, id, 2);
  if (!(active & 0x80))
    report[2] = ' '; // not active

  format_decimal((int)report + 12, shead, 3);
  format_decimal((int)report + 16, ssector, 2);
  format_decimal((int)report + 19, scylinder, 4);
  format_hex((int)report + 27, lba_start, 8);

  format_decimal((int)report + 42, ehead, 3);
  format_decimal((int)report + 46, esector, 2);
  format_decimal((int)report + 49, ecylinder, 4);
  format_hex((int)report + 57, lba_end, 8);

  write_line(report, 2);
#else
  printf("%02X%c : Start=%3d/%2d/%4d or %08X / End=%3d/%2d/%4d or %08X\n", id, active & 80 ? '*' : ' ', shead, ssector,
      scylinder, lba_start, ehead, esector, ecylinder, lba_end);
#endif
}

void show_mbr(void)
{
  char i;

  sdcard_readsector(0);

  write_line("", 0);

  if ((sector_buffer[0x1fe] != 0x55) || (sector_buffer[0x1ff] != 0xAA))
    write_line("Current partition table is invalid.", 2);
  else {
    write_line("Current partition table:", 2);
    for (i = 0; i < 4; i++) {
      show_partition_entry(i);
    }
  }
}

char buffer[80];
unsigned char file_count;
unsigned long file_offset, next_offset, file_len, first_sector;
char eightthree[8 + 3 + 1];

void scan_slots(void)
{
  unsigned char i, j;

  hardware_model_id = PEEK(0xD629);
  if (hardware_model_id == 3)
    slot_size = 1048576L * 8; // 8MB slots for mega65r3 platform
  else
    slot_size = 1048576L * 4;

  for (i = 0; i < MAX_SLOT; i++) {
    mega65slot[i].version[0] = 0;
    mega65slot[i].file_count = 0;
    mega65slot[i].file_offset = 0;
    flash_readsector(i * slot_size);
    lcopy(0xffd6e00, (unsigned long)sector_buffer, 512);
    for (j = 0; j < 16; j++)
      if (slot_magic[j] != sector_buffer[j])
        break;
    if (j < 16) continue;
    for (j = 0; j < 6; j++)
      if (slot_magic[j] != sector_buffer[16 + j]) // check MEGA65 slot
        break;
    if (j < 6) continue;
    for (j = 0; j < 32; j++)
      mega65slot[i].version[j] = sector_buffer[48 + j];
    mega65slot[i].version[j] = 0;
    for (j--; mega65slot[i].version[j] == ' ' && j > 0 ; j--);
    mega65slot[i].file_count = sector_buffer[0x72];
    mega65slot[i].file_offset = i * slot_size + *(unsigned long *)&sector_buffer[0x73];
  }
}

char populate_file_system(unsigned char slot)
{
  unsigned char i, j, k;
  char *pos;

  if (!mega65slot[slot].version[0] || !mega65slot[slot].file_count)
    return 1;

  strcpy(buffer, "Using files embedded in slot @");
  pos = strchr(buffer, '@');
  *pos = 0x30 + slot;
  write_line(buffer, 1);
  file_offset = mega65slot[slot].file_offset;
  file_count = mega65slot[slot].file_count;
  write_line("   Files in Core, starting at $        .", 1);
  format_decimal(screen_line_address - 79, file_count, 2);
  screen_hex(screen_line_address - 48, file_offset);

  for (i = 0; i < file_count; i++) {
    flash_readsector(file_offset);
    next_offset = slot * slot_size + *(unsigned long *)&sector_buffer[0];
    file_len = *(unsigned long *)&sector_buffer[4];
    write_line("Pre-populating file ", 1);
    for (j = 0; sector_buffer[8 + j]; j++)
      lpoke(screen_line_address - 59 + j, sector_buffer[8 + j]);
#ifdef __CC65__
    recolour_last_line(8);
#endif
    // Prepare "EIGHT  THR" formatted DOS filename for fat32_create_contiguous_file
    for (j = 0; j < 11; j++)
      eightthree[j] = ' ';
    eightthree[11] = 0;
    k = 0;
    for (j = 0; sector_buffer[8 + j]; j++) {
      if (sector_buffer[8 + j] == '.')
        k = 8;
      else
        eightthree[k++] = sector_buffer[8 + j];
      if (k >= 11)
        break;
    }

    if (!strcmp(&sector_buffer[8], "MEGA65.ROM"))
      have_rom = 1;

    // Skip header
    file_offset += 4 + 4 + 32;

    first_sector = fat32_create_contiguous_file(eightthree, file_len, fat_partition_start + rootdir_sector,
        fat_partition_start + fat1_sector, fat_partition_start + fat2_sector);

    if (first_sector) {
      // Write out file sectors
      unsigned long addr;
      for (addr = 0; addr <= file_len; addr += 512) {
        POKE(0xD020, PEEK(0xD020) + 1);
        flash_readsector(file_offset + addr);
        sdcard_writesector(first_sector++);
      }
#ifdef __CC65__
      recolour_last_line(1);
#endif
    }
    else {
      write_line("!! Error writing file", 1);
#ifdef __CC65__
      recolour_last_line(2);
#endif
    }

    file_offset = next_offset;
  }

  return 0;
}

#ifdef __CC65__
void main(void)
#else
int main(int argc, char **argv)
#endif
{
  unsigned char key, cardSlot, slotAvail;

rescanSlots:
#ifdef __CC65__
  mega65_fast();
  setup_screen();

next_card:
#endif

  slotAvail = 0;
  sdcard_select(0);
  sdcard_open();

  // Memory map the SD card sector buffer on MEGA65
  sdcard_map_sector_buffer();

  write_line("Detecting SD card(s) (can take a while)", 1);
  write_line("", 0);

  write_line("SD Card 0 (Internal SD slot):", 1);
#ifdef __CC65__
  recolour_last_line(0x2c);
#endif

  sdcard_select(0);
  if (sdcard_reset()) {
    write_line("No card detected on bus 0", 2);
#ifdef __CC65__
    recolour_last_line(8);
#endif
  }
  else {
    sdcard_sectors = sdcard_getsize();

    // Report speed of SD card
    sdcard_readspeed_test();

    // Show summary of current MBR
    show_mbr();

    slotAvail |= 1;
  }

  write_line("", 0);
  write_line("SD Card 1 (External microSD slot):", 1);
#ifdef __CC65__
  recolour_last_line(0x2c);
#endif

  sdcard_select(1);
  if (sdcard_reset()) {
    write_line("No card detected on bus 1", 2);
#ifdef __CC65__
    recolour_last_line(8);
#endif
  }
  else {
    sdcard_sectors = sdcard_getsize();

    // Report speed of SD card
    sdcard_readspeed_test();

    // Show summary of current MBR
    show_mbr();

    slotAvail |= 2;
  }
  write_line("", 0);

  // Make user select SD card
  POKE(0xd020, 6);
  strcpy(buffer, "Please select SD card to modify or r to rescan (");
  if (slotAvail&1)
    strcat(buffer, "0/");
  if (slotAvail&2)
    strcat(buffer, "1/");
  strcat(buffer, "r): ");
  write_line(buffer, 1);
#ifdef __CC65__
  recolour_last_line(7);

  do {
    key = mega65_getkey();
  } while (key != 'r' && (!(slotAvail&1) || key != '0') && (!(slotAvail&2) || key != '1'));
  if (key == 'r')
    goto rescanSlots;

  cardSlot = key & 1;
  sdcard_select(cardSlot);
#endif

  // Then make sure we have correct information for the selected card
  sdcard_open();
  sdcard_sectors = sdcard_getsize();
  sdcard_readspeed_test();
  show_mbr();

  // Calculate sectors for the system and FAT32 partitions.
  // This is the size of the card, minus 2,048 (=0x0800) sectors.
  // The system partition should be sized to be not more than 50% of
  // the SD card, and probably doesn't need to be bigger than 2GB, which would
  // allow 1GB for 1,024 1MB freeze images and 1,024 1MB service images.
  // (note that freeze images might end up being a funny size to allow for all
  // mem plus a D81 image to be saved. This is all to be determined.)
  // Simple solution for now: Use 1/2 disk for system partition, or 2GiB, whichever
  // is smaller.
  sys_partition_sectors = (sdcard_sectors - 0x0800) >> 1;
  if (sys_partition_sectors > (2 * 1024 * (1024 * 1024 / 512)))
    sys_partition_sectors = (2 * 1024 * (1024 * 1024 / 512));
  sys_partition_sectors &= 0xfffff800; // round down to nearest 1MB boundary
  fat_partition_sectors = sdcard_sectors - 0x800 - sys_partition_sectors;

  fat_available_sectors = fat_partition_sectors - reserved_sectors;

  fs_clusters = fat_available_sectors / (sectors_per_cluster);
  fat_sectors = fs_clusters / (512 / 4);
  if (fs_clusters % (512 / 4))
    fat_sectors++;
  sectors_required = 2 * fat_sectors + ((fs_clusters - 2) * sectors_per_cluster);
  while (sectors_required > fat_available_sectors) {
    uint32_t excess_sectors = sectors_required - fat_available_sectors;
    uint32_t delta = (excess_sectors / (1 + sectors_per_cluster));
    if (delta < 1)
      delta = 1;
#ifndef __CC65__
    fprintf(
        stderr, "%d clusters would take %d too many sectors.\r\n", fs_clusters, sectors_required - fat_available_sectors);
#endif
    fs_clusters -= delta;
    fat_sectors = fs_clusters / (512 / 4);
    if (fs_clusters % (512 / 4))
      fat_sectors++;
    sectors_required = 2 * fat_sectors + ((fs_clusters - 2) * sectors_per_cluster);
  }
#ifndef __CC65__
  fprintf(stderr, "VFAT32 PARTITION HAS $%x SECTORS ($%x AVAILABLE)\r\n", fat_partition_sectors, fat_available_sectors);
#else
  // Tell use how many sectors available for partition
  write_line("", 0);
  write_line("$         Sectors available for MEGA65 System partition.", 1);
  screen_hex(screen_line_address - 78, sys_partition_sectors);
  build_mega65_sys_sector(sys_partition_sectors);

  write_line("$         Sectors available for VFAT32 partition.", 1);
  screen_hex(screen_line_address - 78, fat_partition_sectors);
#endif

  fat_partition_start = 0x00000800;
  sys_partition_start = fat_partition_start + fat_partition_sectors;

  fat1_sector = reserved_sectors;
  fat2_sector = fat1_sector + fat_sectors;
  rootdir_sector = fat2_sector + fat_sectors;
  fs_data_sectors = fs_clusters * sectors_per_cluster;

#ifndef __CC65__
  printf("Type DELETE EVERYTHING to delete everything on %s SD.\n", cardSlot&1 ? "external" : "internal");
  char line[1024];
  fgets(line, 1024, stdin);
  while (line[0] && line[strlen(line) - 1] == '\n')
    line[strlen(line) - 1] = 0;
  while (line[0] && line[strlen(line) - 1] == '\r')
    line[strlen(line) - 1] = 0;
  if (strcmp(line, "DELETE EVERYTHING")) {
    fprintf(stderr, "String did not match -- aborting.\n");
    exit(-1);
  }

  fprintf(stderr, "Creating File System with %u (0x%x) CLUSTERS, %d SECTORS PER FAT, %d RESERVED SECTORS.\r\n", fs_clusters,
      fs_clusters, fat_sectors, reserved_sectors);
#else
  write_line("", 0);
  strcpy(buffer, "Format ");
  strcat(buffer, cardSlot&1 ? "external" : "internal");
  strcat(buffer, " Card with new partition table and FAT32 file system?");
  write_line(buffer, 1);
  recolour_last_line(7);
  {
    char col = 6;
    int megs = (fat_partition_sectors + 1) / 2048;
    screen_decimal(screen_line_address + 2, megs);
    if (megs < 10000)
      col = 5;
    if (megs < 1000)
      col = 4;
    if (megs < 100)
      col = 3;
    if (megs < 10)
      col = 2;
    write_line("MiB VFAT32 Data Partition @ $$$$$$$$:", 2 + col);
    screen_hex(screen_line_address - 80 + 28 + 2 + col, fat_partition_start);
  }
  write_line("  $         Clusters,       Sectors/FAT,       Reserved Sectors.", 0);
  screen_hex(screen_line_address - 80 + 3, fs_clusters);
  screen_decimal(screen_line_address - 80 + 22, fat_sectors);
  screen_decimal(screen_line_address - 80 + 41, reserved_sectors);

  {
    char col = 6;
    int megs = (sys_partition_sectors + 1) / 2048;
    screen_decimal(2 + screen_line_address, megs);
    if (megs < 10000)
      col = 5;
    if (megs < 1000)
      col = 4;
    if (megs < 100)
      col = 3;
    if (megs < 10)
      col = 2;
    write_line("MiB MEGA65 System Partition @ $$$$$$$$:", 2 + col);
    screen_hex(screen_line_address - 80 + 30 + 2 + col, sys_partition_start);
  }

  //  multisector_write_test();

  while (1) {
    unsigned char len;
    if (!dont_confirm) {
      write_line("", 0);
      strcpy(buffer, "Type DELETE EVERYTHING to continue formatting the ");
      strcat(buffer, cardSlot&1 ? "external" : "internal");
      strcat(buffer, " SD");
      write_line(buffer, 1);
      recolour_last_line(2);
      write_line("or type FIX MBR to re-write MBR:", 1);
      recolour_last_line(2);
      screen_line_address++;
      len = read_line(buffer, 79);
      screen_line_address--;
      if (len) {
        write_line(buffer, 1);
        recolour_last_line(7);
      }
    }

    if (!strcmp("FIX MBR", buffer)) {
      build_mbr(sys_partition_start, sys_partition_sectors, fat_partition_start, fat_partition_sectors);
      sdcard_writesector(0);
      show_mbr();
      write_line("MBR Re-written", 0);
      while (1)
        continue;
    }
    else if (!strcmp("FOLTERLOS MODUS BITTE", buffer)) {
      // Delete cards REPEATEDLY
      dont_confirm = 1;
      break;
    }
    else if (strcmp("DELETE EVERYTHING", buffer) && strcmp("BATCH MODE", buffer)) {
      write_line("Entered text does not match. Try again.", 1);
      recolour_last_line(8);
    }
    else
      // String matches -- so proceed
      break;
  }
#endif

  // MBR is always the first sector of a disk
#ifdef __CC65__
  write_line("", 0);
  write_line("Writing Partition Table / Master Boot Record...", 1);
#endif
  build_mbr(sys_partition_start, sys_partition_sectors, fat_partition_start, fat_partition_sectors);
  sdcard_writesector(0);
  show_mbr();

  while (0) {
    build_mbr(sys_partition_start, sys_partition_sectors, fat_partition_start, fat_partition_sectors);
    sdcard_writesector(0);
    //  show_mbr();
  }

#ifdef __CC65__
  // write_line("Erasing reserved sectors before first partition...",0);
#endif
  // Blank intervening sectors
  //  sdcard_erase(0+1,sys_partition_start-1);

  if (1) {
    // Write MEGA65 System partition header sector
#ifdef __CC65__
    write_line("Writing MEGA65 System Partition header sector...", 1);
#endif
    build_mega65_sys_sector(sys_partition_sectors);
    sdcard_writesector(sys_partition_start);

#ifdef __CC65__
    write_line("Freeze  dir @ $        ", 1);
    screen_hex(screen_line_address - 79 + 15, sys_partition_freeze_dir);
    write_line("Service dir @ $        ", 1);
    screen_hex(screen_line_address - 79 + 15, sys_partition_service_dir);

#endif

    // Put a valid first config sector in place
    build_mega65_sys_config_sector();
    sdcard_writesector(sys_partition_start + 1L);

    // Erase the rest of the 1MB reserved area
    write_line("Erasing configuration area", 1);
    sdcard_erase(sys_partition_start + 2, sys_partition_start + 1023);

    // erase frozen program directory
    write_line("Erasing frozen program and system service directories", 1);
    sdcard_erase(sys_partition_freeze_dir, sys_partition_freeze_dir + freeze_dir_sectors - 1);

    // erase system service image directory
    sdcard_erase(sys_partition_service_dir, sys_partition_service_dir + service_dir_sectors - 1);
  }

#ifdef __CC65__
  write_line("Writing FAT Boot Sector...", 1);
#endif
  // Partition starts at fixed position of sector 2048, i.e., 1MB
  build_dosbootsector(fat_partition_sectors, fat_sectors);
  sdcard_writesector(fat_partition_start);
  sdcard_writesector(fat_partition_start + 6); // Backup boot sector at partition + 6

#ifdef __CC65__
  write_line("Writing FAT Information Block (and backup copy)...", 1);
#endif
  // FAT32 FS Information block (and backup)
  build_fs_information_sector(fs_clusters);
  sdcard_writesector(fat_partition_start + 1);
  sdcard_writesector(fat_partition_start + 7);

  // FATs
#ifndef __CC65__
  fprintf(stderr, "Writing FATs at offsets 0x%x AND 0x%x\r\n", fat1_sector * 512, fat2_sector * 512);
#else
  write_line("Writing FATs at $         and $         ...", 1);
  screen_hex(screen_line_address - 80 + 18, fat1_sector * 512);
  screen_hex(screen_line_address - 80 + 32, fat2_sector * 512);
#endif
  build_empty_fat();
  sdcard_writesector(fat_partition_start + fat1_sector);
  sdcard_writesector(fat_partition_start + fat2_sector);

#ifdef __CC65__
  write_line("Writing Root Directory...", 1);
#endif
  // Root directory
  build_root_dir(volume_name);
  sdcard_writesector(fat_partition_start + rootdir_sector);

#ifdef __CC65__
  write_line("", 0);
  write_line("Clearing file system data structures...", 1);
  POKE(0xd020U, 6);
#endif
  // Make sure all other sectors are empty
#if 1
  sdcard_erase(fat_partition_start + 1 + 1, fat_partition_start + 6 - 1);
  sdcard_erase(fat_partition_start + 6 + 1, fat_partition_start + fat1_sector - 1);
  sdcard_erase(fat_partition_start + fat1_sector + 1, fat_partition_start + fat2_sector - 1);
  sdcard_erase(fat_partition_start + fat2_sector + 1, fat_partition_start + rootdir_sector - 1);
  sdcard_erase(fat_partition_start + rootdir_sector + 1, fat_partition_start + rootdir_sector + 1 + sectors_per_cluster - 1);
#endif

#ifdef __CC65__
  /* Check if flash slot 0 contains embedded files that we should write to the SD card.
   */
  write_line("          ", 0);
  write_line("Scanning core for embedded files...", 1);
  scan_slots();
  {
    unsigned char i, slotCount, slotActive;
    slotCount = 0;
    slotActive = 0;
    for (i = 0; i < MAX_SLOT; i++) {
      if (!mega65slot[i].version[0] || !mega65slot[i].file_count) continue;
      strcpy(buffer, "(#) MEGA65 -    Files");
      buffer[1] = 0x30 + i;
      format_decimal((int)buffer + 13, mega65slot[i].file_count, 2);
      write_line(buffer, 3);
      write_line(mega65slot[i].version, 7);
      if (mega65slot[i].file_count) {
        slotCount++;
        slotActive |= 1<<i;
      }
    }
    if (!slotCount) {
      write_line("No slots with files found, skipping population.",1);
      recolour_last_line(7);
    }
    else {
      write_line("Populate SD card with embedded files from slot # or s to skip (#/s)?", 1);
      recolour_last_line(7);
      do {
        key = mega65_getkey();
        if (key == 's')
          break;
        for (i=0; i < MAX_SLOT; i++)
          if ((slotActive & (1<<i)) && key == 0x30 + i)
            break;
      } while (i == MAX_SLOT);
      if (key != 's')
        have_sdfiles = !populate_file_system(key&7);
      else
        write_line("Skipping SD card population.", 1);
    }
  }
#else

  // Process loading and reading of files from disk image
  printf("Processing %d arguments.\n", argc);
  for (int i = 1; i < argc; i++) {
    struct stat st;
    fprintf(stdout, "Writing file %s to SD card image\n", argv[i]);
    stat(argv[i], &st);

    FILE *f = fopen(argv[i], "r");
    if (!f) {
      fprintf(stderr, "Could not open file for reading\n");
      exit(-1);
    }

    char dosname[4096];
    char name[1024], extension[1024];
    bzero(name, sizeof(name));
    bzero(extension, sizeof(extension));
    if (sscanf(argv[i], "%[^.].%s", name, extension) != 2) {
      fprintf(stderr, "Could notparse name and extension from file name\n");
      exit(-1);
    }
    if (name[8] || extension[3]) {
      fprintf(stderr, "filename or extension too long. Must fit in 8.3 DOS filename. Got '%s'.'%s'\n", name, extension);
      exit(-1);
    }
    snprintf(dosname, 4096, "%-8s%-3s", name, extension);

    // make dos name upper case
    for (int i = 0; i < 12; i++)
      if (dosname[i] >= 'a' && dosname[i] <= 'z')
        dosname[i] -= 0x20;

    unsigned int first_sector = fat32_create_contiguous_file(dosname, st.st_size, fat_partition_start + rootdir_sector,
        fat_partition_start + fat1_sector, fat_partition_start + fat2_sector);
    if (first_sector) {
      // Write out sectors
      unsigned long addr;
      for (addr = 0; addr <= st.st_size; addr += 512) {
        fread(sector_buffer, 1, 512, f);
        sdcard_writesector(first_sector + (addr / 512));
      }
    }
    fclose(f);
    printf("File written.\n");
  }
#endif

#ifdef __CC65__

  POKE(0xd020U, 6);
  POKE(0xd021U, 6);
  write_line("", 0);
  write_line("SD Card has been formatted.", 1);
  recolour_last_line(0x37);
  if (!have_sdfiles)
    write_line("Remove, Copy SD Essentials and MEGA65.ROM, reinsert AND reboot.", 1);
  else if (!have_rom)
    write_line("Remove, Copy MEGA65.ROM, reinsert AND reboot.", 1);
  else
    write_line("Reboot to continue.", 1);
  recolour_last_line(0x37);

  if (!dont_confirm) {
    while (1)
      continue;
  }
  else {

    write_line("Press ALMOST ANY KEY to format next card", 1);
    while (!PEEK(0xD610))
      continue;
    POKE(0xD610, 0);

    goto next_card;
  }

#else
  return 0;
#endif
}

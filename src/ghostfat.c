/*
 * The MIT License (MIT)
 *
 * Copyright (c) Microsoft Corporation
 * Copyright (c) Ha Thach for Adafruit Industries
 * Copyright (c) Henry Gabryjelski
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "nvs_flash.h"
#include "nvs.h"

#include "compile_date.h"
#include "board_api.h"
#include "uf2.h"
#include "esp_private/system_internal.h" // for esp_restart()

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+

// ota0 partition size
static uint32_t _flash_size;
// size of the measurement data inside the ota1 partition
static uint32_t _measurement_flash_size;

#define STATIC_ASSERT(_exp) _Static_assert(_exp, "static assert failed")

#define STR0(x) #x
#define STR(x) STR0(x)

#define UF2_ARRAY_SIZE(_arr)    ( sizeof(_arr) / sizeof(_arr[0]) )
#define UF2_DIV_CEIL(_v, _d)    ( ((_v) / (_d)) + ((_v) % (_d) ? 1 : 0) )

typedef struct {
    uint8_t JumpInstruction[3];
    uint8_t OEMInfo[8];
    uint16_t SectorSize;
    uint8_t SectorsPerCluster;
    uint16_t ReservedSectors;
    uint8_t FATCopies;
    uint16_t RootDirectoryEntries;
    uint16_t TotalSectors16;
    uint8_t MediaDescriptor;
    uint16_t SectorsPerFAT;
    uint16_t SectorsPerTrack;
    uint16_t Heads;
    uint32_t HiddenSectors;
    uint32_t TotalSectors32;
    uint8_t PhysicalDriveNum;
    uint8_t Reserved;
    uint8_t ExtendedBootSig;
    uint32_t VolumeSerialNumber;
    uint8_t VolumeLabel[11];
    uint8_t FilesystemIdentifier[8];
} __attribute__((packed)) FAT_BootBlock;

typedef struct {
    char name[8];
    char ext[3];
    uint8_t attrs;
    uint8_t reserved;
    uint8_t createTimeFine;
    uint16_t createTime;
    uint16_t createDate;
    uint16_t lastAccessDate;
    uint16_t highStartCluster;
    uint16_t updateTime;
    uint16_t updateDate;
    uint16_t startCluster;
    uint32_t size;
} __attribute__((packed)) DirEntry;
STATIC_ASSERT(sizeof(DirEntry) == 32);

typedef struct FileContent {
  char const name[11];
  void const * content;
  uint32_t size;       // OK to use uint32_T b/c FAT32 limits filesize to (4GiB - 2)

  // computing fields based on index and size
  uint16_t cluster_start;
  uint16_t cluster_end;
} FileContent_t;

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+

#define BPB_SECTOR_SIZE           ( 512)
#define BPB_SECTORS_PER_CLUSTER   (CFG_UF2_SECTORS_PER_CLUSTER)
#define BPB_RESERVED_SECTORS      (   1)
#define BPB_NUMBER_OF_FATS        (   2)
#define BPB_ROOT_DIR_ENTRIES      (  64)
#define BPB_TOTAL_SECTORS         CFG_UF2_NUM_BLOCKS
#define BPB_MEDIA_DESCRIPTOR_BYTE (0xF8)
#define FAT_ENTRY_SIZE            (2)
#define FAT_ENTRIES_PER_SECTOR    (BPB_SECTOR_SIZE / FAT_ENTRY_SIZE)
#define FAT_END_OF_CHAIN          (0xFFFF)

// NOTE: MS specification explicitly allows FAT to be larger than necessary
#define TOTAL_CLUSTERS_ROUND_UP   UF2_DIV_CEIL(BPB_TOTAL_SECTORS, BPB_SECTORS_PER_CLUSTER)
#define BPB_SECTORS_PER_FAT       UF2_DIV_CEIL(TOTAL_CLUSTERS_ROUND_UP, FAT_ENTRIES_PER_SECTOR)
#define DIRENTRIES_PER_SECTOR     (BPB_SECTOR_SIZE/sizeof(DirEntry))
#define ROOT_DIR_SECTOR_COUNT     UF2_DIV_CEIL(BPB_ROOT_DIR_ENTRIES, DIRENTRIES_PER_SECTOR)
#define BPB_BYTES_PER_CLUSTER     (BPB_SECTOR_SIZE * BPB_SECTORS_PER_CLUSTER)

STATIC_ASSERT((BPB_SECTORS_PER_CLUSTER & (BPB_SECTORS_PER_CLUSTER-1)) == 0); // sectors per cluster must be power of two
STATIC_ASSERT(BPB_SECTOR_SIZE                              ==       512); // GhostFAT does not support other sector sizes (currently)
STATIC_ASSERT(BPB_NUMBER_OF_FATS                           ==         2); // FAT highest compatibility
STATIC_ASSERT(sizeof(DirEntry)                             ==        32); // FAT requirement
STATIC_ASSERT(BPB_SECTOR_SIZE % sizeof(DirEntry)           ==         0); // FAT requirement
STATIC_ASSERT(BPB_ROOT_DIR_ENTRIES % DIRENTRIES_PER_SECTOR ==         0); // FAT requirement
STATIC_ASSERT(BPB_BYTES_PER_CLUSTER                        <= (32*1024)); // FAT requirement (64k+ has known compatibility problems)
STATIC_ASSERT(FAT_ENTRIES_PER_SECTOR                       ==       256); // FAT requirement

#define UF2_FIRMWARE_BYTES_PER_SECTOR   256
#define UF2_SECTOR_COUNT                (_flash_size / UF2_FIRMWARE_BYTES_PER_SECTOR)
#define UF2_BYTE_COUNT                  (UF2_SECTOR_COUNT * BPB_SECTOR_SIZE) // always a multiple of sector size, per UF2 spec

#define MEASUREMNT_SECOTR_COUNT         (_measurement_flash_size / UF2_FIRMWARE_BYTES_PER_SECTOR)
#define MEASURMENT_BYTE_COUNT           (MEASUREMNT_SECOTR_COUNT * BPB_SECTOR_SIZE)
// Define a general macro for calculating byte count
#define BYTE_COUNT(_flash_size)        (((_flash_size) / UF2_FIRMWARE_BYTES_PER_SECTOR) * BPB_SECTOR_SIZE)


const uintptr_t SERIALNUM_ADDRESS = 0x3FF000;
#define SERIALNUM_LEN     5

char infoUf2File[128*3] =
    "EnertyUF2 Bootloader " UF2_VERSION "\r\n"
    "Model: " UF2_PRODUCT_NAME "\r\n"
    "Board-ID: " UF2_BOARD_ID "\r\n"
    "Date: " COMPILE_DATE "\r\n"
    "Serial Number: AAAAAAAAAAA\r\n"
    "Flash Size: 0x";

TINYUF2_CONST char indexFile[] =
    "<!doctype html>\n"
    "<html>"
    "<body>"
    "<script>\n"
    "location.replace(\"" UF2_INDEX_URL "\");\n"
    "</script>"
    "</body>"
    "</html>\n";

char test_csv_data[] = "time,CT1,CT2,CT3\n"
                        "0,0,0,0\n"
                        "1,1,1,1\n"
                        "2,2,2,2\n"
                        "3,3,3,3\n"
                        "4,4,4,4\n"
                        "5,5,5,5\n"
                        "6,6,6,6\n"
                        "7,7,7,7\n"
                        "8,8,8,8\n"
                        "9,9,9,9\n";


#ifdef TINYUF2_FAVICON_HEADER
#include TINYUF2_FAVICON_HEADER
const char autorunFile[] = "[Autorun]\r\nIcon=FAVICON.ICO\r\n";
#endif

// size of CURRENT.UF2:
static FileContent_t info[] = {
    {.name = "INFO_UF2TXT", .content = infoUf2File , .size = sizeof(infoUf2File) - 1},
    {.name = "INDEX   HTM", .content = indexFile   , .size = sizeof(indexFile  ) - 1},
#ifdef TINYUF2_FAVICON_HEADER
    {.name = "AUTORUN INF", .content = autorunFile , .size = sizeof(autorunFile) - 1},
    {.name = "FAVICON ICO", .content = favicon_data, .size = favicon_len            },
#endif
    // {.name = "SERIAL  BIN", .content = serialNumberHex, .size = sizeof(serialNumberHex)}, // remove this
    {.name = "TEST    CSV", .content = test_csv_data, .size = sizeof(test_csv_data) - 1},
    {.name = "MEASDAT CSV", .content = NULL, .size = 0},
    // current.uf2 must be the last element and its content must be NULL
    {.name = "CURRENT UF2", .content = NULL       , .size = 0                       },
};

enum {
  NUM_FILES = sizeof(info) / sizeof(info[0]),
  NUM_DIRENTRIES = NUM_FILES + 1 // including volume label as first root directory entry
};

enum {
  FID_INFO = 0,
  FID_INDEX = 1,
  FID_MEASURMENT_DATA = NUM_FILES - 2,
  FID_UF2 = NUM_FILES - 1,
};

STATIC_ASSERT(NUM_DIRENTRIES < BPB_ROOT_DIR_ENTRIES);  // FAT requirement -- Ensures BPB reserves sufficient entries for all files
STATIC_ASSERT(NUM_DIRENTRIES < DIRENTRIES_PER_SECTOR); // GhostFAT bug workaround -- else, code overflows buffer

#define NUM_SECTORS_IN_DATA_REGION (BPB_TOTAL_SECTORS - BPB_RESERVED_SECTORS - (BPB_NUMBER_OF_FATS * BPB_SECTORS_PER_FAT) - ROOT_DIR_SECTOR_COUNT)
#define CLUSTER_COUNT              (NUM_SECTORS_IN_DATA_REGION / BPB_SECTORS_PER_CLUSTER)

// Ensure cluster count results in a valid FAT16 volume!
STATIC_ASSERT( CLUSTER_COUNT >= 0x0FF5 && CLUSTER_COUNT < 0xFFF5 );

// Many existing FAT implementations have small (1-16) off-by-one style errors
// So, avoid being within 32 of those limits for even greater compatibility.
STATIC_ASSERT( CLUSTER_COUNT >= 0x1015 && CLUSTER_COUNT < 0xFFD5 );

#define FS_START_FAT0_SECTOR      BPB_RESERVED_SECTORS
#define FS_START_FAT1_SECTOR      (FS_START_FAT0_SECTOR + BPB_SECTORS_PER_FAT)
#define FS_START_ROOTDIR_SECTOR   (FS_START_FAT1_SECTOR + BPB_SECTORS_PER_FAT)
#define FS_START_CLUSTERS_SECTOR  (FS_START_ROOTDIR_SECTOR + ROOT_DIR_SECTOR_COUNT)

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+

static FAT_BootBlock TINYUF2_CONST BootBlock = {
    .JumpInstruction      = {0xeb, 0x3c, 0x90},
    .OEMInfo              = "UF2 UF2 ",
    .SectorSize           = BPB_SECTOR_SIZE,
    .SectorsPerCluster    = BPB_SECTORS_PER_CLUSTER,
    .ReservedSectors      = BPB_RESERVED_SECTORS,
    .FATCopies            = BPB_NUMBER_OF_FATS,
    .RootDirectoryEntries = BPB_ROOT_DIR_ENTRIES,
    .TotalSectors16       = (BPB_TOTAL_SECTORS > 0xFFFF) ? 0 : BPB_TOTAL_SECTORS,
    .MediaDescriptor      = BPB_MEDIA_DESCRIPTOR_BYTE,
    .SectorsPerFAT        = BPB_SECTORS_PER_FAT,
    .SectorsPerTrack      = 1,
    .Heads                = 1,
    .TotalSectors32       = (BPB_TOTAL_SECTORS > 0xFFFF) ? BPB_TOTAL_SECTORS : 0,
    .PhysicalDriveNum     = 0x80, // to match MediaDescriptor of 0xF8
    .ExtendedBootSig      = 0x29,
    .VolumeSerialNumber   = 0x00420042,
    .VolumeLabel          = UF2_VOLUME_LABEL,
    .FilesystemIdentifier = "FAT16   ",
};

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+

static inline bool is_uf2_block (UF2_Block const *bl) {
  return (bl->magicStart0 == UF2_MAGIC_START0) &&
         (bl->magicStart1 == UF2_MAGIC_START1) &&
         (bl->magicEnd == UF2_MAGIC_END) &&
         (bl->flags & UF2_FLAG_FAMILYID) &&
         !(bl->flags & UF2_FLAG_NOFLASH);
}

static inline bool is_serialnum_block (SerialNum_Block const *bl) {
  return (bl->magicStart0 == SERIALNUM_MAGIC_START0) &&
         (bl->magicStart1 == SERIALNUM_MAGIC_START1) &&
         (bl->magicEnd == SERIALNUM_MAGIC_END);
}

esp_err_t serialnum_from_nvs(nvs_handle_t* nvs, uint8_t* serialNumberHex) {
  size_t required_size = 0;
  esp_err_t err = nvs_get_blob(*nvs, "serialnum", NULL, &required_size);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
  if (required_size == 0) {
    // serialNumberHex = { 0x01, 0x23, 0x45, 0x67, 0x89 };
    serialNumberHex[0] = 0x01;
    serialNumberHex[1] = 0x23;
    serialNumberHex[2] = 0x45;
    serialNumberHex[3] = 0x67;
    serialNumberHex[4] = 0x89;
    serialNumberHex[5] = 0x00;
    return ESP_OK;
  }
  if (required_size != 6) return ESP_ERR_INVALID_SIZE;
  err = nvs_get_blob(*nvs, "serialnum", serialNumberHex, &required_size);
  if (err != ESP_OK) return err;
  return ESP_OK;
}

size_t get_measurment_data_size_nvs(nvs_handle_t* nvs){
  uint32_t size;
  size_t required_size = sizeof(size);
  esp_err_t err = nvs_get_blob(*nvs, "measurment_data_size", &size, &required_size);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return 512;
  return size;
}

esp_err_t serialnum_to_nvs(uint8_t serialNumberHex[6]) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;

  err = nvs_set_blob(nvs, "serialnum", serialNumberHex, 6);
  if (err != ESP_OK) return err;

  // Commit
  err = nvs_commit(nvs);
  if (err != ESP_OK) return err;

  //close
  nvs_close(nvs);
  esp_restart();
  return ESP_OK;
}

esp_err_t init_nvs_partition(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  return err;
}

// cache the cluster start offset for each file
// this allows more flexible algorithms w/o O(n) time
static void init_starting_clusters(void) {
  // +2 because FAT decided first data sector would be in cluster number 2, rather than zero
  uint16_t start_cluster = 2;

  for (uint16_t i = 0; i < NUM_FILES; i++) {
    info[i].cluster_start = start_cluster;
    info[i].cluster_end = start_cluster + UF2_DIV_CEIL(info[i].size, BPB_SECTOR_SIZE*BPB_SECTORS_PER_CLUSTER) - 1;

    start_cluster = info[i].cluster_end + 1;
  }
}

// get file index for file that uses the cluster
// if cluster is past last file, returns ( NUM_FILES-1 ).
//
// Caller must still check if a particular *sector*
// contains data from the file's contents, as there
// are often padding sectors, including all the unused
// sectors past the end of the media.
static uint32_t info_index_of(uint32_t cluster) {
  // default results for invalid requests is the index of the last file (CURRENT.UF2)
  if (cluster >= 0xFFF0) return FID_UF2;

  for (uint32_t i = 0; i < NUM_FILES; i++) {
    if ( (info[i].cluster_start <= cluster) && (cluster <= info[i].cluster_end) ) {
      return i;
    }
  }

  return FID_UF2;
}

static void u32_to_hexstr(uint32_t value, char* buffer) {
  const char hexDigits[] = "0123456789ABCDEF";
  size_t i;

  for (i = 0; i < 8; i++) {
    buffer[i] = hexDigits[(value >> (28 - i * 4)) & 0xF];
  }
  buffer[i] = '\0';
}

static void u8_to_hexstr(uint8_t value[], size_t arr_len, char* buffer) {
  const char hexDigits[] = "0123456789ABCDEF";
  size_t i;

  for (i = 0; i < arr_len; i++) {
    buffer[i * 2] = hexDigits[(value[i] >> 4) & 0xF];  // High nibble
    buffer[i * 2 + 1] = hexDigits[value[i] & 0xF];      // Low nibble
  }
  buffer[i * 2] = '\0';  // Null terminator at the end
}

void uf2_init(void) {
  // TODO maybe limit to application size only if possible board_flash_app_size()
  _flash_size = board_flash_size();
  // update CURRENT.UF2 file size
  info[FID_UF2].size = UF2_BYTE_COUNT;

  // ADDED BY ENERTY
  uint8_t serialNumberHex[6];
  esp_err_t err = init_nvs_partition(); // initialize the NVS partition for serial number storage
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND){
    serialNumberHex[0] = 0x10;
    serialNumberHex[1] = 0x10;
    serialNumberHex[2] = 0x10;
    serialNumberHex[3] = 0x11;
    serialNumberHex[4] = 0x11;
    serialNumberHex[5] = 0x00;
  } else if (err != ESP_OK)
  {
    serialNumberHex[0] = 0x11;
    serialNumberHex[1] = 0x11;
    serialNumberHex[2] = 0x11;
    serialNumberHex[3] = 0x00;
    serialNumberHex[4] = 0x00;
    serialNumberHex[5] = 0x00;
  } else{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs);
    // Replace AAAAAAAAAAA with the actual serial number
    err = serialnum_from_nvs(&nvs, serialNumberHex);
    if (err == ESP_ERR_INVALID_SIZE){
      serialNumberHex[0] = 0x10;
      serialNumberHex[1] = 0x11;
      serialNumberHex[2] = 0x10;
      serialNumberHex[3] = 0x11;
      serialNumberHex[4] = 0x10;
      serialNumberHex[5] = 0x00;
    } else if (err == ESP_ERR_NVS_NOT_FOUND){
      // serial number not found in NVS, use default
      serialNumberHex[0] = 0x10;
      serialNumberHex[1] = 0x10;
      serialNumberHex[2] = 0x10;
      serialNumberHex[3] = 0x10;
      serialNumberHex[4] = 0x10;
      serialNumberHex[5] = 0x00;
    } else if (err != ESP_OK) {
      serialNumberHex[0] = 0x11;
      serialNumberHex[1] = 0x00;
      serialNumberHex[2] = 0x11;
      serialNumberHex[3] = 0x00;
      serialNumberHex[4] = 0x11;
      serialNumberHex[5] = 0x00;
    }
    char serialNumber[12];  // 11 characters + null terminator
    u8_to_hexstr(serialNumberHex + 1, 5, serialNumber + 1); // the first byte should not be converted, it is the hardware identifier and already a character
    serialNumber[0] = serialNumberHex[0];  // the first byte is a character

    // Find and replace "AAAAAAAAAAA" in the info file
    char *serialPosition = strstr(infoUf2File, "AAAAAAAAAAA");
    if (serialPosition != NULL) {
      // Replace the AAAAAAAAAAA with the actual serial number
      strncpy(serialPosition, serialNumber, 11);  // 10 characters (no null terminator)
    }

    // get the size measurment data from the NVS+
    _measurement_flash_size = get_measurment_data_size_nvs(&nvs);
    info[FID_MEASURMENT_DATA].size = MEASURMENT_BYTE_COUNT;
    nvs_close(nvs);
  }
  
  // update INFO_UF2.TXT with flash size if having enough space (8 bytes)
  size_t txt_len = strlen(infoUf2File);
  size_t const max_len = sizeof(infoUf2File) - 1;
  if ( max_len - txt_len > 8) {
    u32_to_hexstr(_flash_size, infoUf2File + txt_len);
    txt_len += 8;

    if (max_len - txt_len > 6) {
      strcpy(infoUf2File + txt_len, " bytes");
      txt_len += 6;
    }
  }
  info[FID_INFO].size = txt_len;

  init_starting_clusters(); // fill the info struct with cluster start and end values
}

/*------------------------------------------------------------------*/
/* Read CURRENT.UF2
 *------------------------------------------------------------------*/
void padded_memcpy (char *dst, char const *src, int len) {
  for ( int i = 0; i < len; ++i ) {
    if ( *src ) {
      *dst = *src++;
    } else {
      *dst = ' ';
    }
    dst++;
  }
}

void uf2_read_block (uint32_t block_no, uint8_t *data) {
  memset(data, 0, BPB_SECTOR_SIZE);
  uint32_t sectionRelativeSector = block_no;

  if ( block_no == 0 ) {
    // Request was for the Boot block
    memcpy(data, &BootBlock, sizeof(BootBlock));
    data[510] = 0x55;    // Always at offsets 510/511, even when BPB_SECTOR_SIZE is larger
    data[511] = 0xaa;    // Always at offsets 510/511, even when BPB_SECTOR_SIZE is larger
  }
  else if ( block_no < FS_START_ROOTDIR_SECTOR ) {
    // Request was for a FAT table sector
    sectionRelativeSector -= FS_START_FAT0_SECTOR;

    // second FAT is same as the first... use sectionRelativeSector to write data
    if ( sectionRelativeSector >= BPB_SECTORS_PER_FAT ) {
      sectionRelativeSector -= BPB_SECTORS_PER_FAT;
    }

    uint16_t* data16 = (uint16_t*) (void*) data;
    uint32_t sectorFirstCluster = sectionRelativeSector * FAT_ENTRIES_PER_SECTOR;
    uint32_t firstUnusedCluster = info[FID_UF2].cluster_end + 1;

    // OPTIMIZATION:
    // Because all files are contiguous, the FAT CHAIN entries
    // are all set to (cluster+1) to point to the next cluster.
    // All clusters past the last used cluster of the last file
    // are set to zero.
    //
    // EXCEPTIONS:
    // 1. Clusters 0 and 1 require special handling
    // 2. Final cluster of each file must be set to END_OF_CHAIN
    //

    // Set default FAT values first.
    for (uint16_t i = 0; i < FAT_ENTRIES_PER_SECTOR; i++) {
      uint32_t cluster = i + sectorFirstCluster;
      if (cluster >= firstUnusedCluster) {
        data16[i] = 0;
      }
      else {
        data16[i] = cluster + 1;
      }
    }

    // Exception #1: clusters 0 and 1 need special handling
    if (sectionRelativeSector == 0) {
      data[0] = BPB_MEDIA_DESCRIPTOR_BYTE;
      data[1] = 0xff;
      data16[1] = FAT_END_OF_CHAIN; // cluster 1 is reserved
    }

    // Exception #2: the final cluster of each file must be set to END_OF_CHAIN
    for (uint32_t i = 0; i < NUM_FILES; i++) {
      uint32_t lastClusterOfFile = info[i].cluster_end;
      if (lastClusterOfFile >= sectorFirstCluster) {
        uint32_t idx = lastClusterOfFile - sectorFirstCluster;
        if (idx < FAT_ENTRIES_PER_SECTOR) {
          // that last cluster of the file is in this sector
          data16[idx] = FAT_END_OF_CHAIN;
        }
      }
    }
  }
  else if ( block_no < FS_START_CLUSTERS_SECTOR ) {
    // Request was for a (root) directory sector .. root because not supporting subdirectories (yet)
    sectionRelativeSector -= FS_START_ROOTDIR_SECTOR;

    DirEntry *d = (void*) data;                   // pointer to next free DirEntry this sector
    int remainingEntries = DIRENTRIES_PER_SECTOR; // remaining count of DirEntries this sector

    uint32_t startingFileIndex;

    if ( sectionRelativeSector == 0 ) {
      // volume label is first directory entry
      padded_memcpy(d->name, (char const*) BootBlock.VolumeLabel, 11);
      d->attrs = 0x28;
      d++;
      remainingEntries--;

      startingFileIndex = 0;
    }else {
      // -1 to account for volume label in first sector
      startingFileIndex = DIRENTRIES_PER_SECTOR * sectionRelativeSector - 1;
    }

    for ( uint32_t fileIndex = startingFileIndex;
          remainingEntries > 0 && fileIndex < NUM_FILES; // while space remains in buffer and more files to add...
          fileIndex++, d++ ) {
      // WARNING -- code presumes all files take exactly one directory entry (no long file names!)
      uint32_t const startCluster = info[fileIndex].cluster_start;

      FileContent_t const *inf = &info[fileIndex];
      padded_memcpy(d->name, inf->name, 11);
      d->createTimeFine   = COMPILE_SECONDS_INT % 2 * 100;
      d->createTime       = COMPILE_DOS_TIME;
      d->createDate       = COMPILE_DOS_DATE;
      d->lastAccessDate   = COMPILE_DOS_DATE;
      d->highStartCluster = startCluster >> 16;
      d->updateTime       = COMPILE_DOS_TIME;
      d->updateDate       = COMPILE_DOS_DATE;
      d->startCluster     = startCluster & 0xFFFF;
      d->size             = (inf->content ? inf->size : UF2_BYTE_COUNT);
    }
  }
  else if ( block_no < BPB_TOTAL_SECTORS ) {
    // Request was to read from the data area (files, unused space, ...)
    sectionRelativeSector -= FS_START_CLUSTERS_SECTOR;

    // plus 2 for first data cluster offset
    uint32_t fid = info_index_of(2 + sectionRelativeSector / BPB_SECTORS_PER_CLUSTER);
    FileContent_t const * inf = &info[fid];

    uint32_t fileRelativeSector = sectionRelativeSector - (info[fid].cluster_start-2) * BPB_SECTORS_PER_CLUSTER;

    if ( fid != FID_UF2 && fid != FID_MEASURMENT_DATA ) {
      // Handle all files other than CURRENT.UF2
      size_t fileContentStartOffset = fileRelativeSector * BPB_SECTOR_SIZE;
      size_t fileContentLength = inf->size;

      // nothing to copy if already past the end of the file (only when >1 sector per cluster)
      if (fileContentLength > fileContentStartOffset) {
        // obviously, 2nd and later sectors should not copy data from the start
        const void * dataStart = (inf->content) + fileContentStartOffset;
        // limit number of bytes of data to be copied to remaining valid bytes
        size_t bytesToCopy = fileContentLength - fileContentStartOffset;
        // and further limit that to a single sector at a time
        if (bytesToCopy > BPB_SECTOR_SIZE) {
          bytesToCopy = BPB_SECTOR_SIZE;
        }
        memcpy(data, dataStart, bytesToCopy);
      }
    }
    else if(fid == FID_UF2) {
      // CURRENT.UF2: generate data on-the-fly
      uint32_t addr = BOARD_FLASH_APP_START + (fileRelativeSector * UF2_FIRMWARE_BYTES_PER_SECTOR);
      if ( addr < (BOARD_FLASH_ADDR_ZERO + _flash_size) ) {
        UF2_Block *bl = (void*) data;
        bl->magicStart0 = UF2_MAGIC_START0;
        bl->magicStart1 = UF2_MAGIC_START1;
        bl->magicEnd = UF2_MAGIC_END;
        bl->blockNo = fileRelativeSector;
        bl->numBlocks = UF2_SECTOR_COUNT;
        bl->targetAddr = addr;
        bl->payloadSize = UF2_FIRMWARE_BYTES_PER_SECTOR;
        bl->flags = UF2_FLAG_FAMILYID;
        bl->familyID = BOARD_UF2_FAMILY_ID;

        board_flash_read(addr, bl->data, bl->payloadSize);
      }
    }
    else {
      // Handle all files other than CURRENT.UF2
      size_t fileContentStartOffset = (fileRelativeSector * BPB_SECTOR_SIZE);
      size_t fileContentLength = inf->size;

            // nothing to copy if already past the end of the file (only when >1 sector per cluster)
      if (fileContentLength > fileContentStartOffset) {
        // limit number of bytes of data to be copied to remaining valid bytes
        size_t bytesToCopy = fileContentLength - fileContentStartOffset;
        // and further limit that to a single sector at a time
        if (bytesToCopy > BPB_SECTOR_SIZE) {
          bytesToCopy = BPB_SECTOR_SIZE;
        }
        board_measuremnt_data_read(fileContentStartOffset, data, bytesToCopy);
      }
    }
  }
}

/*------------------------------------------------------------------*/
/* Write UF2
 *------------------------------------------------------------------*/

/**
 * Write an uf2 block wrapped by 512 sector.
 * @return number of bytes processed, only 3 following values
 *  -1 : if not an uf2 block
 * 512 : write is successful (BPB_SECTOR_SIZE == 512)
 *   0 : is busy with flashing, tinyusb stack will call write_block again with the same parameters later on
 */
int uf2_write_block (uint32_t block_no, uint8_t *data, WriteState *state) {
  (void) block_no;
  UF2_Block *bl = (void*) data;

  if ( !is_uf2_block(bl) ) {
    // added by ENERTY
    SerialNum_Block *sn = (void*) data;
    if ( is_serialnum_block(sn) ) {
      // write serial number to nvs
      esp_err_t err = serialnum_to_nvs(sn->serialNumber);
      if (err != ESP_OK) {
        return -1;
      }
      return BPB_SECTOR_SIZE;
    }
    return -1;
  }

  if (bl->familyID == BOARD_UF2_FAMILY_ID) {
    // generic family ID
    board_flash_write(bl->targetAddr, bl->data, bl->payloadSize);
  }else {
    // TODO family matches VID/PID
    return -1;
  }

  //------------- Update written blocks -------------//
  if ( bl->numBlocks ) {
    // Update state num blocks if needed
    if ( state->numBlocks != bl->numBlocks ) {
      if ( bl->numBlocks >= MAX_BLOCKS || state->numBlocks ) {
        state->numBlocks = 0xffffffff;
      }
      else {
        state->numBlocks = bl->numBlocks;
      }
    }

    if ( bl->blockNo < MAX_BLOCKS ) {
      uint8_t const mask = 1 << (bl->blockNo % 8);
      uint32_t const pos = bl->blockNo / 8;

      // only increase written number with new write (possibly prevent overwriting from OS)
      if ( !(state->writtenMask[pos] & mask) ) {
        state->writtenMask[pos] |= mask;
        state->numWritten++;
      }

      // flush last blocks
      // TODO numWritten can be smaller than numBlocks if return early
      if ( state->numWritten >= state->numBlocks ) {
        board_flash_flush();
      }
    }
  }

  return BPB_SECTOR_SIZE;
}

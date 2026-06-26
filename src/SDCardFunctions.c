#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "ff.h"
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "SDCardFunctions.h"


#define WPCS_ROM_U18 0
#define WPCS_ROM_U14 1
#define WPCS_ROM_U15 2

// XIP Memory Mapped Addresses
#define XIP_BASE_ADDRESS  0x10000000

// Flash offsets (Relative to 0x0 for flash_range operations)
#define FLASH_OFFSET_U18  0x100000  // 1MB - New starting boundary
#define FLASH_OFFSET_U14  0x180000  // +512KB
#define FLASH_OFFSET_U15  0x200000  // +512KB
#define FLASH_OFFSET_META 0x280000  // +512KB
#define FLASH_OFFSET_WAV  0x281000  // 4KB after metadata for sector alignment

#define FLASH_XIP_U18     (XIP_BASE_ADDRESS + FLASH_OFFSET_U18)
#define FLASH_XIP_U14     (XIP_BASE_ADDRESS + FLASH_OFFSET_U14)
#define FLASH_XIP_U15     (XIP_BASE_ADDRESS + FLASH_OFFSET_U15)
#define FLASH_XIP_META    (XIP_BASE_ADDRESS + FLASH_OFFSET_META)
#define FLASH_XIP_WAV     (XIP_BASE_ADDRESS + FLASH_OFFSET_WAV)

#define PIN_CD 6 // Card Detect switch

typedef struct {
    char ROMName[64];
    uint32_t SizeU18;
    uint32_t SizeU14;
    uint32_t SizeU15;
    uint32_t SizeWAV;
    uint32_t ChecksumU18;
    uint32_t ChecksumU14;
    uint32_t ChecksumU15;
} ROMMetadata_t;


static FATFS g_sdFs;



// Helper to perform case-insensitive string comparison for extensions
bool MatchExtension(const char *Filename, const char *TargetExt) {
    const char *dot = strrchr(Filename, '.');
    if (!dot) {
        return false;
    }
    
    while (*dot && *TargetExt) {
        if (tolower((unsigned char)*dot) != tolower((unsigned char)*TargetExt)) {
            return false;
        }
        dot++;
        TargetExt++;
    }
    return (*dot == '\0' && *TargetExt == '\0');
}

// Helper to verify if a file starts with a digit and is a WAV file
bool IsNumericWAV(const char *Filename) {
    if (!isdigit((unsigned char)Filename[0])) {
        return false;
    }
    return MatchExtension(Filename, ".wav");
}


uint8_t InitSDFilesystem(void) {
    // The CD switch connects to ground when a card is inserted
    if (gpio_get(PIN_CD) != 0) {
        return 0; // No card detected
    }

    // Mount the filesystem using the persistent object immediately
    if (f_mount(&g_sdFs, "0:", 1) != FR_OK) {
        return 0; // Error reading/mounting
    }

    return 1; // Success
}


uint8_t CheckSDCardContents(uint16_t *ROMFilesInRoot, uint16_t *ROMFilesInFolder, uint16_t *WAVFilesInRoot, uint16_t *WAVFilesInFolder) {
    DIR dir;
    FILINFO fno;

    // Initialize output parameters to zero
    *ROMFilesInRoot = 0;
    *ROMFilesInFolder = 0;
    *WAVFilesInRoot = 0;
    *WAVFilesInFolder = 0;
/*
    // The CD switch connects to ground when a card is inserted
    if (gpio_get(PIN_CD) != 0) {
        return 0; // No card detected
    }

    // Mount the filesystem using the persistent object
    if (f_mount(&g_sdFs, "0:", 1) != FR_OK) {
        return 0; // Error reading/mounting
    }
*/        

    // 1 & 3: Scan Root Directory for U18 ROMs and numeric WAV files
    if (f_opendir(&dir, "0:/") == FR_OK) {
        while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
            // IGNORE directories, hidden files, system files, and macOS dot-files
            if (!(fno.fattrib & AM_DIR) && !(fno.fattrib & AM_HID) && !(fno.fattrib & AM_SYS) && fno.fname[0] != '.') { 
                if (MatchExtension(fno.fname, ".u18")) {
                    (*ROMFilesInRoot)++;
                }
                if (IsNumericWAV(fno.fname)) {
                    (*WAVFilesInRoot)++;
                }
            }
        }
        f_closedir(&dir);
    }

    // 2: Scan ALL_SND_ROMS directory for U18 ROMs
    if (f_opendir(&dir, "0:/ALL_SND_ROMS") == FR_OK) {
        while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
            if (!(fno.fattrib & AM_DIR) && !(fno.fattrib & AM_HID) && !(fno.fattrib & AM_SYS) && fno.fname[0] != '.') {
                if (MatchExtension(fno.fname, ".u18")) {
                    (*ROMFilesInFolder)++;
                }
            }
        }
        f_closedir(&dir);
    }

    // 4: Scan HPMenuCallouts directory for any WAV files
    if (f_opendir(&dir, "0:/HPMenuCallouts") == FR_OK) {
        while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
            if (!(fno.fattrib & AM_DIR) && !(fno.fattrib & AM_HID) && !(fno.fattrib & AM_SYS) && fno.fname[0] != '.') {
                if (MatchExtension(fno.fname, ".wav")) {
                    (*WAVFilesInFolder)++;
                }
            }
        }
        f_closedir(&dir);
    }

    // The drive is deliberately left mounted for subsequent function calls
    return 1; 
}


// Calculates a basic checksum of the first 16 and last 16 bytes
uint32_t CalculateCustomChecksum(FIL *FileObj, uint32_t FileSize) {
    uint32_t checksum = 0;
    uint8_t buffer[16];
    UINT bytesRead;

    if (FileSize == 0) return 0;

    // Read start
    f_lseek(FileObj, 0);
    if (f_read(FileObj, buffer, sizeof(buffer), &bytesRead) == FR_OK) {
        for (UINT i = 0; i < bytesRead; i++) checksum += buffer[i];
    }

    // Read end
    if (FileSize > sizeof(buffer)) {
        f_lseek(FileObj, FileSize - sizeof(buffer));
        if (f_read(FileObj, buffer, sizeof(buffer), &bytesRead) == FR_OK) {
            for (UINT i = 0; i < bytesRead; i++) checksum += buffer[i];
        }
    }

    // Reset pointer for subsequent full file reads
    f_lseek(FileObj, 0); 
    return checksum;
}

// Handles erasing and writing a single file to a specific flash offset
// OutChecksum can be NULL if the file does not require a custom checksum (e.g., WAV)
bool FlashSingleFile(const char *FilePath, uint32_t FlashOffset, uint32_t *OutSize, uint32_t *OutChecksum, uint32_t *BytesProcessed, uint32_t TotalBytes, ProgressCallback Callback) {

    FIL file;
    if (f_open(&file, FilePath, FA_READ) != FR_OK) {
        if (OutSize) *OutSize = 0;
        if (OutChecksum) *OutChecksum = 0;
        return false; 
    }

    uint32_t fileSize = f_size(&file);
    if (OutSize) *OutSize = fileSize;
    if (OutChecksum) *OutChecksum = CalculateCustomChecksum(&file, fileSize);

    if (fileSize > 0) {
        // Flash erase must be a multiple of 4096 bytes
        uint32_t eraseSize = ((fileSize + (FLASH_SECTOR_SIZE - 1)) / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;
        
        uint32_t interrupts = save_and_disable_interrupts();
        flash_range_erase(FlashOffset, eraseSize);
        restore_interrupts(interrupts);

        uint8_t buffer[FLASH_SECTOR_SIZE];
        UINT bytesRead;
        uint32_t currentOffset = FlashOffset;

        while (f_read(&file, buffer, FLASH_SECTOR_SIZE, &bytesRead) == FR_OK && bytesRead > 0) {
            // Flash program must be a multiple of 256 bytes
            uint32_t writeSize = ((bytesRead + (FLASH_PAGE_SIZE - 1)) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE;
            
            // Pad the remainder of the page with 0xFF
            for (uint32_t i = bytesRead; i < writeSize; i++) {
                buffer[i] = 0xFF;
            }

            interrupts = save_and_disable_interrupts();
            flash_range_program(currentOffset, buffer, writeSize);
            restore_interrupts(interrupts);

            currentOffset += writeSize;
            *BytesProcessed += bytesRead;

            if (Callback && TotalBytes > 0) {
                uint8_t percent = (uint8_t)(((*BytesProcessed) * 100) / TotalBytes);
                Callback(percent);
            }
        }
    }

    f_close(&file);
    return true;
}

// Main function to write the set of ROMs, the WAV description, and Metadata
bool StoreROMData(const char *BaseROMPath, const char *BaseWAVPath, const char *ROMName, ProgressCallback Callback) {
    char pathU18[128];
    char pathU14[128];
    char pathU15[128];
    char pathWAV[128];

    // Construct full file paths assuming the folder inputs contain trailing slashes
    snprintf(pathU18, sizeof(pathU18), "%s%s.u18", BaseROMPath, ROMName);
    snprintf(pathU14, sizeof(pathU14), "%s%s.u14", BaseROMPath, ROMName);
    snprintf(pathU15, sizeof(pathU15), "%s%s.u15", BaseROMPath, ROMName);
    snprintf(pathWAV, sizeof(pathWAV), "%s%s.wav", BaseWAVPath, ROMName);

    FIL file;
    uint32_t totalBytes = 0;

    // Accumulate total bytes for the progress callback (U18 Mandatory)
    if (f_open(&file, pathU18, FA_READ) != FR_OK) {
        return false; 
    }
    totalBytes += f_size(&file);
    f_close(&file);

    // Accumulate U14 (Optional)
    if (f_open(&file, pathU14, FA_READ) == FR_OK) {
        totalBytes += f_size(&file);
        f_close(&file);
    }

    // Accumulate U15 (Optional)
    if (f_open(&file, pathU15, FA_READ) == FR_OK) {
        totalBytes += f_size(&file);
        f_close(&file);
    }

    // Accumulate WAV (Optional)
    if (f_open(&file, pathWAV, FA_READ) == FR_OK) {
        totalBytes += f_size(&file);
        f_close(&file);
    }

    ROMMetadata_t meta;
    memset(&meta, 0, sizeof(ROMMetadata_t));
    strncpy(meta.ROMName, ROMName, sizeof(meta.ROMName) - 1);

    uint32_t bytesProcessed = 0;

    // Flash U18
    FlashSingleFile(pathU18, FLASH_OFFSET_U18, &meta.SizeU18, &meta.ChecksumU18, &bytesProcessed, totalBytes, Callback);

    // Flash U14
    FlashSingleFile(pathU14, FLASH_OFFSET_U14, &meta.SizeU14, &meta.ChecksumU14, &bytesProcessed, totalBytes, Callback);

    // Flash U15
    FlashSingleFile(pathU15, FLASH_OFFSET_U15, &meta.SizeU15, &meta.ChecksumU15, &bytesProcessed, totalBytes, Callback);

    // Flash WAV (Passing NULL for checksum since it is not requested for audio)
    FlashSingleFile(pathWAV, FLASH_OFFSET_WAV, &meta.SizeWAV, NULL, &bytesProcessed, totalBytes, Callback);

    // Write Metadata Sector
    // Allocate a single flash page (256 bytes) to prevent stack overflow
    uint8_t metaPage[FLASH_PAGE_SIZE];
    memset(metaPage, 0xFF, FLASH_PAGE_SIZE);
    memcpy(metaPage, &meta, sizeof(ROMMetadata_t));

    uint32_t interrupts = save_and_disable_interrupts();
    
    // Erase the full 4096-byte sector
    flash_range_erase(FLASH_OFFSET_META, FLASH_SECTOR_SIZE);
    
    // Program only the first 256 bytes
    flash_range_program(FLASH_OFFSET_META, metaPage, FLASH_PAGE_SIZE);
    
    restore_interrupts(interrupts);

    if (Callback) {
        Callback(100);
    }

    return true;
}


// Helper to calculate checksum directly from memory-mapped flash
uint32_t CalculateXIPCustomChecksum(const uint8_t *Address, uint32_t Size) {
    uint32_t checksum = 0;
    
    if (Size == 0) return 0;

    // Read first 16 bytes
    uint32_t readSizeStart = (Size < 16) ? Size : 16;
    for (uint32_t i = 0; i < readSizeStart; i++) {
        checksum += Address[i];
    }

    // Read last 16 bytes
    if (Size > 16) {
        for (uint32_t i = 0; i < 16; i++) {
            checksum += Address[Size - 16 + i];
        }
    }

    return checksum;
}

uint8_t *GetROMAddressFromFlash(uint8_t romID) {
    switch (romID) {
        case WPCS_ROM_U18: return (uint8_t *)FLASH_XIP_U18;
        case WPCS_ROM_U14: return (uint8_t *)FLASH_XIP_U14;
        case WPCS_ROM_U15: return (uint8_t *)FLASH_XIP_U15;
        default: return NULL;
    }
}

uint32_t GetROMSizeFromFlash(uint8_t romID) {
    ROMMetadata_t *meta = (ROMMetadata_t *)FLASH_XIP_META;
    uint32_t expectedChecksum = 0;
    uint32_t size = 0;
    uint8_t *address = GetROMAddressFromFlash(romID);

    if (address == NULL) return 0;

    // Retrieve expected size and checksum from the metadata sector
    switch (romID) {
        case WPCS_ROM_U18:
            size = meta->SizeU18;
            expectedChecksum = meta->ChecksumU18;
            break;
        case WPCS_ROM_U14:
            size = meta->SizeU14;
            expectedChecksum = meta->ChecksumU14;
            break;
        case WPCS_ROM_U15:
            size = meta->SizeU15;
            expectedChecksum = meta->ChecksumU15;
            break;
    }

    if (size == 0) return 0; // ROM not present or metadata is empty/corrupt

    // Validate against stored checksum
    uint32_t actualChecksum = CalculateXIPCustomChecksum(address, size);
    if (actualChecksum != expectedChecksum) {
        return 0; // Checksum mismatch
    }

    return size;
}

uint32_t GetROMDescriptionWAVSize() {
    ROMMetadata_t *meta = (ROMMetadata_t *)FLASH_XIP_META;
    // 0xFFFFFFFF indicates empty/erased flash space
    if (meta->SizeWAV == 0xFFFFFFFF) {
        return 0;
    }
    return meta->SizeWAV;
}

uint8_t *GetROMDescriptionWAVAddress() {
    return (uint8_t *)FLASH_XIP_WAV;
}




uint8_t GetROMFilenameFromSDCard(const char *FilePath, uint8_t ROMNum, char *OutFilename, uint8_t MaxLen) {
    DIR dir;
    FILINFO fno;
    uint8_t matchCount = 0;

    if (OutFilename == NULL || MaxLen == 0) {
        return 0;
    }

    // Initialize buffer to empty string
    OutFilename[0] = '\0'; 

    if (f_opendir(&dir, FilePath) == FR_OK) {
        while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
            // IGNORE directories, hidden files, system files, and macOS dot-files
            if (!(fno.fattrib & AM_DIR) && !(fno.fattrib & AM_HID) && !(fno.fattrib & AM_SYS) && fno.fname[0] != '.') { 
                if (MatchExtension(fno.fname, ".u18")) {
                    if (matchCount == ROMNum) {
                        // Locate the dot to strip the extension
                        char *dot = strrchr(fno.fname, '.');
                        size_t nameLen = dot ? (size_t)(dot - fno.fname) : strlen(fno.fname);
                        
                        // Truncate if the filename exceeds the provided buffer length
                        if (nameLen >= MaxLen) {
                            nameLen = MaxLen - 1;
                        }
                        
                        strncpy(OutFilename, fno.fname, nameLen);
                        OutFilename[nameLen] = '\0';
                        
                        f_closedir(&dir);
                        return 1;
                    }
                    matchCount++;
                }
            }
        }
        f_closedir(&dir);
    }

    return 0;
}



uint8_t DoesFilenameMatchFlash(const char *FileName) {

    if (FileName == NULL) {
        return 0;
    }

    ROMMetadata_t *meta = (ROMMetadata_t *)FLASH_XIP_META;

    // A flash sector erased state is entirely 0xFF. 
    // If the first byte is 0xFF, no valid metadata has been written yet.
    if ((uint8_t)meta->ROMName[0] == 0xFF) {
        return 0;
    }

    // strncmp safely stops at a null terminator or the 64-character limit
    if (strncmp(FileName, meta->ROMName, 64) == 0) {
        return 1;
    }

    return 0;
}
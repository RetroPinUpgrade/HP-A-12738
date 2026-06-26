#ifndef SD_CARD_FUNCTIONS_H
#define SD_CARD_FUNCTIONS_H

#include "stdint.h"


typedef void (*ProgressCallback)(uint8_t);

uint8_t InitSDFilesystem(void);
uint8_t CheckSDCardContents(uint16_t *ROMFilesInRoot, uint16_t *ROMFilesInFolder, uint16_t *WAVFilesInRoot, uint16_t *WAVFilesInFolder);
uint32_t GetROMDescriptionWAVSize();
uint8_t *GetROMDescriptionWAVAddress();
uint32_t GetROMSizeFromFlash(uint8_t romID);
uint8_t *GetROMAddressFromFlash(uint8_t romID);
uint8_t GetROMFilenameFromSDCard(const char *FilePath, uint8_t ROMNum, char *OutFilename, uint8_t MaxLen);
uint8_t DoesFilenameMatchFlash(const char *FileName);
bool StoreROMData(const char *BaseROMPath, const char *BaseWAVPath, const char *ROMName, ProgressCallback Callback);

#endif    
#ifndef WPCS_BOARD_H
#define WPCS_BOARD_H

#include "WPCS6809.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WPCS_RAM_SIZE                        0x2000
#define WPCS_ROM_SIZE                        524288
#define WPCS_HARDWARE_UPPER_ADDRESS          0x3FFF
#define WPCS_SYSTEM_ROM_LOWER_ADDRESS        0x4000
#define WPCS_SYSTEM_ROM_UPPER_ADDRESS        0xFFFF
#define WPCS_BANK_SELECT                     0x2000
#define WPCS_YM2151                          0x2400
#define WPCS_AD7524                          0x2800
#define WPCS_55536_POS                       0x2C00
#define WPCS_WPCSBoard_DATA_RCV              0x3000
#define WPCS_55536_NEG                       0x3400
#define WPCS_EPOT                            0x3800
#define WPCS_WPCSBoard_DATA_SND              0x3C00

#define WPCS_ROM_U18 0
#define WPCS_ROM_U14 1
#define WPCS_ROM_U15 2


typedef uint8_t byte;

bool WPCSBoardInit();
void WPCSBoardRelease();

void WPCSBoardReset();
void WPCSBoardSetROMAddress(uint8_t chipId, uint8_t *romLocation, uint32_t romSize);
//void WPCSBoardSetROMAddress(uint8_t *romLocation, uint32_t romSize);
void WPCSBoardSetCabinetInput(byte value);
void WPCSBoardSetSwitchInput(int switchNr, int optionalValue);
void WPCSBoardSetFliptronicsInput(int value, int optionalValue);
void WPCSBoardToggleMidnightMadnessMode();
void WPCSBoardSetDipSwitchByte(byte dipSwitch);
byte WPCSBoardGetDipSwitchByte();
void WPCSBoardStart();
uint16_t WPCSBoardStep(uint32_t curTicks);
void WPCSBoardWriteMemory(unsigned int offset, byte value);
byte WPCSBoardRead8(unsigned short offset);
void WPCSBoardWrite8(unsigned short offset, byte value);
byte WPCSBoardBankswitchedRead(unsigned int offset);
void WPCSBoardHardwareWrite(unsigned int offset, byte value);
byte WPCSBoardHardwareRead(unsigned int offset);
uint8_t *WPCSBoardGetNVRAMStart();
uint16_t WPCSBoardGetNVRAMSize();

void WPCSBoardFIRQ(bool firqOn);
void WPCSBoardIRQ();

void CVSDProcessTick(void);

void WPCSWriteData(uint8_t value);
uint8_t WPCSReadControl();
uint8_t WPCSReadData();

void WPCSBoardEnableLogging(bool loggingOn);
uint8_t WPCSGetLastCommand();
bool WPCSNewCommandToShowApp();
uint8_t WPCSGetLastResponse();
bool WPCSNewResponseToShowApp();


#ifdef __cplusplus
}
#endif

#endif
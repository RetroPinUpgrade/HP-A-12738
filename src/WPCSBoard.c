#include "stdio.h"
#include "WPCSBoard.h"
#include "pico/multicore.h"
#include "SharedState.h"
#include "pico/platform.h"
#include "hardware/pio.h"


/*
Notes:
YM2151 - synthesizer chip
2064 - RAM (8k or 0x2000)
68B09 - CPU (2MHz)
    FIRQ comes from IRQ line of YM2151


Jumpers on White Water:
  W1: no
  W2: no
  W3: yes
  W4: no
  W5: yes
  W6: no
  W7: yes
  W8: yes
  W9: yes
  W10: yes

Address Map / Registers:
0x0000 - 0x1FFF : RAM (2064 at U9)

0x20XX : latches A16 - A23
0x24XX : CS of YM2151 (A0 control's the chip's "port")
0x28XX : CS of AD7524
0x2CXX : preset for 55536 clock (produces CVSD)
0x30XX : clock for reading data from controller (U30)
0x34XX : clear for 55536 clock
0x38XX : EPOT (strobes D0 and D1 into latches that control X9503 - falling D0 increments, D1 is direction
0x3CXX : clock for latching data back to controller (U29)

0x4000 - 0xFFFF : ROM
0x600000 - 0x6FFFFF : ROM (U18) w/o init, this is the top address space
0xA00000 - 0xAFFFFF : ROM (U15)
0xC00000 - 0xCFFFFF : ROM (U14)

U18 Address Mapping
0x004000 - 0xFFFFFF


Access Addresses from WPC-89
0x3FDC - a write to this port triggers the WPCS6809's IRQ
0x3FDC - a read from this port gets the last value written by 0x3C00
0x3FDD - this can be read at any time. When read, it activates CD0 (B0 of return data) 
          Any value with B0 means that there is data to be read from 0x3FDC

*/


byte WPCSRAM[WPCS_RAM_SIZE];
//byte *WPCSROM;
uint32_t WPCSROMSize;
byte WPCSRomBank;
uint32_t WPCSTicksIrq = 0;
uint8_t WPCSBusDataOutput = 0x00;
uint8_t WPCSBusDataInput = 0x00;
bool WPCSBusDataReadyFlag = false;
bool WPCSBoardLoggingOn = false;
bool WPCSBusDataInputNew = false;
bool WPCSCommandResponseNew = false;
uint8_t ad7524Register = 0x00;
int16_t ad7524OutSample = 0;


uint32_t WPCSUpperAddressMask = 0x00000000;


static uint8_t *s_WpcsRomBanks[3] = {NULL, NULL, NULL};
static uint32_t s_WpcsRomSizes[3] = {0, 0, 0};

static uint8_t s_DummyRomPage[16384]; // Fallback for unmapped reads to prevent NULL dereferencing
static uint8_t *s_MemoryPages[4];


// Global LUT arrays for the dynamic pages
static uint8_t *WpcsRomPage1LUT[256];
static uint8_t *WpcsRomPage2LUT[256];


// Default jumper configuration (W1, W2, W4, W6 = out; W3, W5, W7, W8, W9, W10 = in)
bool g_W2 = false;
bool g_W3 = true;
bool g_W4 = false;
bool g_W5 = true;
bool g_W6 = false;
bool g_W7 = true;

// Static pointer for the fixed upper page
static uint8_t *WpcsRomPage3Fixed;


static uint8_t* WPCSResolveRomPointer(uint16_t logicalAddr) {
  uint8_t a14 = (logicalAddr >> 14) & 1;
  uint8_t a15 = (logicalAddr >> 15) & 1;
  
  uint8_t a16, a17, a18, a19, a20, a21, a22, a23;
  
  // Evaluate 74LS374 (U28) Output State
  if (a14 && a15) { 
      // 0xC000 - 0xFFFF: !OC is HIGH. Latch outputs are High-Z.
      // Resistor network pulls lines high/low
      a16 = 1; a17 = 1; a18 = 1; 
      a19 = 1; // Assuming default high pull for top bank routing
      a20 = 1; a21 = 1; a22 = 1; 
      a23 = 0; 
  } else { 
      // 0x4000 - 0xBFFF: !OC is LOW. Latch outputs follow WPCSRomBank.
      a16 = (WPCSRomBank >> 0) & 1;
      a17 = (WPCSRomBank >> 1) & 1;
      a18 = (WPCSRomBank >> 2) & 1;
      a19 = (WPCSRomBank >> 3) & 1;
      a20 = (WPCSRomBank >> 4) & 1;
      a21 = (WPCSRomBank >> 5) & 1;
      a22 = (WPCSRomBank >> 6) & 1;
      a23 = (WPCSRomBank >> 7) & 1;
  }
  
  // Evaluate Jumper Matrix
  uint8_t a15_prime = g_W5 ? a15 : (g_W2 ? !(a14 | a15) : a15); 
  uint8_t a19_prime = g_W7 ? a19 : (g_W6 ? a15 : 1);
  
  // Assemble Physical Address targeting XMEGROM pins (CA14 - CA20)
  // CA0-CA13 are provided dynamically during the read by (offset & 0x3FFF)
  uint32_t physicalAddr = 0;
  physicalAddr |= (a15_prime << 14);
  physicalAddr |= (a16 << 15);
  physicalAddr |= (a17 << 16);
  physicalAddr |= ((g_W3 ? a18 : (g_W4 ? 1 : a18)) << 17);
  physicalAddr |= (a19_prime << 18);
  physicalAddr |= (a20 << 19);
  
  // Evaluate /CE lines and route to the correct chip buffer
  if (a23 == 0 && s_WpcsRomBanks[WPCS_ROM_U18] != NULL) {
      return s_WpcsRomBanks[WPCS_ROM_U18] + (physicalAddr & (s_WpcsRomSizes[WPCS_ROM_U18] - 1));
  } else if (a21 == 0 && s_WpcsRomBanks[WPCS_ROM_U14] != NULL) {
      return s_WpcsRomBanks[WPCS_ROM_U14] + (physicalAddr & (s_WpcsRomSizes[WPCS_ROM_U14] - 1));
  } else if (a22 == 0 && s_WpcsRomBanks[WPCS_ROM_U15] != NULL) {
      return s_WpcsRomBanks[WPCS_ROM_U15] + (physicalAddr & (s_WpcsRomSizes[WPCS_ROM_U15] - 1));
  }
  
  // Fallback if the CPU reads unpopulated address space
  return s_DummyRomPage;
}



// Pre-calculates all possible memory bank mappings
void WPCSBuildRomLUT(void) {
    uint8_t originalBank = WPCSRomBank;
    
    // Page 3 is fixed and independent of WPCSRomBank
    WpcsRomPage3Fixed = WPCSResolveRomPointer(0xC000);
    
    // Calculate Page 1 and Page 2 for all 256 possible bank values
    for (int i = 0; i < 256; i++) {
        WPCSRomBank = i;
        WpcsRomPage1LUT[i] = WPCSResolveRomPointer(0x4000);
        WpcsRomPage2LUT[i] = WPCSResolveRomPointer(0x8000);
    }
    
    // Restore the active bank
    WPCSRomBank = originalBank;
}


void WPCSUpdateMemoryMap(void) {
  s_MemoryPages[0] = WPCSRAM;                      // 0x0000 - 0x3FFF
  s_MemoryPages[1] = WpcsRomPage1LUT[WPCSRomBank]; // 0x4000 - 0x7FFF
  s_MemoryPages[2] = WpcsRomPage2LUT[WPCSRomBank]; // 0x8000 - 0xBFFF
  s_MemoryPages[3] = WpcsRomPage3Fixed;            // 0xC000 - 0xFFFF
}


void WPCSBoardSetROMAddress(uint8_t chipId, uint8_t *romLocation, uint32_t romSize) {
  if (chipId <= WPCS_ROM_U15) {
      s_WpcsRomBanks[chipId] = romLocation;
      s_WpcsRomSizes[chipId] = romSize;
  }
  WPCSBuildRomLUT();
  WPCSUpdateMemoryMap();
}


bool WPCSBoardInit() {

  // clear RAM and Display's shadow RAM
  uint32_t *p = (uint32_t *)WPCSRAM;
  uint32_t words = WPCS_RAM_SIZE / 4;  
  while(words--) *p++ = 0;

  WPCSTicksIrq = 0;
  WPCSUpperAddressMask = 0x00000000;
  WPCSRomBank = 0;

  WPCSCPUReset();
  return true;
}

void WPCSBoardRelease() {
}

void WPCSBoardReset() {
  WPCSTicksIrq = 0;
  WPCSCPUReset();
}


// This version only handles the case of U18
// Although in retrospect, it wasn't that slow
/*
byte slowWPCSReadFromROM(uint16_t offset) {
  if (!(offset&0xC000) || WPCSROM==NULL) return 0xFF;

  uint32_t transformedOffset;
  transformedOffset = offset & 0x3FFF;

  if ((offset&0xC000)!=0xC000) {
    // the CPU is trying to read from ROM that's
    // switched by the WPCSRomBank
    transformedOffset |= (offset&0x8000) ? 0x4000 : 0; // put line 15 into 14
    transformedOffset |= ((WPCSRomBank & 0x1F)<<15);
  } else {
    // This is not a banked address, so we can assume the 
    // upper lines are fixed
    transformedOffset |= (offset&0x8000) ? 0x4000 : 0; // put line 15 into 14
    transformedOffset |= (0x7F0000>>1);
  }

  // The transformed offset now includes info about which 
  // ROM is trying to be addressed. If A23 is off
  // we're trying to address U18
  // The lower 20 bits are the address within the chip
  if (transformedOffset & 0x800000) return 0xFF; // this is for U14 or U15
  transformedOffset &= 0x07FFFF;
  return WPCSROM[transformedOffset];
}
*/  

byte WPCSReadFromROM(uint16_t offset) {
  if (offset < 0x4000) return 0xFF;

  // Shift by 14 divides by 16384. 
  // 0x4000 -> index 0 | 0x8000 -> index 1 | 0xC000 -> index 2
  return s_MemoryPages[(offset >> 14) - 1][offset & 0x3FFF];
}




void WPCSBoardStart() {
  WPCSBoardReset();
}


uint16_t WPCSBoardStep(uint32_t curTicks) {
  uint16_t wpcsCPUTicks = WPCSCPUStep();
  return wpcsCPUTicks;
}


void WPCSBoardWriteMemory(unsigned int offset, byte value) {
}


void YM2151Write(byte port, byte value) {
  // Post the data to the YM2151 running in the other process
  uint32_t cmd = (port << 8) | value;
  gpio_put(25, 0);
  multicore_fifo_push_blocking(cmd);
}

byte YM2151Read(byte port) {
  // The YM2151 ignores the port on read and always returns the status register.
  // We read the shared state updated by Core 1.
  return g_YmStatus;
}

void AD7524Write(byte value) {
  ad7524Register = value;
  // The AD7524 is an 8-bit DAC: 0x00 = min, 0xFF = max
  // Center it around zero for mixing (0x80 = silence)
  ad7524OutSample = ((int16_t)value - 0x80) << 7;  // scales to roughly ±16384
  g_CurrentDACSample = ad7524OutSample;
}

byte AD7524Read() {
  return 0xFF;
}



// CVSD State Variables
#define CVSD_BIT_BUFFER_SIZE 4096 // Power of 2 ensures fast bitwise wrapping

static volatile uint8_t CVSDBitBuffer[CVSD_BIT_BUFFER_SIZE];
static volatile uint32_t CVSDBitHead = 0;
static volatile uint32_t CVSDBitTail = 0;

static uint8_t CVSDShiftReg = 0;
static uint8_t CVSDLatchedBit = 0;
static int CVSDSylReg = 0x3F; // HC55536 initialization value
static int CVSDIntegrator = 0;
static int16_t CVSDOutSample = 0;

// Constants for HC55536 from the decap analysis
static const int CHARGE_MASK = 0xFC0;
static const int CHARGE_SHIFT = 6;
static const int CHARGE_ADD = 0xFC1;
static const int DECAY_SHIFT = 4;

// Helper to saturate the 10-bit integrator register
static inline int Clip10Bits(int v) {
    return v < -512 ? -512 : v > 511 ? 511 : v;
}

// Helper to sign-extend the 10-bit value for C arithmetic
static inline int SignExt10Bits(int v) {
    return (v & 0x200) != 0 ? v | ~0x3FF : v;
}

void CVSDSetupBit(uint8_t value) {
    CVSDLatchedBit = value & 0x01;
}

void CVSDClockPulse(uint8_t value) {
    // Enqueue the bit during the CPU burst
    uint32_t nextHead = (CVSDBitHead + 1) & (CVSD_BIT_BUFFER_SIZE - 1);
    if (nextHead != CVSDBitTail) {
        CVSDBitBuffer[CVSDBitHead] = CVSDLatchedBit;
        CVSDBitHead = nextHead;
    }
    (void)value;
}

void CVSDProcessTick(void) {
    // Run steady analog decay regardless of clock pulses
    int sum = SignExt10Bits(((~CVSDIntegrator >> DECAY_SHIFT) + 1) & 0x3FF);
    CVSDIntegrator = Clip10Bits(CVSDIntegrator + sum);

    // If a bit is ready, process the digital state machine
    if (CVSDBitHead != CVSDBitTail) {
        uint8_t bit = CVSDBitBuffer[CVSDBitTail];
        CVSDBitTail = (CVSDBitTail + 1) & (CVSD_BIT_BUFFER_SIZE - 1);

        CVSDShiftReg = ((CVSDShiftReg << 1) | bit) & 0x07;

        // Syllabic filter charge update
        CVSDSylReg += (~CVSDSylReg & CHARGE_MASK) >> CHARGE_SHIFT;

        if (CVSDShiftReg != 0 && CVSDShiftReg != 0x07) {
            CVSDSylReg += CHARGE_ADD;
        }
        CVSDSylReg &= 0xFFF;

        // Charge integrator
        sum = CVSDSylReg >> 6;
        if (sum < 2) sum = 2;
        if ((CVSDShiftReg & 1) != 0) sum = -sum;
        CVSDIntegrator = Clip10Bits(CVSDIntegrator + sum);
    } else {
        // Dead zone reached. Blank head and tail to prevent drift/wrap issues.
        CVSDBitHead = 0;
        CVSDBitTail = 0;
    }

    // Scale from 10-bit hardware register to standard 16-bit PCM output
    CVSDOutSample = (int16_t)((CVSDIntegrator << 6) | (((CVSDIntegrator & 0x3FF) ^ 0x200) >> 4));
    g_CurrentCVSDSample = CVSDOutSample;
}




byte ReadEPOT() {
  // Can't read the volume pot
  return 0xFF;
}

/*
void WriteEPOT(byte value) {
  // Track the 32-step wiper position locally
  // Start at 0
  static uint8_t s_epotVolume = 3; 
  
  // /INC idles high
  static uint8_t s_lastD0 = 1;      

  uint8_t d0 = value & 0x01; // /INC (Increment)
  uint8_t d1 = value & 0x02; // U/D (1 = Up, 0 = Down)

  // The digital pot moves the wiper on the falling edge of D0
  if (s_lastD0 == 1 && d0 == 0) {
      if (d1) {
          if (s_epotVolume < 31) s_epotVolume++;
      } else {
          if (s_epotVolume > 0) s_epotVolume--;
      }

      // Pipe the new volume across the FIFO to Core 1 using custom port 2
      uint32_t cmd = (2 << 8) | s_epotVolume;
      multicore_fifo_push_blocking(cmd);
  }

  s_lastD0 = d0;
}
*/


void WriteEPOT(byte value) {
  // Track the internal 7-bit counter (0-127)
  static uint8_t s_epotCounter = 0; 

  // /INC idles high
  static uint8_t s_lastD0 = 1;      

  uint8_t d0 = value & 0x01; // /INC (Increment)
  uint8_t d1 = value & 0x02; // U/D (0 = Up, non-zero = Down)

  // The digital pot moves the wiper on the falling edge of D0
  if (s_lastD0 == 1 && d0 == 0) {
      if (!d1) {
          if (s_epotCounter < 127) s_epotCounter++;
      } else {
          if (s_epotCounter > 0) s_epotCounter--;
      }
      g_mainVolume = s_epotCounter;
    }

  s_lastD0 = d0;
}



void WPCSBoardWriteOutputData(byte value) {
  WPCSBusDataReadyFlag = true;
  WPCSCommandResponseNew = true;
  WPCSBusDataOutput = value;
}

byte WPCSBoardReadInputData() {
  return WPCSBusDataInput;
}


byte WPCSBoardRead8(unsigned short offset) {  
  // Fast path for RAM (Zero-page, Stack) - No pointer indirection
  if (offset < 0x2000) {
      return WPCSRAM[offset];
  }

  // Fast path for ROM (Opcode fetches, Vector reads)
  if (offset >= 0x4000) {
      return s_MemoryPages[offset >> 14][offset & 0x3FFF];
  }

  // I/O
  byte subSystem = (offset & 0x1C00) >> 10;
  if (subSystem == 1) return YM2151Read(offset & 0x01);
  else if (subSystem == 2) return AD7524Read();
  else if (subSystem == 3) CVSDClockPulse(0xFF);
  else if (subSystem == 4) return WPCSBoardReadInputData();
  else if (subSystem==5) CVSDSetupBit(0xFF);
  else if (subSystem == 6) return ReadEPOT();

  return 0xFF;
}

#include "hardware/pio.h"

void WPCSBoardWrite8(unsigned short offset, byte value) {

  if (offset<0x2000) {
    WPCSRAM[offset] = value;
  } else if (offset<0x4000) {
    byte subSystem = (offset&0x1C00)>>10;
    if (subSystem==0) {
      WPCSRomBank = value;
      WPCSUpdateMemoryMap();
    }
    else if (subSystem==1) YM2151Write(offset&0x01, value);
    else if (subSystem==2) AD7524Write(value);
    else if (subSystem==3) CVSDClockPulse(value);
    // 4 is read-only
    else if (subSystem==5) CVSDSetupBit(value);
    else if (subSystem==6) WriteEPOT(value);
    else if (subSystem==7) WPCSBoardWriteOutputData(value);
  }
}

uint8_t WPCSReadControl() {
  return WPCSBusDataReadyFlag ? 0x01 : 0x00;
}

uint8_t WPCSReadData() {
  // Reading this register clears the data ready flag
  WPCSBusDataReadyFlag = false;

  return WPCSBusDataOutput;
}

uint8_t WPCSGetLastCommand() {
  WPCSBusDataInputNew = false;
  return WPCSBusDataInput;
}

bool WPCSNewCommandToShowApp() {
  return WPCSBusDataInputNew;
}

uint8_t WPCSGetLastResponse() {
  WPCSCommandResponseNew = false;
  return WPCSBusDataOutput;
}

bool WPCSNewResponseToShowApp() {
  return WPCSCommandResponseNew;
}


void WPCSWriteData(uint8_t value) {
  WPCSBusDataInput = value;
  WPCSBusDataInputNew = true;
  WPCSCPUIRQ();
}


void WPCSBoardFIRQ(bool firqOn) {
  WPCSCPUFIRQ(firqOn);
}

void WPCSBoardEnableLogging(bool loggingOn) {
  WPCSBoardLoggingOn = loggingOn;
}



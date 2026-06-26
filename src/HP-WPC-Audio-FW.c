#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "ws2812.pio.h" 
#include "hw_config.h" 
#include "sd_card.h"
#include "ff.h"
#include "AudioChain.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "pico/multicore.h"
#include "SharedState.h"
#include "Core1Audio.h"
#include "WPCSBoard.h"
#include "hardware/structs/systick.h"
#include "SDCardFunctions.h"
#include "DataIO.h"

#define PIN_TX 14
#define PIN_CD 6 // Card Detect switch
#define MAX_BRIGHTNESS 100 
#define PULSE_INTERVAL_US 15000 // 15ms in microseconds

#define ENABLE_OVERCLOCKING
#define ENABLE_CORE_1
#define ENABLE_SD_CHECK

extern const uint32_t rom_data14_size;
extern const uint8_t rom_data14[];
extern const uint32_t rom_data15_size;
extern const uint8_t rom_data15[];
extern const uint32_t rom_data18_size;
extern const uint8_t rom_data18[];

static inline void PutPixel(uint32_t pixelGRB) {
    pio_sm_put_blocking(pio0, 0, pixelGRB << 8u);
}

static inline uint32_t UrgbU32(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t) (r) << 8) | ((uint32_t) (g) << 16) | (uint32_t) (b);
}



void EmulateYMWrite(uint8_t port, uint8_t data) {
    uint32_t cmd = (port << 8) | data;
    multicore_fifo_push_blocking(cmd);
}

// Helper to push a register write command across the FIFO
static void WriteYMRegister(uint8_t regAddr, uint8_t data) {
    // Port 0: Set latched address
    uint32_t cmdAddr = (0 << 8) | regAddr;
    multicore_fifo_push_blocking(cmdAddr);

    // Port 1: Write data
    uint32_t cmdData = (1 << 8) | data;
    multicore_fifo_push_blocking(cmdData);
}


void InitYMTimerA(uint32_t targetFrequencyHz) {
    if (targetFrequencyHz == 0) return;

    uint32_t clockRate = 3579545;
    uint32_t divider = 64 * targetFrequencyHz;
    
    // Add half the divider to force proper rounding
    int32_t ticks = (clockRate + (divider / 2)) / divider;
    int32_t timerA = 1024 - ticks;

    if (timerA < 0) timerA = 0;
    if (timerA > 1023) timerA = 1023;

    uint8_t msb = (timerA >> 2) & 0xFF;
    uint8_t lsb = timerA & 0x03;

    WriteYMRegister(0x10, msb);
    WriteYMRegister(0x11, lsb);

    // Stop Timer A to ensure the next write forces a reload
    WriteYMRegister(0x14, 0x04); 
    
    // Enable Timer A IRQ and Load/Start Timer A
    WriteYMRegister(0x14, 0x05);
}



// Clear the IRQ flag without stopping Timer A
void ClearYMIRQ() {
    WriteYMRegister(0x14, 0x15);
}


void RunNonBlockingBoardTest(void) {

    static uint32_t s_lastTime = 0;
    static bool s_pitchState = false;
    static uint32_t btState = 0; 
    static uint32_t testStartTime = 0;

    uint32_t currentTime = time_us_32() - testStartTime; // 1 megahertz ticks

    if (currentTime==s_lastTime) return;
    s_lastTime = currentTime;

    switch (btState) {
        case 750000: WPCSWriteData(0); break;
        case 751000: WPCSWriteData(121); break;
        case 752000: WPCSWriteData(20); break;
        case 753000: WPCSWriteData(235); break;
        case 800000: WPCSWriteData(5); break;
        case 2000000: WPCSWriteData(122); break;
        case 2001000: WPCSWriteData(55); break;
        case 15000000: WPCSWriteData(122); break;
        case 15001000: WPCSWriteData(23); break;
        case 20000000: WPCSWriteData(122); break;
        case 20001000: WPCSWriteData(23); break;
        case 25000000: WPCSWriteData(122); break;
        case 25001000: WPCSWriteData(24); break;
        case 30000000: WPCSWriteData(122); break;
        case 30001000: WPCSWriteData(25); break;            
            
    }
    btState+=2;

}


// Call this repeatedly inside your Controller non-blocking main loop
void RunNonBlockingYmTest(void) {
    static uint32_t s_lastTime = 0;
    static bool s_pitchState = false;
    static uint8_t s_state = 0; 

    uint32_t currentTime = time_us_32() / 1000;

    // States 0-19: Drip-feed initialization 
    if (s_state < 20) {
        if (currentTime - s_lastTime >= 1) {
            s_lastTime = currentTime;
            
            // Map the state directly to the port and data
            switch (s_state) {
                case 0:     
                    // YM timer A test
                    InitYMTimerA(40000);
                    break; 
            }
            s_state++;
        }
        return; 
    }

}


void InitProfilePins(void) {
    // Initialize the first profiling pin
    gpio_init(24);
    gpio_set_dir(24, GPIO_OUT);

    // Initialize the second profiling pin
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
}


void EnableOverclocking() {
    #ifdef ENABLE_OVERCLOCKING    
    // Bump the core voltage slightly (default is 1.10V)
    vreg_set_voltage(VREG_VOLTAGE_1_30); 

    // Allow 1ms for the physical voltage plane to stabilize before overclocking
    sleep_ms(1);

    // Push the clock to 250MHz
    set_sys_clock_khz(300000, true);
#endif    
}



void InitializeIO() {
    stdio_init_all();
    // Initialize Card Detect Pin as input with pull-up
    gpio_init(PIN_CD);
    gpio_set_dir(PIN_CD, GPIO_IN);
    gpio_pull_up(PIN_CD);
    InitProfilePins();
    sd_init_driver();
    DataIOInit();

    // Force the I2S_SCK pin to ground to stabilize the PCM5100 PLL
//    gpio_init(15);
//    gpio_set_dir(15, GPIO_OUT);
//    gpio_put(15, 0);
}

void InitializeLEDs() {
    static uint s_ws2812Offset = 0;
    static bool s_ledsLoaded = false;
    
    PIO pio = pio0;
    int sm = 0;

    // Only load the program into PIO memory on the very first call
    if (!s_ledsLoaded) {
        s_ws2812Offset = pio_add_program(pio, &ws2812_program);
        s_ledsLoaded = true;
    }

    // Ws2812ProgramInit reads the current system clock and sets the correct divider.
    // It is safe to call this multiple times to "re-clock" the LEDs.
    Ws2812ProgramInit(pio, sm, s_ws2812Offset, PIN_TX, 800000, false);

    PutPixel(UrgbU32(0, 0, 0));
    PutPixel(UrgbU32(0, 0, 0));
    PutPixel(UrgbU32(0, 0, 0));
    PutPixel(UrgbU32(0, 0, 0));
}

void InitializeAudioRing() {
    InitAudioI2S();
    SetAudioMute(true);
}

void InitializeCore1() {
#ifdef ENABLE_CORE_1   
    // Launch the YM2151 Sandbox
    // Clean up any state left from previous flash/run
    multicore_reset_core1();
    multicore_fifo_drain();
    
    // Now it's safe to launch
    multicore_launch_core1(Core1Main);

    // Initialize Audio
    while (GetBufferDepth() < 256) {
        // Wait for Core 1 audio to get a head start
        tight_loop_contents();
    }
#endif    

    // Some default volumes
    g_DACGainL = 150;
    g_DACGainR = 150;
    g_CVSDGainL = 200;
    g_CVSDGainR = 200;
    g_YMGainL = 150;
    g_YMGainR = 150;
}



uint8_t LoadROMs() {

    // First, check to see if we have ROMs available in flash
    uint32_t romSize = GetROMSizeFromFlash(WPCS_ROM_U18);
    if (romSize==0) return 0;
    WPCSBoardSetROMAddress(WPCS_ROM_U18, (uint8_t *)GetROMAddressFromFlash(WPCS_ROM_U18), romSize);

    romSize = GetROMSizeFromFlash(WPCS_ROM_U14);
    if (romSize) WPCSBoardSetROMAddress(WPCS_ROM_U14, (uint8_t *)GetROMAddressFromFlash(WPCS_ROM_U14), romSize);

    romSize = GetROMSizeFromFlash(WPCS_ROM_U15);
    if (romSize) WPCSBoardSetROMAddress(WPCS_ROM_U15, (uint8_t *)GetROMAddressFromFlash(WPCS_ROM_U15), romSize);

    return 1;
}


void RunMenu() {
    // This menu is for loading ROMs
    // and testing funtionality of the board
    uint32_t lastPulseTime = time_us_32();
    while (1) {
        uint32_t currentTime = time_us_32();
        if ((currentTime-lastPulseTime)>50000) {
            lastPulseTime = currentTime;

            if ((currentTime/100000) % 2) {
                // LED 0: Standard Heartbeat Pulse
                PutPixel(UrgbU32(255, 0, 0));
                PutPixel(UrgbU32(255, 0, 0));
                PutPixel(UrgbU32(255, 0, 0));
                PutPixel(UrgbU32(255, 0, 0));
            } else {
                PutPixel(UrgbU32(0, 0, 0));
                PutPixel(UrgbU32(0, 0, 0));
                PutPixel(UrgbU32(0, 0, 0));
                PutPixel(UrgbU32(0, 0, 0));
            }
        }

    }

}


void FlashWriteProgress(uint8_t progress) {
    static uint8_t lastProgress = 0;

    if (progress>lastProgress) {
        lastProgress = progress;
        uint8_t fractionalProgress = (progress%25)*10;

        if (progress<25) {
            PutPixel(UrgbU32(0, fractionalProgress, 0));
            PutPixel(UrgbU32(0, 0, 0));
            PutPixel(UrgbU32(0, 0, 0));
            PutPixel(UrgbU32(0, 0, 0));
        } else if (progress<50) {
            PutPixel(UrgbU32(0, 255, 0));
            PutPixel(UrgbU32(0, fractionalProgress, 0));
            PutPixel(UrgbU32(0, 0, 0));
            PutPixel(UrgbU32(0, 0, 0));
        } else if (progress<75) {
            PutPixel(UrgbU32(0, 255, 0));
            PutPixel(UrgbU32(0, 255, 0));
            PutPixel(UrgbU32(0, fractionalProgress, 0));
            PutPixel(UrgbU32(0, 0, 0));
        } else {
            PutPixel(UrgbU32(0, 255, 0));
            PutPixel(UrgbU32(0, 255, 0));
            PutPixel(UrgbU32(0, 255, 0));
            PutPixel(UrgbU32(0, fractionalProgress, 0));
        }

    }

}


int main() {

    // Set up variables, pins, and core 1 process
    InitializeIO();

    uint16_t ROMsInRoot = 0;
    uint16_t ROMsInFolder = 0;
    uint16_t WAVsInRoot = 0;
    uint16_t WAVsInFolder = 0;
#ifdef ENABLE_SD_CHECK
    if (InitSDFilesystem()) {
        if (CheckSDCardContents(&ROMsInRoot, &ROMsInFolder, &WAVsInRoot, &WAVsInFolder)) {
            if (ROMsInRoot==1) {            
                // Get the name of the ROM in root
                char newROMName[64];
                
                if (GetROMFilenameFromSDCard("0:/", 0, newROMName, 64)) {
                    // There's a ROM in the root of the card.
                    // if it doesn't match what's in flash, we should store it
                    if (!DoesFilenameMatchFlash(newROMName)) {
                        // set up LEDs at this clock speed
                        InitializeLEDs();
                        PutPixel(UrgbU32(0, 255, 0));
                        PutPixel(UrgbU32(0, 255, 0));
                        PutPixel(UrgbU32(0, 255, 0));
                        PutPixel(UrgbU32(0, 255, 0));
                        StoreROMData("0:/", "0:/HPMenuCallouts/", newROMName, FlashWriteProgress);
                    }
                }
                    
            }
        }
    }
#endif    
    
    if (!LoadROMs()) {
        RunMenu();
    }

    EnableOverclocking();

    InitializeLEDs(); // set up LEDs at overclock speed
    InitializeAudioRing();
    InitializeCore1();

 //   WPCSBoardSetROMAddress(WPCS_ROM_U18, (uint8_t *)rom_data18, rom_data18_size);
    //if (rom_data14_size) WPCSBoardSetROMAddress(WPCS_ROM_U14, (uint8_t *)rom_data14, rom_data14_size);
    //if (rom_data15_size) WPCSBoardSetROMAddress(WPCS_ROM_U15, (uint8_t *)rom_data15, rom_data15_size);        

    WPCSBoardInit();

    // Timer and animation state
    uint32_t lastPulseTime = time_us_32();
    int brightness = 0;
    int fadeAmount = 2;

    uint64_t startUs = time_us_64();
    uint64_t totalEmulatedTicks = 0;

    systick_hw->csr = 0x5; 
    systick_hw->rvr = 0x00FFFFFF;

    uint8_t lastYMStatus = 0;
    uint64_t totalCvsdTicks = 0;

    PutPixel(UrgbU32(0, 255, 0));
    PutPixel(UrgbU32(0, 0, 0));
    PutPixel(UrgbU32(0, 0, 0));
    PutPixel(UrgbU32(0, 0, 0));
    SetAudioMute(false);

    // Main non-blocking loop
    while (true) {
//        RunNonBlockingBoardTest();
//        RunNonBlockingYmTest();
        ProcessAudioI2S();

        uint32_t currentTime = time_us_32();
        if (0 && (currentTime-lastPulseTime)>PULSE_INTERVAL_US) {
            lastPulseTime = currentTime;
            brightness += fadeAmount;
            if (brightness <= 0 || brightness >= MAX_BRIGHTNESS) {
                fadeAmount = -fadeAmount; 
            }

            // LED 0: Standard Heartbeat Pulse
            PutPixel(UrgbU32(0, brightness, 0));
            PutPixel(UrgbU32(0, 0, 0));
            PutPixel(UrgbU32(0, 0, 0));
            PutPixel(UrgbU32(0, 0, 0));
            g_core1Alive = false;
        }

        // Calculate how many CPU ticks *should* have passed by now
        uint64_t currentUs = time_us_64() - startUs;
        uint64_t targetTicks = currentUs * 2; // 2MHz = 2 ticks per microsecond

        // See if it's time for the next Board tick
        if (totalEmulatedTicks < targetTicks) {

            // Non-fancy, non-profiling
            totalEmulatedTicks += WPCSBoardStep(totalEmulatedTicks);

            // Evaluate YM2151 FIRQ status. 
            // Bit 0 = Timer A, Bit 1 = Timer B. If either is set, pull the IRQ line.
            //WPCSBoardFIRQ(g_YmStatus&0x03);
        } 

        // 1. MPU -> RP2350A
        if (DataIOHasNewData()) {
            // Drives DATA_IN_OC low, reads the bus, and clears the polling flag
            uint8_t incomingData = DataIOReadData();
            
            // Pushes data to WPCSBusDataInput, flags WPCSBusDataInputNew, and triggers WPCSCPUIRQ()
            WPCSWriteData(incomingData);
        }
        
        // 2. RP2350A -> MPU
        if (WPCSNewResponseToShowApp()) {
            // Retrieves WPCSBusDataOutput and clears the WPCSCommandResponseNew flag
            uint8_t outgoingData = WPCSGetLastResponse();
            
            // Sets bus to output, applies data, triggers DATA_OUT_LATCH via PIO, and reverts bus
            DataIOWriteData(outgoingData);
        }
      
    }

}
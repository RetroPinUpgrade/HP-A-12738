#ifndef SHARED_STATE_H
#define SHARED_STATE_H

#include <stdint.h>
#include <stdbool.h>

// Represents the physical voltage level of the YM2151 /IRQ pin
extern volatile bool g_ymIrqLine;
extern volatile bool g_core1Ready;
extern volatile bool g_core1Alive;
extern volatile bool g_ymBusy;
extern volatile bool g_ymKeyOn;
extern volatile uint8_t g_YmStatus;

extern volatile int16_t g_CurrentDACSample;
extern volatile int16_t g_CurrentCVSDSample;
extern volatile int16_t g_DACGainL;
extern volatile int16_t g_DACGainR;
extern volatile int16_t g_CVSDGainL;
extern volatile int16_t g_CVSDGainR;
extern volatile int16_t g_YMGainL;
extern volatile int16_t g_YMGainR;
extern volatile int32_t g_mainVolume;

#endif
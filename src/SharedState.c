#include "stdint.h"
#include "SharedState.h"

volatile bool g_ymIrqLine = false;
volatile bool g_core1Ready = false;
volatile bool g_core1Alive = false;
volatile bool g_ymBusy = false;
volatile bool g_ymKeyOn = false;
volatile uint8_t g_YmStatus = 0;

// Audio samples (created by core 0 and mixed by core 1)
volatile int16_t g_CurrentDACSample = 0;
volatile int16_t g_CurrentCVSDSample = 0;
volatile int16_t g_DACGainL = 256;
volatile int16_t g_DACGainR = 256;
volatile int16_t g_CVSDGainL = 256;
volatile int16_t g_CVSDGainR = 256;
volatile int16_t g_YMGainL = 256;
volatile int16_t g_YMGainR = 256;
volatile int32_t g_mainVolume = 20;
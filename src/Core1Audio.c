#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "ym2151.h" 
#include "SharedState.h"
#include "WPCSBoard.h"

#define AUDIO_BUFFER_SIZE 2048

// Default to 4 instead of 31 so it does not deafen you on boot
//static uint8_t s_masterVolume = 3;

typedef struct {
    volatile int16_t buffer[AUDIO_BUFFER_SIZE * 2]; 
    volatile uint32_t head;
    volatile uint32_t tail;
} AudioRingBuffer;

static AudioRingBuffer s_ymAudioBuffer = {0};


static bool AudioRingBufferPush(int16_t left, int16_t right) {
    uint32_t nextHead = (s_ymAudioBuffer.head + 2) % (AUDIO_BUFFER_SIZE * 2);
    
    // If the buffer is full, drop the sample to prevent locking the core
    if (nextHead == s_ymAudioBuffer.tail) {
        return false; 
    }
    
    s_ymAudioBuffer.buffer[s_ymAudioBuffer.head] = left;
    s_ymAudioBuffer.buffer[s_ymAudioBuffer.head + 1] = right;
    s_ymAudioBuffer.head = nextHead;
    
    return true;
}



// Fast integer-based soft clipper
static inline __attribute__((always_inline)) int32_t ApplySoftKnee(int32_t sample) {
    // Threshold where the soft knee begins (roughly 75% of full scale)
    const int32_t threshold = 24576; 
    const int32_t limit = 32767;
    
    int32_t absSample = sample < 0 ? -sample : sample;
    
    // Pass the audio through transparently if it is under the threshold
    if (absSample <= threshold) {
        return sample;
    }
    
    int32_t over = absSample - threshold;
    
    // If the sample pushes past the end of our parabolic curve, hard clamp it
    if (over >= 16384) {
        absSample = limit; 
    } else {
        // Fast quadratic soft compression: over - (over^2 / 32768)
        // Bit shifting by 15 divides by 32768 in a single clock cycle
        int32_t soft = over - ((over * over) >> 15);
        absSample = threshold + soft;
    }
    
    // Restore the original sign
    return sample < 0 ? -absSample : absSample;
}


// Static state for the CVSD analog filter chain
static int32_t s_cvsdDcOffset = 0;
static int32_t s_cvsdLp1 = 0;
static int32_t s_cvsdLp2 = 0;
static int32_t s_cvsdLp3 = 0;
static int32_t s_cvsdLp4 = 0;

// High-pass alpha: 32700 (~14Hz cutoff at 44.1kHz to match the 1uF C37 cap)
static const int32_t DC_ALPHA = 32700; 

/* Values for LP_ALPHA for different cutoff frequencies
    1000 Hz	        4351
    2000 Hz	        8124
    3000 Hz	        11397
    3600 Hz	        13148
    4000 Hz	        14234
    5000 Hz	        16696
    6000 Hz	        18828
    8000 Hz	        22285
    10000 Hz	    24888
*/
static const int32_t LP_ALPHA = 29000;

// Consolidated function to handle the entire A-12738 output stage
static inline int32_t oldApplyCVSDAnalogFilters(int32_t sample) {
    // 1. High-Pass (DC Blocker) corresponding to the 1uF Tantalum cap
    int32_t hp = sample - s_cvsdDcOffset;
    s_cvsdDcOffset = (sample * (32768 - DC_ALPHA) + (s_cvsdDcOffset * DC_ALPHA)) >> 15;

    // 2. 4-Pole Low-Pass Filter simulating U7A and U7B cascaded MFB stages
    // Cascading four 1-pole EMA filters creates a steep 4th-order roll-off
    s_cvsdLp1 = s_cvsdLp1 + (((hp - s_cvsdLp1) * LP_ALPHA) >> 15);
    s_cvsdLp2 = s_cvsdLp2 + (((s_cvsdLp1 - s_cvsdLp2) * LP_ALPHA) >> 15);
    s_cvsdLp3 = s_cvsdLp3 + (((s_cvsdLp2 - s_cvsdLp3) * LP_ALPHA) >> 15);
    s_cvsdLp4 = s_cvsdLp4 + (((s_cvsdLp3 - s_cvsdLp4) * LP_ALPHA) >> 15);

    return s_cvsdLp4;
}


// Aggressiveness controls how many LP poles are applied and scales LP_ALPHA:
//   0 = no filtering (bypass)
//   4 = 1-pole, gentle (matches original level 1)
//   8 = 2-pole, moderate (matches original level 2)
//  12 = 3-pole, strong (matches original level 3)
//  15 = 4-pole, most aggressive (matches original level 4)
static inline int32_t ApplyCVSDAnalogFilters(int32_t sample, int aggressiveness) {
    // 1. High-Pass (DC Blocker) — always active regardless of aggressiveness
    int32_t hp = sample - s_cvsdDcOffset;
    s_cvsdDcOffset = (sample * (32768 - DC_ALPHA) + (s_cvsdDcOffset * DC_ALPHA)) >> 15;

    if (aggressiveness <= 0) {
        return hp; // Full bypass
    }

    // Clamp boundary
    if (aggressiveness > 15) {
        aggressiveness = 15;
    }

    // Maps 1-15 to the number of active poles. 
    static const uint8_t kNumPoles[16] = {
        0, 
        1, 1, 1, 1, 
        2, 2, 2, 2, 
        3, 3, 3, 3, 
        4, 4, 4
    };

    // Linearly scales alpha to match the original 4 anchor points precisely:
    // Old L1 (8192), Old L2 (16384), Old L3 (24576), Old L4 (32768)
    static const int32_t kAlphaScale[16] = {
        0,
        2048,  4096,  6144,  8192,
        10240, 12288, 14336, 16384,
        18432, 20480, 22528, 24576,
        27306, 30037, 32768
    };

    // 32-bit math optimization. Does not require int64_t if LP_ALPHA <= 32768.
    int32_t alpha = (LP_ALPHA * kAlphaScale[aggressiveness]) >> 15;
    
    int32_t lp = hp;
    int poles = kNumPoles[aggressiveness];

    // 2. Multi-Pole Low-Pass Filter
    if (poles >= 1) {
        s_cvsdLp1 = s_cvsdLp1 + (((lp - s_cvsdLp1) * alpha) >> 15);
        lp = s_cvsdLp1;
    }
    if (poles >= 2) {
        s_cvsdLp2 = s_cvsdLp2 + (((lp - s_cvsdLp2) * alpha) >> 15);
        lp = s_cvsdLp2;
    }
    if (poles >= 3) {
        s_cvsdLp3 = s_cvsdLp3 + (((lp - s_cvsdLp3) * alpha) >> 15);
        lp = s_cvsdLp3;
    }
    if (poles >= 4) {
        s_cvsdLp4 = s_cvsdLp4 + (((lp - s_cvsdLp4) * alpha) >> 15);
        lp = s_cvsdLp4;
    }

    return lp;
}


#define YM_CLOCK 3579545
#define YM_NATIVE_RATE 44100


void Core1Main(void) {
    // Force hardware state clean on soft reboots
    s_ymAudioBuffer.head = 0;
    s_ymAudioBuffer.tail = 0;

    // Initialize MAME YM2151: Clock = 3.579545 MHz, Sample Rate = 44100 Hz
    ym2151_init(YM_CLOCK, YM_NATIVE_RATE);
    ym2151_reset_chip();

    // Tell Core 0 we are awake and the FIFO is clear
    g_core1Ready = true;

    uint8_t latchedAddress = 0;
    
    // MAME writes to an array of pointers to support multi-channel output
    int32_t outL;
    int32_t outR;
    
    // Use a scaled fixed-point value to prevent integer truncation drift
    // 1000000us * 65536 / 176400 = 371519 (5.6689... us per sample in Q16 format)
    const uint64_t samplePeriodScaled = (1000000ULL << 16) / 44100; 
    uint64_t nextSampleTimeScaled = time_us_64() << 16;
    uint64_t startUs = time_us_64();
    uint64_t totalSamplesGenerated = 0;
    uint64_t totalTicksGenerated = 0; // Track the 3.58MHz YM clock ticks
    uint64_t lastSampleVolumeChange = 0;
    int32_t stabilizedMainVolume = g_mainVolume;
    int32_t newMainVolume = g_mainVolume;

    g_core1Alive = true;

    // Register this core to automatically pause when Core 0 requests a lockout
    //multicore_lockout_victim_init();

    while (true) {
        // 1. Process 68B09 writes instantaneously
        while (multicore_fifo_rvalid()) {
            uint32_t cmd = multicore_fifo_pop_blocking();
            uint8_t port = (cmd >> 8) & 0xFF;
            uint8_t data = cmd & 0xFF;
            
            if (port == 0) {
                latchedAddress = data;
            } else if (port == 1) {
                ym2151_write_reg(latchedAddress, data);
                g_YmStatus = ym2151_read_status();
            }
        }

        // 2. Establish the current baseline time
        uint64_t elapsedUs = time_us_64() - startUs;

        // 3. Catch up the native 3.58MHz timers
        uint64_t targetTicks = (elapsedUs * YM_CLOCK) / 1000000ULL;
        while (totalTicksGenerated < targetTicks) {
            ym2151_advance_timers_one_tick();
            totalTicksGenerated++;
        }
        
        // Expose the updated status immediately to the Controller core 
        // in case the tick loop triggered an IRQ
        g_YmStatus = ym2151_read_status();

        // 4. Calculate exactly how many audio samples SHOULD exist by this microsecond
        uint64_t targetSamples = (elapsedUs * YM_NATIVE_RATE) / 1000000ULL;

        // 5. Catch up the 44.1kHz audio rendering if we are behind
        if (totalSamplesGenerated < targetSamples) {
            
            ym2151_update_one(&outL, &outR);

            // Process any CVSD data at 22010 Hz, which is 1% 
            // lower than it's produced, but it's a big ring buffer
            static bool CvsdToggle = false;            
            if (CvsdToggle) CVSDProcessTick();
            CvsdToggle = !CvsdToggle;

            // Filter the CVSD to replicate the low-pass filters
            // in U7A and U7B
            int32_t filteredCVSD = ApplyCVSDAnalogFilters(g_CurrentCVSDSample, 1);

            // Mix all the audio channels together
            int32_t mixedL = ((outL * g_YMGainL) + (g_CurrentDACSample * g_DACGainL) + (filteredCVSD * g_CVSDGainL))>>8;
            int32_t mixedR = ((outR * g_YMGainR) + (g_CurrentDACSample * g_DACGainR) + (filteredCVSD * g_CVSDGainR))>>8;

            // Apply the master volume 
            // (which is a value between 0-127)
            mixedL = (mixedL * stabilizedMainVolume)>>8;
            mixedR = (mixedR * stabilizedMainVolume)>>8;

            // Apply the soft-knee limiter
            int16_t finalL = (int16_t)ApplySoftKnee(mixedL);
            int16_t finalR = (int16_t)ApplySoftKnee(mixedR);

            AudioRingBufferPush(finalL, finalR);

            // Advance the target time for the next sample
            nextSampleTimeScaled += samplePeriodScaled;

            // Check to see if the volume needs to be updated
            if (g_mainVolume!=stabilizedMainVolume) {
                if (g_mainVolume!=newMainVolume) {
                    newMainVolume = g_mainVolume;
                    lastSampleVolumeChange = totalSamplesGenerated;
                } else if (totalSamplesGenerated > (lastSampleVolumeChange+2000)) {
                    stabilizedMainVolume = g_mainVolume;
                }
            }

            totalSamplesGenerated++;
        } else {
            // We are exactly on pace, yield the core
            tight_loop_contents();
        }
    }
}



// Called by Core 0 to pull rendered samples for the I2S DMA
bool AudioRingBufferPop(int16_t* left, int16_t* right) {
    if (s_ymAudioBuffer.head == s_ymAudioBuffer.tail) {
        return false; // Buffer empty
    }
    
    *left = s_ymAudioBuffer.buffer[s_ymAudioBuffer.tail];
    *right = s_ymAudioBuffer.buffer[s_ymAudioBuffer.tail + 1];
    
    s_ymAudioBuffer.tail = (s_ymAudioBuffer.tail + 2) % (AUDIO_BUFFER_SIZE * 2);
    return true;
}


// Calculates the number of stereo samples currently in the ring buffer
uint32_t GetBufferDepth(void) {
    uint32_t head = s_ymAudioBuffer.head;
    uint32_t tail = s_ymAudioBuffer.tail;
    
    if (head >= tail) {
        return (head - tail) / 2; // Divide by 2 because of left/right interleaving
    } else {
        return ((AUDIO_BUFFER_SIZE * 2) - tail + head) / 2;
    }
}
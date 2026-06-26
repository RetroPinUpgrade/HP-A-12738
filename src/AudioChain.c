#include "AudioChain.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "i2s.pio.h" 
#include "SineTable.h"
#include "Core1Audio.h"

#define PIN_SCK 15
#define PIN_BCK 26
#define PIN_LRCK 27
#define PIN_DOUT 28
#define PIN_MUTE 29

static PIO i2sPio = pio2;
static uint i2sSm = 0;


void SetAudioMute(bool mute) {
    // High = Unmute, Low = Mute
    gpio_put(PIN_MUTE, mute ? 0 : 1);
}


void InitAudioI2S(void) {
    gpio_init(PIN_MUTE);
    gpio_set_dir(PIN_MUTE, GPIO_OUT);
    gpio_put(PIN_MUTE, 0);

    gpio_init(PIN_SCK);
    gpio_set_dir(PIN_SCK, GPIO_OUT);
    gpio_put(PIN_SCK, 0);

    uint offset = pio_add_program(i2sPio, &audio_i2s_program);
    pio_sm_config c = audio_i2s_program_get_default_config(offset);
    
    sm_config_set_out_pins(&c, PIN_DOUT, 1);
    sm_config_set_sideset_pins(&c, PIN_BCK);
    sm_config_set_out_shift(&c, false, true, 32); 
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

    pio_gpio_init(i2sPio, PIN_DOUT);
    pio_gpio_init(i2sPio, PIN_BCK);
    pio_gpio_init(i2sPio, PIN_LRCK);

    pio_sm_set_pindirs_with_mask(i2sPio, i2sSm, 
        (1u << PIN_DOUT) | (3u << PIN_BCK), 
        (1u << PIN_DOUT) | (3u << PIN_BCK));

    // Integer clock calculation: 44.1kHz * 32 bits per frame * 2 PIO clock cycles per bit
    uint32_t sysFreq = clock_get_hz(clk_sys);
    sysFreq = 300000000; // hard-coded test
    uint32_t targetFreq = 44100 * 32 * 2;
    uint16_t divInt = sysFreq / targetFreq;
    uint8_t divFrac = ((sysFreq % targetFreq) * 256) / targetFreq;
    
    sm_config_set_clkdiv_int_frac(&c, divInt, divFrac);

    pio_sm_init(i2sPio, i2sSm, offset, &c);
    
    // Seed the initial X register with 14 for the 16-bit loop
    pio_sm_exec(i2sPio, i2sSm, pio_encode_set(pio_x, 14));
    pio_sm_set_enabled(i2sPio, i2sSm, true);
}


/* This version inserts zeroes on under-run -- not ideal
void ProcessAudioI2S(void) {
    static int16_t lastLeft = 0;
    static int16_t lastRight = 0;
    
    int16_t leftSample = 0;
    int16_t rightSample = 0;
    
    while (!pio_sm_is_tx_fifo_full(i2sPio, i2sSm)) {
        if (AudioRingBufferPop(&leftSample, &rightSample)) {
            lastLeft = leftSample;
            lastRight = rightSample;
        } else {
            // Force silence on buffer underflow to expose CPU starvation
            lastLeft = 0;
            lastRight = 0;
        }
        
        uint32_t stereoFrame = ((uint32_t)lastLeft << 16) | ((uint32_t)lastRight & 0xFFFF);
        pio_sm_put(i2sPio, i2sSm, stereoFrame);
    }
}
*/

void ProcessAudioI2S(void) {
    static int16_t lastLeft = 0;
    static int16_t lastRight = 0;
    
    int16_t leftSample = 0;
    int16_t rightSample = 0;
    
    while (!pio_sm_is_tx_fifo_full(i2sPio, i2sSm)) {
        if (AudioRingBufferPop(&leftSample, &rightSample)) {
            lastLeft = leftSample;
            lastRight = rightSample;
        } 
        // Do not force zeroes on underflow. 
        // Hold the last sample to stretch the wave so the scope can lock.
        
        uint32_t stereoFrame = ((uint32_t)lastLeft << 16) | ((uint32_t)lastRight & 0xFFFF);
        pio_sm_put(i2sPio, i2sSm, stereoFrame);
    }
}
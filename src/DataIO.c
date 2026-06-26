#include "DataIO.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "DataIO.pio.h"

#define PIN_DATA_IN_READY  9
#define PIN_DATA_IN_OC     10
#define PIN_DATA_OUT_LATCH 11
#define PIN_DATA_OUT_OC    12

#define BUS_PIN_START      16
#define BUS_PIN_END        23
#define BUS_MASK           (0xFF << BUS_PIN_START)

static volatile bool s_hasNewData = false;
static PIO s_pioInstance = pio0;
static uint s_pioSm = 0;

// Interrupt handler for the rising edge of DATA_IN_READY 
// The 138 goes LOW during the write cycle and HIGH at the end, which clocks the LVC374
static void DataIOInReadyCallback(uint gpio, uint32_t events) {
    if (gpio == PIN_DATA_IN_READY) {
        s_hasNewData = true;
    }
}


void DataIOInit(void) {
    // Configure the Data Bus as inputs by default
    for (int i = BUS_PIN_START; i <= BUS_PIN_END; i++) {
        gpio_init(i);
        gpio_set_dir(i, GPIO_IN);
    }

    // Configure DATA_IN_OC
    gpio_init(PIN_DATA_IN_OC);
    gpio_set_dir(PIN_DATA_IN_OC, GPIO_OUT);
    gpio_put(PIN_DATA_IN_OC, 1); // Active LOW, default to HIGH

    // Configure DATA_IN_READY Interrupt
    gpio_init(PIN_DATA_IN_READY);
    gpio_set_dir(PIN_DATA_IN_READY, GPIO_IN);
    gpio_set_irq_enabled_with_callback(PIN_DATA_IN_READY, GPIO_IRQ_EDGE_RISE, true, &DataIOInReadyCallback);

    // Load and configure the PIO program for outgoing data
    uint offset = pio_add_program(s_pioInstance, &AutoClearLatch_program);
    s_pioSm = pio_claim_unused_sm(s_pioInstance, true);
    AutoClearLatchProgramInit(s_pioInstance, s_pioSm, offset, PIN_DATA_OUT_LATCH, PIN_DATA_OUT_OC);}

bool DataIOHasNewData(void) {
    return s_hasNewData;
}


uint8_t DataIOReadData(void) {
    // Pull DATA_IN_OC LOW to enable U8 output
    gpio_put(PIN_DATA_IN_OC, 0);
    
    // LVC374 max output enable time is ~8ns.
    // RP2350 input synchronizer delay is ~2-3 cycles (10ns).
    // At 300MHz (3.33ns/cycle), we need ~20ns total. 8 NOPs provides ~26ns.
    __asm volatile (
        "nop\n nop\n nop\n nop\n"
        "nop\n nop\n nop\n nop\n"
    ); 
    
    // Read the bus
    uint32_t allPins = gpio_get_all();
    uint8_t mpuData = (uint8_t)((allPins & BUS_MASK) >> BUS_PIN_START);
    
    // Pull DATA_IN_OC HIGH to disable U8 output
    gpio_put(PIN_DATA_IN_OC, 1);
    
    // Clear the polling flag
    s_hasNewData = false;
    
    return mpuData;
}

void DataIOWriteData(uint8_t data) {
    // Switch the data bus to OUTPUT
    for (int i = BUS_PIN_START; i <= BUS_PIN_END; i++) {
        gpio_set_dir(i, GPIO_OUT);
    }
    
    // Apply the data byte to GPIO16-23
    gpio_put_masked(BUS_MASK, ((uint32_t)data) << BUS_PIN_START);
    
    // Force DATA_OUT_LATCH LOW first to guarantee a rising edge if it was already HIGH
    pio_sm_exec(s_pioInstance, s_pioSm, pio_encode_set(pio_pins, 0));
    
    // HCT374 data setup time is ~12ns. Clock pulse width minimum is ~16ns.
    // 8 NOPs provides ~26ns, satisfying both hardware requirements at 300MHz.
    __asm volatile (
        "nop\n nop\n nop\n nop\n"
        "nop\n nop\n nop\n nop\n"
    ); 
    
    // Force DATA_OUT_LATCH HIGH to clock data into U9 and flag the MPU
    pio_sm_exec(s_pioInstance, s_pioSm, pio_encode_set(pio_pins, 1));
    
    // Wait a few cycles for the HCT374 to register the clock edge (hold time ~5ns)
    __asm volatile ("nop\n nop\n nop\n");
    
    // Revert the data bus back to INPUT to prevent bus contention
    for (int i = BUS_PIN_START; i <= BUS_PIN_END; i++) {
        gpio_set_dir(i, GPIO_IN);
    }
}
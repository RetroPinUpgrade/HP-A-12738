#ifndef DATAIO_H
#define DATAIO_H

#include <stdint.h>
#include <stdbool.h>

// Initialize GPIO, Interrupts, and PIO State Machine
void DataIOInit(void);

// Returns true if a new write from the MPU has been latched into U8
bool DataIOHasNewData(void);

// Asserts DATA_IN_OC, reads the bus, de-asserts, and clears the new data flag
uint8_t DataIOReadData(void);

// Drives the bus, triggers the PIO to latch into U9, and waits for latch completion
void DataIOWriteData(uint8_t data);

#endif // DATAIO_H
#ifndef WPCS_CPU_6809_H
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool WPCSCPUSetCallbacks(void (*memoryWriteFunction)(uint16_t, uint8_t), uint8_t (*memoryReadFunction)(uint16_t));
void WPCSCPUIRQ();
void WPCSCPUFIRQ(bool firqOn);
void WPCSCPUNMI();

void WPCSCPUReset();
uint16_t WPCSCPUStep();
uint16_t WPCSCPUSteps(uint16_t numTicks);
uint16_t WPCSCPUGetPC();

void WPCSCPUEnableLogging(bool loggingOn);

#ifdef __cplusplus
}
#endif

#define WPCS_CPU_6809_H
#endif



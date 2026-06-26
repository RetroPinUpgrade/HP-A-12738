#ifndef CORE_1_MAIN_H
#define CORE_1_MAIN_H

void Core1Main(void);
bool AudioRingBufferPop(int16_t* left, int16_t* right);
uint32_t GetBufferDepth(void);


#endif
#ifndef AUDIO_CHAIN_H
#define AUDIO_CHAIN_H

#include <stdbool.h>

void InitAudioI2S(void);
void SetAudioMute(bool mute);
void ProcessAudioI2S(void);

#endif 
// AUDIO_CHAIN_H
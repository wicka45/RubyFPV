#pragma once
#include "../base/base.h"
#include "../base/config.h"

bool hardware_board_has_audio_builtin(u32 uBoardType);
bool hardware_has_audio_playback();
const char* hardware_audio_get_playback_device();
void hardware_audio_system_volume_step(int iDeltaPercent);   // x64: 0=mute toggle, +/-N=percent step (ALSA hw mixer)
int hardware_enable_audio_output();
bool hardware_has_audio_volume();
int hardware_set_audio_output_volume(int iAudioVolume);
int hardware_audio_play_file(const char* szFile);
int hardware_audio_play_file_async(const char* szFile);
void hardware_audio_stop_async_play();


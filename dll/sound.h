// Sound effects, mixed in software and played through waveOut.
// The clips come from a pack built by tools/uk_sounds.py out of the user's own copy of ULTRAKILL.
#pragma once

// Loads the pack and starts the mixer thread. Safe to call repeatedly; false if there is nothing to play.
bool sound_init(const wchar_t *pack_path);

// Plays a sound by name. `pitch` is a playback speed, as Unity's AudioSource.pitch is.
// Returns a voice id for sound_set / sound_stop, or 0 if the sound is missing. Callable from any thread.
// A sound played on a `channel` above 0 cuts whatever was playing on that channel, the way one Unity
// AudioSource can only play one clip: ULTRAKILL's revolver has a single source, so each shot ends the last
// shot's tail. Channel 0 sounds overlap freely (ULTRAKILL spawns those as separate objects).
int sound_play(const char *name, float volume = 1.0f, float pitch = 1.0f, bool loop = false, int channel = 0);
void sound_set(int voice, float volume, float pitch);
void sound_stop(int voice);

// Everything is scaled by this (0..1). The default is 0.2.
void sound_master(float volume);

// How many times the output ran out of queued audio (an audible dropout), for the log.
int sound_underruns();

// Writes the last 30 seconds the mixer sent to the audio device as a WAV file, and next to it a text
// file listing every sound started in that time. For finding out why something sounded wrong.
bool sound_dump(const wchar_t *wav_path);

// For the offline test (dll/sound_test.cpp): load without an audio device, and render 48 kHz stereo frames.
bool sound_init_offline(const wchar_t *pack_path);
void sound_render(short *out, int frames);

// Why sound_init failed, for the log.
const char *sound_error();

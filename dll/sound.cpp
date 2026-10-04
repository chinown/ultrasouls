// Sound effects: a small software mixer feeding the Windows audio engine (WASAPI, shared mode).
//
// v0.41 to v0.44 fed waveOut instead. Measured outside the game (sound_rt_test.cpp), that path only
// took 5.6 s of audio in 6.5 s: with 60 ms queued, waveOut's own buffering ran dry between blocks, so
// about a seventh of the time was silence chopped into every sound. That was the "buffer problem".
// Here the engine wakes the mixer each time it wants more, and the mixer tops its buffer up.
//
// A pack ("USSND001", count, then name[40], sample rate, channels, frame count, int16 PCM per sound) is
// loaded whole. Up to 32 voices play at once; each reads its clip at its own speed (pitch times the
// clip's rate over the output rate) with linear interpolation, and is mixed into 48 kHz stereo.
#include "sound.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Mixed as 48 kHz 16-bit stereo, in pieces of at most BLOCK_FRAMES; Windows converts to the device's format.
const int OUT_RATE = 48000, BLOCK_FRAMES = 720, MAX_VOICES = 32;
const REFERENCE_TIME BUFFER_100NS = 300000;      // 30 ms asked for; the engine may round it up
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

struct Clip {
    char name[41];
    uint32_t rate, channels, frames;
    const int16_t *pcm;
};
struct Voice {
    const Clip *clip;
    double pos;          // in the clip's frames
    float volume, pitch;
    bool loop;
    int id;              // 0 = free
    int channel;         // above 0: only one voice per channel
    float gain;          // 1, or falling to 0 while the voice is being cut
    bool ending;
};
const float END_STEP = 1.0f / 240.0f;   // a cut voice fades over 5 ms rather than stopping on a step

std::vector<uint8_t> g_data;
std::vector<Clip> g_clips;
Voice g_voices[MAX_VOICES];
CRITICAL_SECTION g_lock;
bool g_ready = false, g_tried = false;
int g_next_id = 1;
// The clips are mastered to full scale (shots average -12 dBFS); at 0.6 the mix averaged -16 dBFS
// with peaks at -9, far above Dark Souls' own audio.
float g_master = 0.2f;
volatile long g_underruns = 0;

// what was sent to the device lately, and which sounds were started, for sound_dump
const int DUMP_FRAMES = OUT_RATE * 30;
std::vector<int16_t> g_history;                 // DUMP_FRAMES * 2 samples, used as a ring
long long g_history_frames = 0;                 // frames written in total
struct Started { long long frame; char name[24]; float volume, pitch; int channel; };
Started g_started[512];
int g_started_n = 0;
std::string g_error;
IAudioClient *g_client = nullptr;
IAudioRenderClient *g_render = nullptr;
HANDLE g_event = nullptr, g_started_event = nullptr;
UINT32 g_buffer_frames = 0;
volatile long g_device_ok = 0;

void mix(int16_t *out, int frames) {
    static float acc[BLOCK_FRAMES * 2];
    memset(acc, 0, sizeof(float) * frames * 2);
    EnterCriticalSection(&g_lock);
    for (Voice &v : g_voices) {
        if (!v.id) continue;
        const Clip &c = *v.clip;
        double step = (double)v.pitch * c.rate / OUT_RATE;
        if (step < 0.0005) step = 0.0005;                 // a pitch of zero would never finish
        float vol = v.volume * g_master;
        for (int i = 0; i < frames; i++) {
            if (v.ending && (v.gain -= END_STEP) <= 0) { v.id = 0; break; }
            if (v.pos >= c.frames - 1) {
                if (!v.loop) { v.id = 0; break; }
                v.pos = fmod(v.pos, (double)(c.frames - 1));
            }
            uint32_t k = (uint32_t)v.pos;
            float f = (float)(v.pos - k);
            const int16_t *s = c.pcm + (size_t)k * c.channels;
            float l = s[0] + (s[c.channels] - s[0]) * f;
            float r = c.channels > 1 ? s[1] + (s[c.channels + 1] - s[1]) * f : l;
            acc[i * 2] += l * vol * v.gain;
            acc[i * 2 + 1] += r * vol * v.gain;
            v.pos += step;
        }
    }
    LeaveCriticalSection(&g_lock);
    for (int i = 0; i < frames * 2; i++) {
        float x = acc[i];
        out[i] = (int16_t)(x > 32767.0f ? 32767.0f : x < -32768.0f ? -32768.0f : x);
    }
    if (!g_history.empty()) {
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < frames; i++) {
            size_t k = (size_t)(g_history_frames % DUMP_FRAMES) * 2;
            g_history[k] = out[i * 2];
            g_history[k + 1] = out[i * 2 + 1];
            g_history_frames++;
        }
        LeaveCriticalSection(&g_lock);
    }
}

// Opens the default output device. COM objects are created and used on the mixer thread only.
bool open_device() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) { /* already initialised another way: still usable */ }
    IMMDeviceEnumerator *devices = nullptr;
    IMMDevice *device = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&devices))) {
        g_error = "no audio device enumerator";
        return false;
    }
    HRESULT hr = devices->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    devices->Release();
    if (FAILED(hr)) { g_error = "no default audio output"; return false; }
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&g_client);
    device->Release();
    if (FAILED(hr)) { g_error = "audio client unavailable"; return false; }
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = OUT_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = OUT_RATE * 4;
    hr = g_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                              AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                              BUFFER_100NS, 0, &fmt, nullptr);
    if (FAILED(hr)) { g_error = "audio client would not start a 48 kHz stereo stream"; return false; }
    g_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(g_client->SetEventHandle(g_event)) || FAILED(g_client->GetBufferSize(&g_buffer_frames)) ||
        FAILED(g_client->GetService(__uuidof(IAudioRenderClient), (void **)&g_render))) {
        g_error = "audio stream setup failed";
        return false;
    }
    return true;
}

DWORD WINAPI mixer_thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    bool ok = open_device() && SUCCEEDED(g_client->Start());
    InterlockedExchange(&g_device_ok, ok ? 1 : 0);
    SetEvent(g_started_event);
    if (!ok) return 0;
    bool primed = false;
    for (;;) {
        if (WaitForSingleObject(g_event, 200) != WAIT_OBJECT_0) continue;
        UINT32 queued = 0;
        if (FAILED(g_client->GetCurrentPadding(&queued))) continue;
        if (primed && queued == 0) InterlockedIncrement(&g_underruns);   // the engine had used everything up: a dropout
        UINT32 want = g_buffer_frames - queued;
        while (want > 0) {
            UINT32 n = want > (UINT32)BLOCK_FRAMES ? (UINT32)BLOCK_FRAMES : want;
            BYTE *data = nullptr;
            if (FAILED(g_render->GetBuffer(n, &data))) break;
            mix((int16_t *)data, (int)n);
            g_render->ReleaseBuffer(n, 0);
            want -= n;
        }
        primed = true;
    }
    return 0;
}

bool load_pack(const wchar_t *path) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) { g_error = "sound pack not found"; return false; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_data.resize(size > 0 ? size : 0);
    size_t got = fread(g_data.data(), 1, g_data.size(), f);
    fclose(f);
    if (got != g_data.size() || size < 12 || memcmp(g_data.data(), "USSND001", 8) != 0) { g_error = "sound pack unreadable"; return false; }
    uint32_t count;
    memcpy(&count, g_data.data() + 8, 4);
    size_t at = 12;
    for (uint32_t i = 0; i < count; i++) {
        if (at + 52 > g_data.size()) { g_error = "sound pack truncated"; return false; }
        Clip c{};
        memcpy(c.name, g_data.data() + at, 40);
        memcpy(&c.rate, g_data.data() + at + 40, 4);
        memcpy(&c.channels, g_data.data() + at + 44, 4);
        memcpy(&c.frames, g_data.data() + at + 48, 4);
        at += 52;
        size_t bytes = (size_t)c.frames * c.channels * 2;
        if (!c.rate || c.channels < 1 || c.channels > 2 || c.frames < 2 || at + bytes > g_data.size()) { g_error = "sound pack entry invalid"; return false; }
        c.pcm = (const int16_t *)(g_data.data() + at);
        at += bytes;
        g_clips.push_back(c);
    }
    return !g_clips.empty();
}

}   // namespace

// For dll/sound_test.cpp: load the pack without opening an audio device, then pull mixed audio directly.
bool sound_init_offline(const wchar_t *pack_path) {
    if (g_tried) return g_ready;
    g_tried = true;
    InitializeCriticalSection(&g_lock);
    g_ready = load_pack(pack_path);
    return g_ready;
}
void sound_render(short *out, int frames) {
    while (frames > 0) {
        int n = frames > BLOCK_FRAMES ? BLOCK_FRAMES : frames;
        mix(out, n);
        out += n * 2;
        frames -= n;
    }
}

bool sound_init(const wchar_t *pack_path) {
    if (g_tried) return g_ready;
    g_tried = true;
    InitializeCriticalSection(&g_lock);
    if (!load_pack(pack_path)) return false;
    g_history.assign((size_t)DUMP_FRAMES * 2, 0);
    g_started_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    CreateThread(nullptr, 0, mixer_thread, nullptr, 0, nullptr);
    WaitForSingleObject(g_started_event, 3000);           // the thread opens the device and reports back
    g_ready = g_device_ok != 0;
    if (!g_ready && g_error.empty()) g_error = "audio device did not start";
    return g_ready;
}

bool sound_dump(const wchar_t *wav_path) {
    if (!g_ready || g_history.empty()) return false;
    EnterCriticalSection(&g_lock);
    long long total = g_history_frames;
    int frames = (int)(total < DUMP_FRAMES ? total : DUMP_FRAMES);
    std::vector<int16_t> copy((size_t)frames * 2);
    long long first = total - frames;
    for (int i = 0; i < frames; i++) {
        size_t k = (size_t)((first + i) % DUMP_FRAMES) * 2;
        copy[(size_t)i * 2] = g_history[k];
        copy[(size_t)i * 2 + 1] = g_history[k + 1];
    }
    Started started[512];
    int n = g_started_n < 512 ? g_started_n : 512;
    for (int i = 0; i < n; i++) started[i] = g_started[(g_started_n - n + i) % 512];
    float master = g_master;
    long underruns = g_underruns;
    LeaveCriticalSection(&g_lock);

    FILE *w = _wfopen(wav_path, L"wb");
    if (!w) return false;
    uint32_t bytes = (uint32_t)(copy.size() * 2), u32;
    uint16_t u16;
    fwrite("RIFF", 1, 4, w); u32 = 36 + bytes; fwrite(&u32, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w);
    u32 = 16; fwrite(&u32, 4, 1, w); u16 = 1; fwrite(&u16, 2, 1, w); u16 = 2; fwrite(&u16, 2, 1, w);
    u32 = OUT_RATE; fwrite(&u32, 4, 1, w); u32 = OUT_RATE * 4; fwrite(&u32, 4, 1, w); u16 = 4; fwrite(&u16, 2, 1, w); u16 = 16; fwrite(&u16, 2, 1, w);
    fwrite("data", 1, 4, w); fwrite(&bytes, 4, 1, w); fwrite(copy.data(), 1, bytes, w);
    fclose(w);
    std::wstring txt = std::wstring(wav_path) + L".txt";
    FILE *t = _wfopen(txt.c_str(), L"w");
    if (t) {
        fprintf(t, "master volume %.2f, dropouts %ld, %d frames\n", master, underruns, frames);
        for (int i = 0; i < n; i++)
            if (started[i].frame >= first)
                fprintf(t, "%7.3f s  %-18s volume %.2f pitch %.2f channel %d\n", (double)(started[i].frame - first) / OUT_RATE, started[i].name,
                        started[i].volume, started[i].pitch, started[i].channel);
        fclose(t);
    }
    return true;
}

int sound_play(const char *name, float volume, float pitch, bool loop, int channel) {
    if (!g_ready) return 0;
    const Clip *clip = nullptr;
    for (const Clip &c : g_clips)
        if (strcmp(c.name, name) == 0) { clip = &c; break; }
    if (!clip) return 0;
    int id = 0;
    EnterCriticalSection(&g_lock);
    if (channel > 0)
        for (Voice &v : g_voices)
            if (v.id && v.channel == channel) v.ending = true;
    Voice *slot = nullptr;
    for (Voice &v : g_voices)
        if (!v.id) { slot = &v; break; }
    if (!slot) {                                          // all busy: take over the one furthest along that is not a loop
        double best = -1;
        for (Voice &v : g_voices)
            if (!v.loop && v.pos / v.clip->frames > best) { best = v.pos / v.clip->frames; slot = &v; }
    }
    if (slot) {
        Started &st = g_started[g_started_n++ % 512];
        st.frame = g_history_frames;
        strncpy(st.name, name, sizeof(st.name) - 1);
        st.name[sizeof(st.name) - 1] = 0;
        st.volume = volume;
        st.pitch = pitch;
        st.channel = channel;
        id = g_next_id++;
        if (g_next_id <= 0) g_next_id = 1;
        *slot = Voice{clip, 0.0, volume, pitch, loop, id, channel, 1.0f, false};
    }
    LeaveCriticalSection(&g_lock);
    return id;
}

void sound_set(int voice, float volume, float pitch) {
    if (!g_ready || !voice) return;
    EnterCriticalSection(&g_lock);
    for (Voice &v : g_voices)
        if (v.id == voice) { v.volume = volume; v.pitch = pitch; }
    LeaveCriticalSection(&g_lock);
}

void sound_stop(int voice) {
    if (!g_ready || !voice) return;
    EnterCriticalSection(&g_lock);
    for (Voice &v : g_voices)
        if (v.id == voice) v.ending = true;
    LeaveCriticalSection(&g_lock);
}

void sound_master(float volume) { g_master = volume < 0 ? 0 : volume > 1 ? 1 : volume; }

int sound_underruns() { return (int)g_underruns; }

const char *sound_error() { return g_error.c_str(); }

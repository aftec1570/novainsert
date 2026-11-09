// Minimal ABI-compatible REAPER SDK types. The vendored SDK's reaper_plugin.h pulls in
// WDL/swell (not vendored); NovaInsert only needs these few POD definitions, so they are
// declared here with the exact layouts from reaper_plugin.h / swell-types.h (see F0.6 spike).
#pragma once

typedef double ReaSample;

typedef struct audio_hook_register_t
{
    void (*OnAudioBuffer)(bool isPost, int len, double srate, struct audio_hook_register_t* reg);
    void* userdata1;
    void* userdata2;
    // set by host; only valid inside OnAudioBuffer
    int input_nch, output_nch;
    ReaSample* (*GetBuffer)(bool isOutput, int idx);
} audio_hook_register_t;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // HWND, GUID
#else
typedef void* HWND;
typedef struct   // swell-types.h layout: 16 bytes (Data1 is 32-bit; `unsigned long` would be 8 bytes on LP64)
{
    unsigned int Data1;
    unsigned short Data2;
    unsigned short Data3;
    unsigned char Data4[8];
} GUID;
#endif

struct ReaProject;
struct MediaTrack;
struct KbdSectionInfo;

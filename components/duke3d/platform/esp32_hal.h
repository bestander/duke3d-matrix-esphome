#pragma once
#include <cstdint>
#include <cstdio>

// Platform shim declarations for Duke3D engine.
// These replace the original ODROID-GO / ILI9341 platform functions.

// Called by engine to output one rendered frame.
// src: 320x200 palette-indexed buffer. pal: 256xRGB888 palette.
void platform_blit_frame(const uint8_t* src, const uint8_t* pal);

// Called by engine's file I/O layer — redirects to SD card mount point.
// Returns a FILE* on /sdcard/duke3d/<rel_path>, or nullptr on failure.
FILE* platform_open_file(const char* rel_path, const char* mode);

// Called by engine's audio layer — stub until Phase 7.
// n: number of int16_t samples (both channels combined).
void platform_audio_write(const int16_t* pcm, int n);

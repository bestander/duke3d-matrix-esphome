# Music Streaming Proposal

Duke3D music is stored in the GRP as HMI-format MIDI sequences (~20–80 KB each).
This is fundamentally different from sound effects (raw PCM): MIDI is an event list,
not audio data. You cannot stream-and-mix it directly — a synthesis step is required.

Currently music is disabled (`MusicDevice = NumSoundCards`, `MusicToggle = 0`).

---

## Why the SFX streaming approach doesn't apply directly

Sound effects: raw PCM bytes → read from GRP → mix into output buffer. Zero processing.

Music: MIDI events → **synthesis engine** → PCM → mix into output buffer.

The SDL desktop port sidesteps this by loading the whole song into a 100 MB buffer and
handing it to `SDL_mixer`'s software synth. That path is impossible on ESP32.

---

## Option A — Pre-rendered PCM on SD

Convert all Duke3D music tracks to raw 11025 Hz mono 8-bit `.raw` files and place them
on the SD card alongside `DUKE3D.GRP`. The music "voice" reads 256 bytes per mix period
— identical to the SFX streaming mechanism. No synthesis, no decoder, no new dependencies.

**How to convert** (requires a MIDI soundfont or the original OPL2 recording):
```bash
# From OGG/MP3 source (e.g. Duke3D Redux or similar):
ffmpeg -i track01.ogg -ar 11025 -ac 1 -acodec pcm_u8 -f u8 music/track01.raw

# From MIDI + soundfont:
fluidsynth -F track01.wav soundfont.sf2 track01.mid
ffmpeg -i track01.wav -ar 11025 -ac 1 -acodec pcm_u8 -f u8 music/track01.raw
```

**Storage:** ~11 KB/s × avg 180 s/track = ~2 MB/track.
- Shareware (5 tracks): ~10 MB
- Atomic Edition (28 tracks): ~56 MB

Both fit comfortably on a 128 MB or larger SD card alongside the GRP.

**SD bandwidth:** music adds ~11 KB/s to the SD read load. Combined with 8 SFX voices
at ~89 KB/s, total is ~100 KB/s — under 10% of the 1 MB/s SPI throughput budget.

**RAM cost:** zero. The pre-rendered track is never buffered in PSRAM — same
256-byte stack-local read window as SFX voices.

**Quality:** depends on source. OPL2 recordings sound authentic; a good soundfont
render can sound richer but less "DOS-era".

**Integration sketch:**
- `PlayMusic(filename)`: look up the track number from `filename`, open
  `/sdcard/duke3d/music/trackNN.raw`, store fd + length in a `MusicVoice` struct.
- Audio pump task: on each mix period, `fread(256 bytes)` from the music fd into
  a stack buffer, mix at full volume into the paintbuffer before SFX voices.
  Loop at EOF if `loopflag` set.
- `MUSIC_StopSong()`: close the fd.
- No changes to `multivoc.c` mixing loop — music is just another 8-bit mono
  stream fed through the same path.

---

## Option B — Software OPL2 Emulator

Port a lightweight OPL2 emulator. The HMI MIDI file (~50 KB) stays in PSRAM during
playback; the emulator generates a `musBuffer[256]` of PCM samples per mix period and
mixes them into the output. This is exactly what the MGM240 port does.

**Emulator candidates:**

| Library | Code size | License | Notes |
|---------|-----------|---------|-------|
| `emu8950` | ~12 KB | MIT | Minimal YM3812 (OPL2); used in several embedded ports |
| `fmopl` (MAME) | ~25 KB | LGPL | More accurate; heavier |
| `nuked-opl3` | ~30 KB | LGPL | OPL3 superset; overkill but very accurate |

`emu8950` is the right choice for this platform.

**CPU cost:** OPL2 synthesis at 11025 Hz on ESP32-S3 @ 240 MHz is roughly 2–5% of one
core. Well within budget alongside the game task.

**RAM cost:** HMI file in PSRAM (~50 KB peak), OPL2 register state (~2 KB BSS),
`musBuffer[256]` on audio task stack (freed each mix period).

**Quality:** identical to the original DOS experience — OPL2 FM synthesis is what
the game was composed for.

**Integration sketch:**
- Add `emu8950` (single `.c` + `.h`) to `AUDIOLIB_SRCS` in `CMakeLists.txt`.
- Implement `MUSIC_Init` / `MUSIC_PlaySong` / `MUSIC_StopSong` in a new
  `components/duke3d/engine/components/audiolib/esp_music.c`.
- `MUSIC_PlaySong`: read HMI file from GRP into PSRAM buffer; init HMI sequencer
  state; init OPL2 emulator at 11025 Hz.
- Audio pump task (in `dsl.c`): after `MV_ServiceVoc()`, call `OPL2_Update(musBuffer, 256)`;
  mix `musBuffer` into the same output buffer before `platform_audio_write`.
- HMI sequencer: Duke3D uses Apogee's HMI format; a small sequencer (~200 lines)
  advances MIDI events and writes OPL2 register writes. Reference: the `midi.c` /
  `al_midi.c` files already in the audiolib (DOS OPL2 driver code to adapt from).

---

## Comparison

| | Option A (pre-rendered PCM) | Option B (OPL2 emulator) |
|---|---|---|
| Implementation effort | Low | Medium–High |
| New dependencies | None | `emu8950` + HMI sequencer |
| RAM during playback | 0 bytes PSRAM | ~52 KB PSRAM |
| SD storage needed | ~10–56 MB extra | 0 (HMI already in GRP) |
| Audio quality | Source-dependent | Authentic OPL2 |
| Looping accuracy | Sample-accurate | MIDI-tick-accurate |
| Works with Shareware GRP | Yes (if you pre-render the tracks) | Yes (HMI files are in GRP) |

---

## Recommendation

Start with **Option A**. It reuses the SFX streaming infrastructure directly, has no
new runtime dependencies, and gets music working in a single afternoon. If the FM
synthesis sound is important later, Option B can be layered in without changing the
overall audio architecture.

Option A requires pre-rendering the tracks once. Good free sources for Duke3D OPL2
recordings exist (the MIDI files + a GM soundfont, or community OPL2 captures).

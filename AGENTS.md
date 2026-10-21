# AGENTS.md

Guide for coding agents working on cucumber-satellite. User-facing docs: `README.md` (Russian).

## What this is

A native C++ daemon replacing the Yandex stack (`maind` / `quasar_launcher2`) on a rooted Yandex
Station Midi (`cucumber`: Buildroot, glibc 2.36, Cortex-A55, 1 GB RAM). It exposes the speaker to
Home Assistant over the ESPHome Native API (plaintext, port 6053) and to Music Assistant as a
Sendspin player (port 8928). Wake word, command spotting and echo cancellation use the firmware's
own proprietary libraries, loaded with `dlopen` at runtime; they are never copied into the repo.

## Layout

| Path | Content |
|---|---|
| `satellite/src/core/` | epoll event loop (`Loop`: fds, timers, cross-thread `post`), logging |
| `satellite/src/esphome/` | ESPHome API server (hand-written protobuf in `proto.h`), mDNS responder |
| `satellite/src/audio/frontend.*` | capture thread: ALSA `hw:0,2` → Yandex VQE → wake + command spotters → 16 kHz stream |
| `satellite/src/media/player.*` | playback through the firmware's GStreamer (`libquasar_gstreamer.so`), channels MUSIC / ANNOUNCE; `Mixer` = ALSA "Master Playback Volume" (softvol of the `speakers` PCM) |
| `satellite/src/sendspin/` | Sendspin player on top of sendspin-cpp, own ALSA sink on the `speakers` dmix |
| `satellite/src/hw/` | LED ring / logo (`/dev/fb0`, `/dev/fb1`), clock (sysfs segments), input (`awinic_sar` keys, `SW_MUTE_DEVICE`), light sensor (`ltr308als01`) |
| `satellite/src/controller.*` | all satellite logic: voice pipeline state machine, entities, local commands, persistence |
| `satellite/deploy/` | `install.sh` (host side); `boot.sh`, `supervise.sh`, `ctl.sh` (device side) |
| `satellite/tools/` | probes: `hw_probe`, `player_probe`, `ww_offline` (offline spotter test on a WAV), `api_selftest` |
| `satellite/third_party/` | `fetch.sh` (pinned deps), `sysroot.sh` (alsa-lib link sysroot), CMake glue, zig toolchain wrappers |
| `ssh/` | `build.sh` (static dropbear + sftp-server), `start.sh` (device side) |

Git-ignored and never to be published: `backup/` (full device backups), `dump/` (firmware binaries,
logs), `re/` (Ghidra projects and decompiled Yandex code), `ref/` (upstream checkouts).

## Build

```sh
make -C satellite -j"$(nproc)"            # → satellite/build/cucumber-satellite
make -C satellite build/tools/ww_offline   # any tool in satellite/tools/
ssh/build.sh                               # → ssh/out/{dropbearmulti,sftp-server}
```

Host tools: zig 0.13, make, git, curl, cmake ≥ 3.16, ninja, patch. The first build fetches pinned
dependencies into `satellite/third_party/src/` and builds the ALSA sysroot into
`satellite/build/sysroot/`. There is no host test suite; verification happens on the device.

## Deploy

```sh
satellite/deploy/install.sh --list         # discover stations (mDNS; --scan = adb port scan of the /24) + model probe
satellite/deploy/install.sh <ip>[:port]    # build if needed, copy binary (+ .prev), add boot hook, restart
```

Models are identified by `/proc/device-tree/compatible` (`yandex,<codename>`). Only codenames in
`SUPPORTED` in `install.sh` are installed (currently `cucumber` = Midi); others, e.g. `plum` =
Lite 2, are listed and skipped. Porting a model means a hardware study first (ALSA capture layout,
VQE preset name, spotter models, LEDs, inputs), then runtime model selection in the daemon.

Manual binary update (e.g. over ssh when adb push is flaky for the ~24 MB binary):

```sh
scp satellite/build/cucumber-satellite root@<ip>:/data/cucumber-satellite/cucumber-satellite.new
ssh root@<ip> 'cd /data/cucumber-satellite && cp cucumber-satellite cucumber-satellite.prev &&
  mv cucumber-satellite.new cucumber-satellite && ./ctl.sh restart'
```

Device layout: `/data/cucumber-satellite/{cucumber-satellite,.prev,boot.sh,supervise.sh,ctl.sh,
state.conf,enabled,sendspin/}`. Log: `/tmp/cucumber-satellite.log` (`ctl.sh log N`). Boot chain:
`onboot.sh` (firmware) → `/data/quasar/rootkeeper.sh` → `boot.sh` → waits for quasar, `q stop` →
`supervise.sh` restart loop while `enabled` exists.

## Device safety rules

- **Never break root adb access.** Do not touch adbd, `/data/quasar/activate_*` markers, or the
  existing contents of `/data/quasar/rootkeeper.sh`. It is shared with other tooling: only insert
  hook lines, and only before its endless `while` loop (code after the loop never runs).
- Do not modify the rootfs (`/system`, `/etc`); everything persistent lives in `/data`.
- Several devices may share one adb server: always pass `adb -s <serial>`.
- `adb shell` on this firmware is a pty: output has `\r`, is not binary-safe (use push/pull) and
  the exit status is always 0 (see `sh_()` in `install.sh`).
- Before replacing the binary, keep the working one as `.prev`. A crashing binary is restarted by
  `supervise.sh` every 3 s; roll back with `.prev` or `ctl.sh disable` (starts the Yandex stack).
- Do not open ESPHome API connections to a device that Home Assistant is using; test against a
  separate binary on another port with the service stopped (`ctl.sh disable` or kill the supervisor).
- No volume changes or test sounds on a live speaker without the owner's consent; tests that play
  audio must cap volume.
- `/tmp` is tmpfs on a 1 GB device: remove test files afterwards.

## Yandex libraries (reverse-engineered C API, see `src/audio/frontend.cpp`)

From `/system/vendor/quasar/libyandex_evil.so`:

- VQE: `YandexVqeC_Config_CreateFromPreset("cucumber", &cfg)`,
  `YandexVqeC_Create(cfg, "/system/vendor/quasar/vqe_models", 0, nullptr, &vqe)`,
  `YandexVqeC_Process(vqe, mics, 4 * 3072, spk, 4 * 3072)` with float samples in [-1, 1] (mic
  channels 0–3 and speaker reference channels 4–7 of `hw:0,2`, 8 ch @ 96 kHz),
  `YandexVqeC_GetOutputChannelData(vqe, type, idx, &ptr, &n)` → 512 samples @ 16 kHz.
- Spotter: `spotter_create_multichannel(conf, 16000, n, types, false, &err)`,
  `spotter_submit_raw_data(sp, s16_interleaved, bytes, &err)`. Channel count per type comes from the
  model config. Hit events: 2 and 5.
- Spotter type → VQE output: type 0 = main channel. The wake model (`spotter_models/activation/alisa`)
  needs `VQE_OMNI_WITH_GAIN` (9); the command model (`spotter_models/command`, single channel)
  only works with `VQE_OMNI` (6). Measured with `ww_offline`; re-measure when changing it.
- The command spotter only runs while something plays, during TTS/announcements/timer ring, or
  2.5 s after the wake word (as `maind` does), to avoid false positives from conversation.
- `GetOutputChannelData` always returns no error; an absent output type yields `n == 0`.
- `YandexVqeC_Config_GetOutputChannelCount` takes `(cfg, type, int *out)`; the other `Config_Get*`
  getters return the value directly.

## Station Lite 2 (`plum`) — studied, not supported yet

Findings from the live device (firmware 0.369.1.46, read-only study):

| | Midi (`cucumber`) | Lite 2 (`plum`) |
|---|---|---|
| SoC / userland | Amlogic A5, aarch64, glibc 2.36, 1 GB RAM | Amlogic AXG, Cortex-A53, **32-bit armhf** userland, glibc 2.30, kernel 4.9, **237 MB RAM** |
| Storage | `/data` partition (several GB) | `/data` → `/bkupgrade/data` (ubifs, ~81 MB free); rootfs ubifs ro, 87 % full |
| Capture | `hw:0,2`, 8 ch @ 96 kHz: mics 0–3, speaker ref 4–7 | `hw:0,1`, 8 ch @ 48 kHz: mics on ch 0 and 2, speaker feedback on ch 6 (`/etc/asound.conf`, `multi_capture_raw`) |
| VQE preset | `cucumber`: 4 mics, 4 spk, chunk 3072 | `plum`: 2 mics, 1 spk, 48 kHz, chunk 1536 → 512 @ 16 kHz; outputs 1, 2, 4, 6, 8, 9 |
| Spotters | alisa: types 0, 3, 4; command: type 0 | alisa/yandex: types 0 and 4 (1 ch each); command: type 0. Models in `spotter_models/` are plum-tuned |
| Ring | `/dev/fb0` 24×1 + `/dev/fb1` logo 6×1, BGR | `/dev/fb0` "YNDX LED ring" 3×1, 24 bpp, r 16 / g 8 / b 0 (BGR bytes); trigger `led_ring_fb`; no logo |
| Clock | `cucumber:white:clock-<seg><digit>`, dimming via chip `current` | `plum:white:clock<part><pos>`: parts A–H × positions 0–3, colon `DH`+`DL`, also `DM`, `DEGREE` (36 LEDs, see `quasar.cfg` `/leds/clock`); dimming via per-LED `brightness` 0–255 |
| Keys | `awinic_sar`: 582 action, 115/114 volume | `aw9610x`: same codes 582, 115, 114 |
| Mic mute | input `mute`: `SW_MUTE_DEVICE` | input `gpio-keys`: `SW_MUTE_DEVICE` (+ `KEY_MUTE`) |
| Light sensor | `ltr308als01` (iio) | same |
| Volume | "Master Playback Volume" softvol on `speakers` | same names; dmix 48 kHz stereo |
| Earcons | `common/spotter_*.mp3`, `timer.wav`, `ru-RU/vins_error.wav` | same except no `vins_error.wav` (`ru-RU/playback_error.mp3` exists) |
| Boot hook | `onboot.sh` → `/data/quasar/rootkeeper.sh` | same (line 67); `q stop` available |

Toolchain: zig 0.13 C++ release builds for `arm-linux-gnueabihf.2.30` crash before `main` (debug
builds and C work); zig 0.14.1 builds run. A plum port needs zig ≥ 0.14.1 for both targets.

Not yet verified on plum: mic channel mapping under speech (the hardware mic mute was on during the
study, so capture contained no usable signal), GStreamer playback through the 32-bit
`libquasar_gstreamer.so`, Sendspin on armhf, memory headroom with the Yandex stack stopped.

## Conventions

- C++20, single-threaded controller: worker threads (capture, GStreamer, Sendspin) hand results to
  the loop with `Loop::post`.
- Keep the ESPHome API server pending until a valid `HelloRequest`; probes must not displace HA.
- State persisted in `state.conf` (key=value); add new keys with safe defaults.
- Comments explain device facts and constraints, not what the code does.

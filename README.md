# SoundSpin

A [Sendspin](https://github.com/Sendspin/sendspin-cpp) client for Bose SoundTouch
speakers, so they can be used as synchronised players in
[Music Assistant](https://music-assistant.io/).

Audio output goes through [libshelbourne](https://github.com/davidfreina/libshelbourne),
which drives the SoundTouch codec, amplifier and McASP DMA path directly.

Tested on:

| Model | Variant | Module |
|---|---|---|
| SoundTouch 10 | `rhino` | `sm2` |
| SoundTouch 20 | `spotty` | `sm2` |

## Important: the DAC runs at 46875 Hz, not 48 kHz

The McASP TX bit clock is a round 3.000 MHz (24 MHz / 8). At 2 channels ×
32 bit that is `3000000 / 64 = 46875` frames/s — **2.34 % below the nominal
48 kHz**. This was measured repeatably at 46875.5 Hz (±0.15 Hz over 10 s) on
both devices.

SoundSpin therefore advertises **46875 Hz** to the server and lets Music
Assistant resample once, server-side.

If you instead advertise 48 kHz, the speaker drains ~2.34 % slower than the
server produces. Buffers grow by roughly 23 ms every second and Sendspin's sync
task compensates forever — inserting silence (heard as *crackle*) and
periodically dropping frames (heard as brief *speed-ups*). It looks exactly like
a buffering or latency bug, but no amount of buffer, delay or threshold tuning
fixes it, because it is a **rate** error rather than an offset.

`Opus` is deliberately not advertised: it only supports 48/24/16/12/8 kHz, so it
cannot carry the native rate. FLAC and PCM are used instead.

You can re-measure the rate on other hardware with `latencytest` in
libshelbourne.

## Building

Builds are cross-compiled for ARM Cortex-A8 inside Docker, so the only host
requirements are Docker and Make:

```bash
make build
```

The result is a statically linked binary at `build/soundspin`.

libshelbourne is fetched by CMake at the SHA pinned in `CMakeLists.txt` — the
local `libshelbourne/` directory (if present) is a separate working checkout and
is **not** used by the build. To build against local changes:

```bash
cmake -B build -DFETCHCONTENT_SOURCE_DIR_LIBSHELBOURNE=/work/libshelbourne ...
```

To make them permanent, push libshelbourne and bump `GIT_TAG`.

## Deploying

```bash
make deploy DEVICE=<device-ip>
```

This copies the binary to `/tmp/` on the device. Note that `/tmp` is a tmpfs and
is **wiped on reboot** — for a permanent install, place the binary and config on
persistent storage and start it from the shepherd override (see
libshelbourne's `docs/autoboot.md`).

SSH to these devices needs legacy algorithms:

```bash
ssh -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa root@<device-ip>
```

## Configuration

Copy `soundspin.conf` next to the binary and edit it:

```ini
server_url     = ws://homeassistant.lan:8927/sendspin
player_name    = SoundSpin
log_level      = info      # none, error, warn, info, debug, verbose
initial_volume = 10        # 0-100
```

Run it with:

```bash
./soundspin -c soundspin.conf
```

SoundSpin takes over the audio hardware, so it stops the stock Bose processes
(`BoseApp`, `APServer`, …) and freezes the `shepherdd` supervisor while running.

`files/` contains shepherd overrides that stop those processes from starting at
boot in the first place.

## Notes on the hardware

- The kernel DMA buffer is only about one 512-frame chunk (~10.7 ms) deep, so
  the playback thread runs at `SCHED_FIFO` to avoid underruns.
- On `rhino`, the stock codec firmware (`rhino_sm2_normal.cfg`) loads the
  miniDSP's input-mixer volume coefficient as **zero**, which mutes everything
  inside the DSP even though the codec, DAC and amplifier are configured
  correctly. libshelbourne writes the stock coefficient after codec init to
  restore it.

## License

MIT — see [LICENSE](LICENSE).

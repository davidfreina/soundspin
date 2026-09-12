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

SSH to these devices needs legacy algorithms:

```bash
ssh -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa root@<device-ip>
```

### Temporary (for testing)

```bash
make deploy DEVICE=<device-ip>
```

Copies the binary to `/tmp/`, which is a tmpfs and is **wiped on reboot**. Run
it by hand with `./soundspin -c soundspin.conf`.

### Permanent (auto-boot)

```bash
make install DEVICE=<device-ip>
ssh ... root@<device-ip> reboot
```

This puts the binary and config on NAND (`/mnt/nv/`) and installs a shepherd
override so the device's own supervisor starts soundspin at boot and restarts it
if it crashes. **The stock Bose software no longer runs** — soundspin owns the
audio hardware.

The config is only seeded if `/mnt/nv/soundspin.conf` does not already exist, so
re-installing never clobbers your settings. On a first install, edit
`server_url` before rebooting.

Revert to stock at any time:

```bash
make uninstall DEVICE=<device-ip>
ssh ... root@<device-ip> reboot
```

### Safety net

`make install` also installs `files/netwatch.sh` as a shepherd daemon. Two
minutes after boot it pings the default gateway; if the network is unreachable
it restores a networking-only fallback config and reboots. Without this, a build
that breaks networking at boot leaves the device reachable only via physical
access (USB gadget, serial console, or factory reset).

Two hazards this deployment is built around — both cost real debugging time:

- **`shelbourne_stop_stock_processes()` must not run under the override.** Its
  list includes `NetManager`, which the override itself runs, and it also
  SIGSTOPs `shepherdd`. At boot that kills networking before Wi-Fi associates
  and freezes the supervisor so it can neither restart NetManager nor honour
  `recovery="reboot"`. The sink therefore skips it when `/mnt/nv/shepherd`
  exists.
- **Do not "fix" the Firewall args to match rhino's stock config.** Stock rhino
  uses `-4i wlan0`; applying that ruleset to the live wireless interface locks
  the device off the network completely. Keep `-4i eth0 -i wlan0` (rhino has no
  `eth0`, so wlan0 stays reachable).

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

When run by hand on a stock device, SoundSpin takes over the audio hardware by
stopping the stock Bose processes (`BoseApp`, `APServer`, …) and freezing the
`shepherdd` supervisor for as long as it runs. Under the auto-boot override
those processes never start, so nothing needs stopping.

### Identity

Each player identifies itself to the server with a `client_id` derived from the
Wi-Fi MAC address, e.g. `soundspin-a81b6a65b244`. That is the only stable
hardware identity on these devices: there is no `machine-id` (no systemd), no
serial number in the device tree, and the hostname is just the model variant
(`rhino`/`spotty`), which would collide between two units of the same model.
The "Bose SoundTouch XXXXXX" name shown over SSH is BoseApp state in a
user-editable, factory-reset-able file, not a hardware serial.

`player_name` is sent on every connection, but Music Assistant stores a
player's name when it first sees a `client_id` and does not rename it on later
connections. To change the name, rename the player in Music Assistant — or
remove it there and let it re-register.

The device info reports manufacturer `Bose` and the model (`SoundTouch 10` /
`SoundTouch 20`) from `/proc/variant`.

### Encryption

Music Assistant may warn that the player is *"connected without encryption
(legacy mode). Pairing not available."* This is expected: no release of
[sendspin-cpp](https://github.com/Sendspin/sendspin-cpp) (through v0.7.2 at the
time of writing) implements the encrypted/paired transport, so it is not
something SoundSpin can enable. Playback is unaffected; the connection is plain
WebSocket on your LAN.

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

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

### Firewall

The override runs Bose's own firewall: inbound connections are accepted only
from the speaker's own subnet (replies to its outbound connections, e.g. to
Music Assistant, always get through). Since SSH on these devices is
**passwordless root**, keep it on.

If you manage the speakers from another subnet or VLAN, allow it explicitly in
`soundspin.conf`:

```ini
firewall_allow = 10.0.20.0/24 192.168.88.0/23
```

`files/firewall.sh` applies Bose's rules unchanged and then accepts these
subnets. `make install` fills in the interfaces the device actually has
(`eth0` and/or `wlan0`): the Firewall daemon exits if told to watch a missing
interface, and shepherdd then restarts it in a tight loop — on a SoundTouch 10,
which has no `eth0`, that loop alone used most of the CPU.

### Hazard: never stop "stock" processes under the override

`shelbourne_stop_stock_processes()` must not run under the override. Its list
includes `NetManager`, which the override itself runs, and it also SIGSTOPs
`shepherdd`. At boot that kills networking before Wi-Fi associates and freezes
the supervisor so it can neither restart NetManager nor honour
`recovery="reboot"`, leaving the device unreachable. The sink therefore skips it
when `/mnt/nv/shepherd` exists.

### Changing the Wi-Fi network

NetManager keeps its networks in
`/mnt/nv/BoseApp-Persistence/1/NetworkProfiles.xml`. Add one over SSH while the
speaker is still on the current network:

```sh
cd /mnt/nv/BoseApp-Persistence/1
cp NetworkProfiles.xml NetworkProfiles.xml.bak
sed -i 's|</WiFiProfiles>|<profile SSID="NEW_SSID" security="wpa_or_wpa2" passphrase="NEW_PASSWORD" /></WiFiProfiles>|' NetworkProfiles.xml
sync; reboot
```

NetManager encrypts the passphrase itself on the next boot. It also
**rewrites the `priority` values** and, with `--autoswitching`, moves to whichever
known network it rates best (5 GHz beat 2.4 GHz on the same access point in
testing). So to force a network, delete the other profiles rather than editing
priorities.

Bose's offline path also works under the override: a FAT32 stick with
`network.txt` (SSID on line 1, passphrase on line 2) in the rear SERVICE
micro-USB port. It **replaces** all known networks and reboots.

If the new network is a different subnet, check `server_url` still routes to
Music Assistant and that your management subnet is in `firewall_allow`.

## Configuration

Copy `soundspin.conf` next to the binary and edit it:

```ini
server_url     = ws://homeassistant.lan:8927/sendspin
player_name    = SoundSpin
log_level      = info      # none, error, warn, info, debug, verbose
initial_volume = 10        # 0-100
```

Trailing `# comments` are allowed after a value.

Run it with:

```bash
./soundspin -c soundspin.conf
```

When run by hand on a stock device, SoundSpin takes over the audio hardware by
stopping the stock Bose processes (`BoseApp`, `APServer`, …) and freezing the
`shepherdd` supervisor for as long as it runs. Under the auto-boot override
those processes never start, so nothing needs stopping.

### Buttons

The speaker's own buttons work. Every key is configurable with a short-press
and an optional long-press action:

```ini
button_power      = play_pause   # short press
button_power_long = next         # held >= 0.8 s
button_preset_6   = stop
volume_step       = 1            # 1-25
```

Keys: `volume_up`, `volume_down`, `power`, `aux`, `preset_1` … `preset_6`.

Actions: `volume_up`, `volume_down`, `mute` (toggle), `play_pause` (toggle),
`play`, `pause`, `stop`, `next`, `previous`, `shuffle` (toggle), `repeat`
(cycles off → all → one), `switch`, `none`.

| Key | Default |
|---|---|
| Vol + / Vol − | `volume_up` / `volume_down`, ±1 per step (auto-repeat while held) |
| Power | `play_pause` |
| AUX | `mute` |
| Preset 1 / 2 / 3 | `previous` / `play_pause` / `next` |
| Preset 4 / 5 | `shuffle` / `repeat` |
| Preset 6 | `none` |

Volume and mute act on **this speaker** (Music Assistant's slider follows);
transport actions are sent to the server and act on the **whole group**.

A key without a long-press action fires the moment it is pressed. A key *with*
one fires its short action on release instead, since it can't know earlier
whether you'll keep holding — and volume keys stop auto-repeating.

Presets cannot start a favourite playlist or station: Sendspin's controller
commands are transport-only, with no way to select content.

### Logging

Under auto-boot the daemon's output is otherwise invisible, because shepherdd
discards it. Set a log file to see it:

```ini
log_file = /tmp/soundspin.log   # truncated on each start; keep it on tmpfs
```

Every button press is logged, which also makes it easy to check the key mapping
on a model that hasn't been tested yet.

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

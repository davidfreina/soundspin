#include "sendspin/client.h"
#include "sendspin/player_role.h"
#include "sendspin/controller_role.h"
#include "sendspin/metadata_role.h"

#include "libshelbourne_sink.h"
#include "buttons.h"
#include "config.h"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <getopt.h>
#include <string>
#include <thread>
#include <unistd.h>

using namespace sendspin;

static constexpr const char* DEFAULT_CONFIG_PATH = "/etc/soundspin.conf";
static constexpr const char* VERSION = "0.1.0";

static std::atomic<bool> running{true};

static void signal_handler(int /*sig*/) { running.store(false); }

static bool parse_log_level(const std::string& str, LogLevel& level) {
    if (str == "none") { level = LogLevel::NONE; return true; }
    if (str == "error") { level = LogLevel::ERROR; return true; }
    if (str == "warn") { level = LogLevel::WARN; return true; }
    if (str == "info") { level = LogLevel::INFO; return true; }
    if (str == "debug") { level = LogLevel::DEBUG; return true; }
    if (str == "verbose") { level = LogLevel::VERBOSE; return true; }
    return false;
}

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s [options]\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -c PATH   Config file (default: %s)\n", DEFAULT_CONFIG_PATH);
    fprintf(stderr, "  -h        Show this help\n");
    fprintf(stderr, "  -V        Show version\n");
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Parse command line
    std::string config_path = DEFAULT_CONFIG_PATH;
    int opt;
    while ((opt = getopt(argc, argv, "c:hV")) != -1) {
        switch (opt) {
            case 'c': config_path = optarg; break;
            case 'h': print_usage(argv[0]); return 0;
            case 'V': fprintf(stdout, "soundspin %s\n", VERSION); return 0;
            default: print_usage(argv[0]); return 1;
        }
    }

    // Load configuration
    Config cfg;
    if (!load_config(config_path, cfg)) {
        return 1;
    }
    if (cfg.server_url.empty()) {
        fprintf(stderr, "Error: server_url is required in %s\n", config_path.c_str());
        return 1;
    }

    // Optional log file. Under the shepherd auto-boot override stderr goes
    // nowhere, so without this a running speaker's logs are invisible.
    // Truncated on each start; keep it on tmpfs (/tmp) to spare the NAND.
    if (!cfg.log_file.empty()) {
        int fd = open(cfg.log_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "Warning: cannot open log_file %s: %s\n",
                    cfg.log_file.c_str(), strerror(errno));
        } else {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
            setvbuf(stdout, nullptr, _IOLBF, 0);
        }
    }

    // Apply log level
    LogLevel log_level = LogLevel::INFO;
    if (!parse_log_level(cfg.log_level, log_level)) {
        fprintf(stderr, "Warning: unknown log level '%s', using 'info'\n",
                cfg.log_level.c_str());
    }
    SendspinClient::set_log_level(log_level);

    // Derive a unique, persistent client_id. The spec requires this to be
    // unique per device so servers can track group membership and de-duplicate
    // connections; Sendspin's own guidance is to use the MAC address.
    //
    // The Wi-Fi MAC is the only stable hardware identity on these devices:
    // /etc/machine-id is empty (no systemd), there is no serial number in the
    // device tree, and the hostname is just the variant ("rhino"/"spotty") --
    // identical across every unit of that model, so two of the same model
    // would collide. The "Bose SoundTouch XXXXXX" name is BoseApp state in a
    // user-editable, factory-reset-able file, not a hardware serial.
    std::string client_id;
    {
        std::ifstream mac("/sys/class/net/wlan0/address");
        std::string line;
        if (std::getline(mac, line)) {
            for (char c : line) {
                if (std::isxdigit(static_cast<unsigned char>(c)))
                    client_id += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
    }
    if (client_id.empty()) {
        char hbuf[256] = {};
        if (gethostname(hbuf, sizeof(hbuf)) == 0 && hbuf[0] != '\0') {
            client_id = hbuf;
        } else {
            client_id = "unknown";
        }
        fprintf(stderr, "Warning: no wlan0 MAC; client_id falls back to '%s' "
                        "(not unique across units of one model)\n", client_id.c_str());
    }

    // Model name from the hardware variant, for the server's device info.
    std::string product_name = "SoundTouch";
    {
        std::ifstream v("/proc/variant");
        std::string variant;
        if (std::getline(v, variant)) {
            if (variant == "rhino")       product_name = "SoundTouch 10";
            else if (variant == "spotty") product_name = "SoundTouch 20";
            else if (!variant.empty())    product_name = "SoundTouch (" + variant + ")";
        }
    }

    // Configure client
    SendspinClientConfig client_config;
    client_config.client_id = "soundspin-" + client_id;
    client_config.name = cfg.player_name;
    client_config.product_name = product_name;
    client_config.manufacturer = "Bose";
    client_config.software_version = VERSION;
    fprintf(stderr, "Identity: client_id=%s name=\"%s\" product=\"%s\"\n",
            client_config.client_id.c_str(), client_config.name.c_str(),
            client_config.product_name.c_str());

    SendspinClient client(std::move(client_config));

    // Add player role with supported audio formats
    PlayerRoleConfig player_config;
    // Advertise the DAC's true rate (see ShelbourneSink::HW_SAMPLE_RATE), so the
    // server resamples once instead of us drifting 2.34% against it forever.
    // Opus is omitted deliberately: it only supports 48/24/16/12/8 kHz, so it
    // cannot carry the native rate and would reintroduce the mismatch.
    player_config.audio_formats = {
        {SendspinCodecFormat::FLAC, 2, ShelbourneSink::HW_SAMPLE_RATE, 16},
        {SendspinCodecFormat::PCM,  2, ShelbourneSink::HW_SAMPLE_RATE, 16},
    };
    player_config.audio_buffer_capacity = 1000000;
    // Must equal ShelbourneSink::PIPELINE_DELAY_US: both represent the same quantity
    // (DMA write -> audible at speaker), so the sync task and playback thread
    // report timestamps in the same frame of reference.
    player_config.fixed_delay_us = ShelbourneSink::PIPELINE_DELAY_US;
    player_config.initial_static_delay_ms = 0;
    
    auto& player = client.add_player(std::move(player_config));
    auto& controller = client.add_controller();
    auto& metadata = client.add_metadata();

    ShelbourneSink audio_sink;

    // SoundSpin Listener implementations

    struct SoundSpinPlayerListener : PlayerRoleListener {
        ShelbourneSink& sink;
        PlayerRole& player;
        SoundSpinPlayerListener(ShelbourneSink& sink, PlayerRole& player) : sink(sink), player(player) {}

        size_t on_audio_write(uint8_t* data, size_t length, uint32_t timeout_ms) override {
            return sink.write(data, length, timeout_ms);
        }

        void on_stream_start() override {
            auto& params = player.get_current_stream_params();
            if (!params.is_complete()) {
                fprintf(stderr, "ShelbourneSink: stream/start missing required fields, ignoring\n");
                return;
            }
            if (!sink.configure(*params.sample_rate, *params.channels, *params.bit_depth)) {
                fprintf(stderr, "ShelbourneSink: configure() rejected stream parameters\n");
            }
        }

        void on_stream_end() override {
            sink.clear();
        }

        void on_volume_changed(uint8_t vol) override {
            sink.set_volume(vol);
        }
        
        void on_mute_changed(bool muted) override {
            sink.set_muted(muted);
        }

        void on_static_delay_changed(uint16_t delay_ms) override {
            player.update_static_delay(delay_ms);
        }
    };

    struct SoundSpinClientListener : SendspinClientListener {
        void on_time_sync_updated(float error) override {
            if (SendspinClient::get_log_level() >= LogLevel::DEBUG) {
                // TODO: add useful logging here
                //log_sync_quality(error);
            }
        }
    };

    struct HostNetworkProvider : SendspinNetworkProvider {
        bool is_network_ready() override { 
            // TODO: check actual network status
            return true;
        }
    };

    SoundSpinPlayerListener player_listener(audio_sink, player);
    audio_sink.on_frames_played = [&player](uint32_t frames, int64_t timestamp) {
        player.notify_audio_played(frames, timestamp);
    };

    SoundSpinClientListener client_listener;
    HostNetworkProvider network_provider;

    player.set_listener(&player_listener);
    client.set_listener(&client_listener);
    client.set_network_provider(&network_provider);

    if (!client.start_server()) {
        fprintf(stderr, "Failed to start server\n");
        return 1;
    }

    // Apply the configured startup volume before the first connection so the
    // hardware is at the expected level even before the server issues a
    // VOLUME command.  Without this the sink defaults to 100 while the
    // SDK reports 0 until the server speaks.
    if (cfg.initial_volume >= 0) {
        auto vol = static_cast<uint8_t>(cfg.initial_volume);
        audio_sink.set_volume(vol);
        player.update_volume(vol);
        fprintf(stderr, "Initial volume set to %u\n", vol);
    }

    // Hardware buttons. Volume and mute act on this player (so the speaker's
    // own buttons change this speaker, and Music Assistant's slider follows);
    // transport actions go to the server via the controller role and so act on
    // the whole group.
    const uint8_t volume_step = static_cast<uint8_t>(cfg.volume_step);
    ButtonHandler buttons(cfg.buttons, [&](ButtonHandler::Action action, const char* key) {
        using A = ButtonHandler::Action;
        using C = SendspinControllerCommand;
        const auto& cs = controller.get_controller_state();
        auto supported = [&](C c) {
            for (auto s : cs.supported_commands) if (s == c) return true;
            return false;
        };

        switch (action) {
            case A::VOLUME_UP:
            case A::VOLUME_DOWN: {
                int v = player.get_volume();
                v += (action == A::VOLUME_UP) ? volume_step : -volume_step;
                v = v < 0 ? 0 : (v > 100 ? 100 : v);
                if (player.get_muted()) {  // touching volume un-mutes, as on most speakers
                    audio_sink.set_muted(false);
                    player.update_muted(false);
                }
                audio_sink.set_volume(static_cast<uint8_t>(v));
                player.update_volume(static_cast<uint8_t>(v));
                fprintf(stderr, "Buttons: %s -> volume %d\n", key, v);
                return;
            }
            case A::MUTE: {
                const bool m = !player.get_muted();
                audio_sink.set_muted(m);
                player.update_muted(m);
                fprintf(stderr, "Buttons: %s -> %s\n", key, m ? "muted" : "unmuted");
                return;
            }
            default:
                break;
        }

        if (!client.is_connected()) {
            fprintf(stderr, "Buttons: %s -> %s ignored (not connected)\n",
                    key, ButtonHandler::action_name(action));
            return;
        }

        C cmd;
        switch (action) {
            case A::PLAY_PAUSE: {
                const auto& st = client.get_group_state().playback_state;
                const bool playing = st.has_value() && *st == SendspinPlaybackState::PLAYING;
                cmd = !playing ? C::PLAY : (supported(C::PAUSE) ? C::PAUSE : C::STOP);
                break;
            }
            case A::PLAY:     cmd = C::PLAY; break;
            case A::PAUSE:    cmd = C::PAUSE; break;
            case A::STOP:     cmd = C::STOP; break;
            case A::NEXT:     cmd = C::NEXT; break;
            case A::PREVIOUS: cmd = C::PREVIOUS; break;
            case A::SWITCH:   cmd = C::SWITCH; break;
            case A::SHUFFLE:  cmd = cs.shuffle ? C::UNSHUFFLE : C::SHUFFLE; break;
            case A::REPEAT:
                cmd = cs.repeat == SendspinRepeatMode::OFF ? C::REPEAT_ALL
                    : cs.repeat == SendspinRepeatMode::ALL ? C::REPEAT_ONE
                                                           : C::REPEAT_OFF;
                break;
            default:
                return;
        }
        if (!cs.supported_commands.empty() && !supported(cmd)) {
            fprintf(stderr, "Buttons: %s -> %s not supported by server right now\n",
                    key, ButtonHandler::action_name(action));
            return;
        }
        fprintf(stderr, "Buttons: %s -> %s\n", key, ButtonHandler::action_name(action));
        controller.send_command(cmd);
    });


    using clock = std::chrono::steady_clock;
    static constexpr auto RECONNECT_INTERVAL = std::chrono::seconds(5);
    auto last_connect_attempt = clock::now() - RECONNECT_INTERVAL;

    while (running.load()) {
        client.loop();
        buttons.poll(audio_sink.hardware());

        if (!client.is_connected()) {
            auto now = clock::now();
            if (now - last_connect_attempt >= RECONNECT_INTERVAL) {
                fprintf(stderr, "Connecting to %s...\n", cfg.server_url.c_str());
                client.connect_to(cfg.server_url);
                last_connect_attempt = now;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    fprintf(stderr, "Shutting down...\n");
    client.disconnect(SendspinGoodbyeReason::SHUTDOWN);
    return 0;
}

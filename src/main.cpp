#include "sendspin/client.h"
#include "sendspin/player_role.h"
#include "sendspin/controller_role.h"
#include "sendspin/metadata_role.h"

#include "libshelbourne_sink.h"
#include "config.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
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

    // Apply log level
    LogLevel log_level = LogLevel::INFO;
    if (!parse_log_level(cfg.log_level, log_level)) {
        fprintf(stderr, "Warning: unknown log level '%s', using 'info'\n",
                cfg.log_level.c_str());
    }
    SendspinClient::set_log_level(log_level);

    // Derive a unique, persistent client_id from /etc/machine-id (systemd).
    // Falls back to hostname.  The spec requires this to be unique per device
    // so that servers can track group membership and de-duplicate connections.
    std::string client_id;
    {
        std::ifstream mid("/etc/machine-id");
        std::getline(mid, client_id);
        client_id.erase(0, client_id.find_first_not_of(" \t\r\n"));
        client_id.erase(client_id.find_last_not_of(" \t\r\n") + 1);
    }
    if (client_id.empty()) {
        char hbuf[256] = {};
        if (gethostname(hbuf, sizeof(hbuf)) == 0 && hbuf[0] != '\0') {
            client_id = hbuf;
        } else {
            client_id = "soundspin";
        }
    }

    // Configure client
    SendspinClientConfig client_config;
    client_config.client_id = "soundspin-" + client_id;
    client_config.name = cfg.player_name;
    client_config.product_name = "soundspin";
    client_config.manufacturer = "soundspin";
    client_config.software_version = VERSION;

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


    using clock = std::chrono::steady_clock;
    static constexpr auto RECONNECT_INTERVAL = std::chrono::seconds(5);
    auto last_connect_attempt = clock::now() - RECONNECT_INTERVAL;

    while (running.load()) {
        client.loop();

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

#pragma once

#include <map>
#include <string>

/// Configuration loaded from soundspin.conf
struct Config {
    std::string server_url;   // e.g. ws://192.168.1.10:8928/sendspin
    std::string player_name = "soundspin";
    std::string log_level = "info";
    std::string log_file;     // empty = stderr; otherwise stdout+stderr are redirected here
    int initial_volume = -1;  // 0-100 to override hardware volume on startup; -1 = server default
    int volume_step = 5;      // 1-25, volume change per button press / repeat

    /// Raw button bindings: "button_<key>" and "button_<key>_long" -> action name.
    /// Validated by ButtonHandler, which knows the key and action vocabulary.
    std::map<std::string, std::string> buttons;
};

/// Parse a simple key=value config file.
/// Returns false and prints an error if the file cannot be read.
bool load_config(const std::string& path, Config& config);

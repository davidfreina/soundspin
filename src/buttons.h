#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <string>

struct shelbourne;

/// Turns SoundTouch keypad presses into configurable actions.
///
/// Each key has a short-press action and an optional long-press action:
///
///   - No long action: the short action fires on press (no added latency).
///     Volume actions additionally auto-repeat while the key is held.
///   - With a long action: the short action fires on release if the key was
///     held less than LONG_PRESS; the long action fires as soon as the key has
///     been held for LONG_PRESS (and the release is then ignored).
///
/// The handler only decides *which* action happened; the caller decides what
/// it means, so this knows nothing about Sendspin.
class ButtonHandler {
  public:
    enum class Action {
        NONE,
        VOLUME_UP,
        VOLUME_DOWN,
        MUTE,        // toggle
        PLAY_PAUSE,  // toggle
        PLAY,
        PAUSE,
        STOP,
        NEXT,
        PREVIOUS,
        SHUFFLE,     // toggle
        REPEAT,      // cycle off -> all -> one -> off
        SWITCH,
    };

    static constexpr std::chrono::milliseconds LONG_PRESS{800};
    static constexpr std::chrono::milliseconds REPEAT_DELAY{400};
    static constexpr std::chrono::milliseconds REPEAT_INTERVAL{150};

    /// Builds the default bindings, then applies `config` ("<key>" or
    /// "<key>_long" -> action name). Unknown keys or actions are reported on
    /// stderr and ignored.
    ButtonHandler(const std::map<std::string, std::string>& config,
                  std::function<void(Action, const char* key)> run);

    /// Reads pending keypad events and fires actions; call every loop
    /// iteration (~10 ms). No-op if the keypad is unavailable.
    void poll(struct shelbourne* hw);

    static const char* action_name(Action a);

  private:
    static constexpr int NUM_KEYS = 10;

    struct Binding {
        Action short_press = Action::NONE;
        Action long_press = Action::NONE;
    };
    struct KeyState {
        bool down = false;
        bool long_fired = false;
        std::chrono::steady_clock::time_point since{};
        std::chrono::steady_clock::time_point next_repeat{};
    };

    void on_press(int key, std::chrono::steady_clock::time_point now);
    void on_release(int key);
    void on_hold(int key, std::chrono::steady_clock::time_point now);
    void fire(int key, Action a);

    Binding bindings_[NUM_KEYS];
    KeyState state_[NUM_KEYS];
    std::function<void(Action, const char*)> run_;
    bool warned_unavailable_ = false;
};

#include "buttons.h"

extern "C" {
    #include "shelbourne.h"
}

#include <cstdio>
#include <cstring>

namespace {

using Action = ButtonHandler::Action;

// Indexed by shelbourne_key_t (0..9). Names are the config keys
// ("button_<name>" / "button_<name>_long").
const char* const KEY_NAMES[] = {
    "volume_up", "volume_down", "power", "aux",
    "preset_1", "preset_2", "preset_3", "preset_4", "preset_5", "preset_6",
};
static_assert(sizeof(KEY_NAMES) / sizeof(KEY_NAMES[0]) == SHELBOURNE_KEY_UNKNOWN,
              "KEY_NAMES must match shelbourne_key_t");

struct ActionName { Action action; const char* name; };
const ActionName ACTIONS[] = {
    {Action::NONE, "none"},
    {Action::VOLUME_UP, "volume_up"},
    {Action::VOLUME_DOWN, "volume_down"},
    {Action::MUTE, "mute"},
    {Action::PLAY_PAUSE, "play_pause"},
    {Action::PLAY, "play"},
    {Action::PAUSE, "pause"},
    {Action::STOP, "stop"},
    {Action::NEXT, "next"},
    {Action::PREVIOUS, "previous"},
    {Action::SHUFFLE, "shuffle"},
    {Action::REPEAT, "repeat"},
    {Action::SWITCH, "switch"},
};

bool parse_action(const std::string& s, Action& out) {
    for (const auto& a : ACTIONS) {
        if (s == a.name) { out = a.action; return true; }
    }
    return false;
}

int find_key(const std::string& s) {
    for (int i = 0; i < SHELBOURNE_KEY_UNKNOWN; ++i) {
        if (s == KEY_NAMES[i]) return i;
    }
    return -1;
}

bool repeats(Action a) { return a == Action::VOLUME_UP || a == Action::VOLUME_DOWN; }

}  // namespace

const char* ButtonHandler::action_name(Action a) {
    for (const auto& n : ACTIONS) {
        if (n.action == a) return n.name;
    }
    return "?";
}

ButtonHandler::ButtonHandler(const std::map<std::string, std::string>& config,
                             std::function<void(Action, const char*)> run)
    : run_(std::move(run)) {
    // Defaults. Presets cannot mean "play favourite N": Sendspin has no
    // command for selecting content, only for transport control.
    bindings_[SHELBOURNE_KEY_VOLUME_UP]   = {Action::VOLUME_UP, Action::NONE};
    bindings_[SHELBOURNE_KEY_VOLUME_DOWN] = {Action::VOLUME_DOWN, Action::NONE};
    bindings_[SHELBOURNE_KEY_POWER]       = {Action::PLAY_PAUSE, Action::NONE};
    bindings_[SHELBOURNE_KEY_AUX]         = {Action::MUTE, Action::NONE};
    bindings_[SHELBOURNE_KEY_PRESET_1]    = {Action::PREVIOUS, Action::NONE};
    bindings_[SHELBOURNE_KEY_PRESET_2]    = {Action::PLAY_PAUSE, Action::NONE};
    bindings_[SHELBOURNE_KEY_PRESET_3]    = {Action::NEXT, Action::NONE};
    bindings_[SHELBOURNE_KEY_PRESET_4]    = {Action::SHUFFLE, Action::NONE};
    bindings_[SHELBOURNE_KEY_PRESET_5]    = {Action::REPEAT, Action::NONE};
    bindings_[SHELBOURNE_KEY_PRESET_6]    = {Action::NONE, Action::NONE};

    for (const auto& [name, value] : config) {
        std::string key = name;
        bool is_long = false;
        if (key.size() > 5 && key.compare(key.size() - 5, 5, "_long") == 0) {
            key.resize(key.size() - 5);
            is_long = true;
        }
        int k = find_key(key);
        if (k < 0) {
            fprintf(stderr, "Buttons: unknown key 'button_%s' (valid: volume_up, "
                            "volume_down, power, aux, preset_1..preset_6)\n", name.c_str());
            continue;
        }
        Action a;
        if (!parse_action(value, a)) {
            fprintf(stderr, "Buttons: unknown action '%s' for button_%s\n",
                    value.c_str(), name.c_str());
            continue;
        }
        (is_long ? bindings_[k].long_press : bindings_[k].short_press) = a;
    }

    for (int k = 0; k < NUM_KEYS; ++k) {
        if (bindings_[k].long_press != Action::NONE && repeats(bindings_[k].short_press)) {
            fprintf(stderr, "Buttons: %s has a long-press action, so its %s will not "
                            "auto-repeat\n", KEY_NAMES[k], action_name(bindings_[k].short_press));
        }
    }
}

void ButtonHandler::fire(int key, Action a) {
    if (a == Action::NONE) return;
    run_(a, KEY_NAMES[key]);
}

void ButtonHandler::on_press(int key, std::chrono::steady_clock::time_point now) {
    KeyState& s = state_[key];
    const Binding& b = bindings_[key];
    s.down = true;
    s.long_fired = false;
    s.since = now;
    s.next_repeat = now + REPEAT_DELAY;
    if (b.long_press == Action::NONE) {
        fire(key, b.short_press);  // nothing to disambiguate: act immediately
    }
}

void ButtonHandler::on_release(int key) {
    KeyState& s = state_[key];
    const Binding& b = bindings_[key];
    if (s.down && b.long_press != Action::NONE && !s.long_fired) {
        fire(key, b.short_press);
    }
    s.down = false;
}

void ButtonHandler::on_hold(int key, std::chrono::steady_clock::time_point now) {
    KeyState& s = state_[key];
    const Binding& b = bindings_[key];
    if (!s.down) return;
    if (b.long_press != Action::NONE) {
        if (!s.long_fired && now - s.since >= LONG_PRESS) {
            s.long_fired = true;
            fire(key, b.long_press);
        }
    } else if (repeats(b.short_press) && now >= s.next_repeat) {
        s.next_repeat = now + REPEAT_INTERVAL;
        fire(key, b.short_press);
    }
}

void ButtonHandler::poll(struct shelbourne* hw) {
    if (!hw) return;
    shelbourne_key_event_t events[16];
    int n = shelbourne_keypad_poll(hw, events, 16);
    if (n < 0) {
        if (!warned_unavailable_) {
            fprintf(stderr, "Buttons: keypad unavailable, buttons disabled\n");
            warned_unavailable_ = true;
        }
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        const int k = events[i].key;
        if (k < 0 || k >= NUM_KEYS) {  // SHELBOURNE_KEY_UNKNOWN
            if (events[i].pressed) fprintf(stderr, "Buttons: unrecognised key pressed\n");
            continue;
        }
        if (events[i].pressed) {
            // Human-rate, so cheap to always log; makes the keypad mapping on a
            // new model easy to verify.
            fprintf(stderr, "Buttons: %s pressed\n", KEY_NAMES[k]);
            on_press(k, now);
        } else {
            on_release(k);
        }
    }
    for (int k = 0; k < NUM_KEYS; ++k) on_hold(k, now);
}

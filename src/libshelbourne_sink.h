#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct shelbourne;  // libshelbourne handle (opaque)

/// Audio sink that writes to the Bose SoundTouch snd_shelby DMA via
/// libshelbourne. Drop-in replacement for AlsaPipeSink — same ring-buffered
/// producer/consumer split, same listener API.
///
/// Hardware is fixed at HW_SAMPLE_RATE (46875 Hz) / 32-bit / stereo. Input PCM
/// may be 16, 24 or 32 bit stereo at that rate; other formats are rejected at
/// configure() time. Advertise only HW_SAMPLE_RATE formats to the server.
class ShelbourneSink {
  public:
    /// Approximate hardware pipeline delay (DMA + codec + amp).
    /// Sendspin's Kalman filter absorbs steady offsets, so an honest
    /// estimate is sufficient — exact calibration is not required.
    static constexpr int32_t PIPELINE_DELAY_US = 15000;

    /// True DAC rate of the SoundTouch McASP TX path.
    ///
    /// The McASP bit clock is a round 3.000 MHz (24 MHz / 8); at 2ch x 32-bit
    /// that is 3000000/64 = 46875 frames/s, NOT the nominal 48000. Measured
    /// repeatably at 46875.5 Hz (+-0.15 Hz over 10 s).
    ///
    /// Advertising 48000 while the DAC drains 46875 makes the client consume
    /// 2.34% slower than the server sends, so the buffer grows ~23 ms/s and
    /// Sendspin's sync task has to keep dumping frames (audible glitches).
    /// Advertise the native rate instead and let the server resample.
    static constexpr uint32_t HW_SAMPLE_RATE = 46875;

    ShelbourneSink();
    ~ShelbourneSink();

    ShelbourneSink(const ShelbourneSink&) = delete;
    ShelbourneSink& operator=(const ShelbourneSink&) = delete;

    bool configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);
    size_t write(uint8_t* data, size_t length, uint32_t timeout_ms);
    void stop();
    void clear();
    void set_volume(uint8_t volume);
    void set_muted(bool muted);
    void set_device(const std::string& device);  // no-op (API compat)

    std::function<void(uint32_t frames, int64_t timestamp)> on_frames_played;

    /// libshelbourne handle, for other subsystems on the same device (keypad).
    /// nullptr if hardware init failed. Keypad polling is safe to interleave
    /// with the playback thread's audio writes (separate fd).
    struct shelbourne* hardware() const { return static_cast<struct shelbourne*>(hw_); }

  private:
    void* hw_{nullptr};  // shelbourne_t*

    uint32_t sample_rate_{0};
    uint8_t channels_{0};
    uint8_t bits_per_sample_{0};
    size_t bytes_per_frame_{0};

    static constexpr size_t RING_CAPACITY = 2 * 1024 * 1024;
    std::vector<uint8_t> ring_;
    size_t ring_head_{0};
    size_t ring_tail_{0};
    size_t ring_used_{0};
    std::mutex ring_mtx_;
    std::condition_variable ring_cv_;

    std::thread playback_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> flush_requested_{false};

    // Written and read exclusively by the playback thread.
    int64_t total_frames_written_{0};
    int64_t playback_anchor_us_{0};

    std::atomic<uint8_t> volume_{100};
    std::atomic<bool> muted_{false};

    void playback_loop();
    void process_chunk(const uint8_t* in, int32_t* out_s32, size_t frames);
};
#include "libshelbourne_sink.h"

extern "C" {
    #include "shelbourne.h"
}

#include <pthread.h>
#include <sched.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <sys/stat.h>

#define HW static_cast<shelbourne_t*>(hw_)

ShelbourneSink::ShelbourneSink() {
    // Take the audio hardware away from the stock software -- but ONLY when the
    // stock software is actually in charge.
    //
    // When /mnt/nv/shepherd exists, shepherdd was pointed at our own override
    // and no stock daemon was ever started, so there is nothing to stop. Worse,
    // stopping "stock" processes there is actively destructive: libshelbourne's
    // list includes NetManager, which our override runs to bring up Wi-Fi, and
    // the call also SIGSTOPs shepherdd -- our own supervisor. At boot that kills
    // networking before Wi-Fi has associated and freezes the supervisor so it
    // can neither restart NetManager nor honour its recovery="reboot", leaving
    // the device unreachable until the override is removed by hand.
    struct stat st;
    const bool shepherd_override =
        (stat("/mnt/nv/shepherd", &st) == 0) && S_ISDIR(st.st_mode);

    if (shepherd_override) {
        fprintf(stderr, "ShelbourneSink: shepherd override active; leaving "
                        "supervisor and NetManager alone\n");
    } else if (shelbourne_stock_processes_active()) {
        fprintf(stderr, "ShelbourneSink: stock processes active, stopping them\n");
        shelbourne_stop_stock_processes();
    }

    hw_ = shelbourne_init();  // ~4s: codec init + amp stabilize
    if (!hw_) {
        fprintf(stderr, "ShelbourneSink: shelbourne_init() failed\n");
    }
}

ShelbourneSink::~ShelbourneSink() {
    stop();
    if (hw_) {
        shelbourne_shutdown(HW);
        hw_ = nullptr;
    }
}

void ShelbourneSink::set_device(const std::string&) {
    // No device selection on snd_shelby — always the fixed hardware path.
}

bool ShelbourneSink::configure(uint32_t sample_rate, uint8_t channels,
                               uint8_t bits_per_sample) {
    if (!hw_) {
        fprintf(stderr, "ShelbourneSink: hardware not initialized\n");
        return false;
    }
    if (sample_rate != HW_SAMPLE_RATE) {
        fprintf(stderr, "ShelbourneSink: unsupported sample rate %u (must be %u)\n",
                sample_rate, HW_SAMPLE_RATE);
        return false;
    }
    if (channels != 2) {
        fprintf(stderr, "ShelbourneSink: unsupported channels %u (must be 2)\n",
                channels);
        return false;
    }
    if (bits_per_sample != 16 && bits_per_sample != 24 && bits_per_sample != 32) {
        fprintf(stderr, "ShelbourneSink: unsupported bit depth %u\n", bits_per_sample);
        return false;
    }

    stop();

    sample_rate_ = sample_rate;
    channels_ = channels;
    bits_per_sample_ = bits_per_sample;
    bytes_per_frame_ = channels * (bits_per_sample / 8);

    ring_.resize(RING_CAPACITY);
    ring_head_ = ring_tail_ = ring_used_ = 0;
    flush_requested_.store(false);

    shelbourne_audio_reset(HW);
    running_.store(true);
    playback_thread_ = std::thread([this] { playback_loop(); });

    fprintf(stderr, "ShelbourneSink: configured %u Hz %u ch %u bit\n",
            sample_rate_, channels_, bits_per_sample_);
    return true;
}

void ShelbourneSink::stop() {
    if (running_.exchange(false)) {
        ring_cv_.notify_all();
        if (playback_thread_.joinable()) playback_thread_.join();
    }
    std::lock_guard<std::mutex> lk(ring_mtx_);
    ring_head_ = ring_tail_ = ring_used_ = 0;
}

void ShelbourneSink::clear() {
    {
        std::lock_guard<std::mutex> lk(ring_mtx_);
        ring_head_ = ring_tail_ = ring_used_ = 0;
    }
    flush_requested_.store(true);
    ring_cv_.notify_all();
    // playback_anchor_us_ and total_frames_written_ are reset by the playback
    // thread when it processes the flush, avoiding a data race.
}

void ShelbourneSink::set_volume(uint8_t volume) {
    volume_.store(volume > 100 ? 100 : volume);
}

void ShelbourneSink::set_muted(bool muted) {
    muted_.store(muted);
    if (hw_) shelbourne_mute(HW, muted ? 1 : 0);  // hardware mute for instant silence
}

size_t ShelbourneSink::write(uint8_t* data, size_t length, uint32_t timeout_ms) {
    if (!running_.load()) return length;

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    size_t written = 0;
    std::unique_lock<std::mutex> lk(ring_mtx_);

    while (written < length && running_.load()) {
        size_t space = RING_CAPACITY - ring_used_;
        if (space == 0) {
            if (std::chrono::steady_clock::now() >= deadline) break;
            ring_cv_.wait_until(lk, deadline, [&] {
                return (RING_CAPACITY - ring_used_) > 0 || !running_.load();
            });
            continue;
        }
        size_t chunk = std::min(length - written, space);
        size_t head_room = RING_CAPACITY - ring_head_;
        size_t first = std::min(chunk, head_room);
        std::memcpy(ring_.data() + ring_head_, data + written, first);
        if (chunk > first)
            std::memcpy(ring_.data(), data + written + first, chunk - first);
        ring_head_ = (ring_head_ + chunk) % RING_CAPACITY;
        ring_used_ += chunk;
        written += chunk;
    }
    lk.unlock();
    ring_cv_.notify_one();
    return written;
}

// Upscale input PCM to S32LE, applying quadratic volume gain inline.
// `frames` is the number of stereo frames; in_buf must hold frames * bytes_per_frame_ bytes.
void ShelbourneSink::process_chunk(const uint8_t* in, int32_t* out_s32, size_t frames) {
    const uint8_t vol = volume_.load();
    const bool muted = muted_.load();
    const size_t samples = frames * channels_;

    if (muted || vol == 0) {
        std::memset(out_s32, 0, samples * sizeof(int32_t));
        return;
    }

    static constexpr uint64_t Q32_ONE = UINT64_C(1) << 32;
    static constexpr int FRAC_BITS = 32;
    static constexpr int64_t ROUND = INT64_C(1) << (FRAC_BITS - 1);
    const int64_t scale =
        static_cast<int64_t>((static_cast<uint64_t>(vol) * vol * Q32_ONE) / 10000);
    const bool unity = (vol >= 100);

    if (bits_per_sample_ == 16) {
        const int16_t* src = reinterpret_cast<const int16_t*>(in);
        for (size_t i = 0; i < samples; ++i) {
            int32_t s32 = static_cast<int32_t>(src[i]) << 16;
            if (!unity)
                s32 = static_cast<int32_t>(
                    (static_cast<int64_t>(s32) * scale + ROUND) >> FRAC_BITS);
            out_s32[i] = s32;
        }
    } else if (bits_per_sample_ == 24) {
        for (size_t i = 0; i < samples; ++i) {
            const uint8_t* p = in + i * 3;
            int32_t s24 = static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
            if (s24 & 0x800000) s24 |= static_cast<int32_t>(0xFF000000);
            int32_t s32 = s24 << 8;
            if (!unity)
                s32 = static_cast<int32_t>(
                    (static_cast<int64_t>(s32) * scale + ROUND) >> FRAC_BITS);
            out_s32[i] = s32;
        }
    } else {  // 32-bit
        const int32_t* src = reinterpret_cast<const int32_t*>(in);
        if (unity) {
            std::memcpy(out_s32, src, samples * sizeof(int32_t));
        } else {
            for (size_t i = 0; i < samples; ++i)
                out_s32[i] = static_cast<int32_t>(
                    (static_cast<int64_t>(src[i]) * scale + ROUND) >> FRAC_BITS);
        }
    }
}

void ShelbourneSink::playback_loop() {
    total_frames_written_ = 0;
    playback_anchor_us_ = 0;
    // SCHED_FIFO: predictable pacing for the 48 kHz DMA writes.
    struct sched_param sp = {};
    sp.sched_priority = 1;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        fprintf(stderr, "ShelbourneSink: SCHED_FIFO unavailable (need CAP_SYS_NICE or "
                "RLIMIT_RTPRIO >= 1); audio may glitch under CPU load\n");
    }

    constexpr size_t HW_FRAMES = SHELBOURNE_FRAMES_PER_WRITE;  // 512
    const size_t in_chunk_bytes = HW_FRAMES * bytes_per_frame_;

    std::vector<uint8_t> in_buf(in_chunk_bytes);
    std::vector<int32_t> out_buf(HW_FRAMES * channels_);

    while (running_.load()) {
        if (flush_requested_.exchange(false)) {
            shelbourne_audio_reset(HW);
            total_frames_written_ = 0;
            playback_anchor_us_ = 0;
            continue;
        }

        // Pull one DMA-sized chunk from the ring.
        bool underrun = false;
        {
            std::unique_lock<std::mutex> lk(ring_mtx_);
            if (ring_used_ < in_chunk_bytes) {
                ring_cv_.wait_for(lk, std::chrono::milliseconds(5), [&] {
                    return ring_used_ >= in_chunk_bytes || !running_.load()
                           || flush_requested_.load();
                });
                if (!running_.load() || flush_requested_.load()) continue;
                if (ring_used_ < in_chunk_bytes) {
                    underrun = true;
                }
            }
            if (!underrun) {
                size_t tail_room = RING_CAPACITY - ring_tail_;
                size_t first = std::min(in_chunk_bytes, tail_room);
                std::memcpy(in_buf.data(), ring_.data() + ring_tail_, first);
                if (in_chunk_bytes > first)
                    std::memcpy(in_buf.data() + first, ring_.data(), in_chunk_bytes - first);
                ring_tail_ = (ring_tail_ + in_chunk_bytes) % RING_CAPACITY;
                ring_used_ -= in_chunk_bytes;
            }
        }
        if (!underrun) {
            ring_cv_.notify_one();
            process_chunk(in_buf.data(), out_buf.data(), HW_FRAMES);
        } else {
            // Keep the hardware clock running with silence rather than starving
            // the DMA, which would cause clicks or repeated samples.
            std::memset(out_buf.data(), 0, HW_FRAMES * channels_ * sizeof(int32_t));
        }

        shelbourne_audio_write(HW, out_buf.data(), HW_FRAMES);  // non-blocking
        shelbourne_audio_wait(HW);                              // paces at 48 kHz

        // Skip timing feedback on underrun: no real audio was played, so
        // advancing the sync task's playtime estimate would cause drift.
        if (underrun) continue;

        if (on_frames_played) {
            int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            // Snapshot the count before this batch, then advance.
            int64_t frames_before = total_frames_written_;
            total_frames_written_ += HW_FRAMES;

            int64_t elapsed_before  = (frames_before        * INT64_C(1000000)) / sample_rate_;
            int64_t playout_offset  = (total_frames_written_ * INT64_C(1000000)) / sample_rate_;

            if (playback_anchor_us_ == 0) {
                // First call: anchor = wall-clock at moment this chunk was queued.
                playback_anchor_us_ = now_us;
            } else {
                // Drift correction. implied_anchor uses elapsed time for the PRIOR frames
                // (already in the pipeline), so it equals anchor when hardware runs at
                // nominal rate. Any deviation is real drift, slowly tracked at ~0.1%/iter.
                int64_t implied_anchor = now_us - elapsed_before;
                int64_t correction = (implied_anchor - playback_anchor_us_) / 1024;
                playback_anchor_us_ += correction;
            }

            int64_t playout_us = playback_anchor_us_ + playout_offset + PIPELINE_DELAY_US;
            on_frames_played(static_cast<uint32_t>(HW_FRAMES), playout_us);
        }
    }
}

#undef HW
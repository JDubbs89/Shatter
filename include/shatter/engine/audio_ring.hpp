#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace shatter::engine {

class AudioRingBuffer {
public:
    static constexpr std::size_t capacity_frames = 8192;

    AudioRingBuffer()
        : samples_(std::make_unique<float[]>(capacity_frames * 2))
    {
    }

    std::size_t write(const float* input, std::size_t frames) noexcept
    {
        return write_with_gain(input, frames, 1.0F);
    }

    std::size_t write_with_gain(const float* input, std::size_t frames, float gain) noexcept
    {
        const std::uint64_t write_frame = write_frame_.load(std::memory_order_relaxed);
        const std::uint64_t read_frame = read_frame_.load(std::memory_order_acquire);
        const auto available = capacity_frames - static_cast<std::size_t>(write_frame - read_frame);
        const auto written = std::min(frames, available);

        for (std::size_t frame = 0; frame < written; ++frame) {
            const auto slot = static_cast<std::size_t>((write_frame + frame) % capacity_frames) * 2;
            samples_[slot] = input[frame * 2] * gain;
            samples_[slot + 1] = input[frame * 2 + 1] * gain;
        }
        write_frame_.store(write_frame + written, std::memory_order_release);
        return written;
    }

    std::size_t write_with_gain_ramp(const float* input, std::size_t frames,
                                     float start_gain, float gain_increment) noexcept
    {
        const std::uint64_t write_frame = write_frame_.load(std::memory_order_relaxed);
        const std::uint64_t read_frame = read_frame_.load(std::memory_order_acquire);
        const auto available = capacity_frames - static_cast<std::size_t>(write_frame - read_frame);
        const auto written = std::min(frames, available);
        float gain = start_gain;

        for (std::size_t frame = 0; frame < written; ++frame) {
            const auto slot = static_cast<std::size_t>((write_frame + frame) % capacity_frames) * 2;
            samples_[slot] = input[frame * 2] * gain;
            samples_[slot + 1] = input[frame * 2 + 1] * gain;
            gain += gain_increment;
        }
        write_frame_.store(write_frame + written, std::memory_order_release);
        return written;
    }

    std::size_t read(float* output, std::size_t frames) noexcept
    {
        const std::uint64_t read_frame = read_frame_.load(std::memory_order_relaxed);
        const std::uint64_t write_frame = write_frame_.load(std::memory_order_acquire);
        const auto available = static_cast<std::size_t>(write_frame - read_frame);
        const auto consumed = std::min(frames, available);

        for (std::size_t frame = 0; frame < consumed; ++frame) {
            const auto slot = static_cast<std::size_t>((read_frame + frame) % capacity_frames) * 2;
            output[frame * 2] = samples_[slot];
            output[frame * 2 + 1] = samples_[slot + 1];
        }
        std::fill(output + consumed * 2, output + frames * 2, 0.0F);
        read_frame_.store(read_frame + consumed, std::memory_order_release);
        return consumed;
    }

    void discard_all() noexcept
    {
        const auto write_frame = write_frame_.load(std::memory_order_acquire);
        read_frame_.store(write_frame, std::memory_order_release);
    }

    std::size_t accumulate(float* output, std::size_t frames, float gain) noexcept
    {
        const std::uint64_t read_frame = read_frame_.load(std::memory_order_relaxed);
        const std::uint64_t write_frame = write_frame_.load(std::memory_order_acquire);
        const auto available = static_cast<std::size_t>(write_frame - read_frame);
        const auto consumed = std::min(frames, available);

        for (std::size_t frame = 0; frame < consumed; ++frame) {
            const auto slot = static_cast<std::size_t>((read_frame + frame) % capacity_frames) * 2;
            output[frame * 2] += samples_[slot] * gain;
            output[frame * 2 + 1] += samples_[slot + 1] * gain;
        }
        read_frame_.store(read_frame + consumed, std::memory_order_release);
        return consumed;
    }


private:
    std::unique_ptr<float[]> samples_;
    alignas(64) std::atomic<std::uint64_t> write_frame_{0};
    alignas(64) std::atomic<std::uint64_t> read_frame_{0};
};

} // namespace shatter::engine

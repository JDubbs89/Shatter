#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace shatter::engine {

class AudioRingBuffer {
public:
    static constexpr std::size_t capacity_frames = 16384;

    std::size_t write(const float* input, std::size_t frames) noexcept
    {
        const std::uint64_t write_frame = write_frame_.load(std::memory_order_relaxed);
        const std::uint64_t read_frame = read_frame_.load(std::memory_order_acquire);
        const auto available = capacity_frames - static_cast<std::size_t>(write_frame - read_frame);
        const auto written = std::min(frames, available);

        for (std::size_t frame = 0; frame < written; ++frame) {
            const auto slot = static_cast<std::size_t>((write_frame + frame) % capacity_frames) * 2;
            samples_[slot] = input[frame * 2];
            samples_[slot + 1] = input[frame * 2 + 1];
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

private:
    alignas(64) std::array<float, capacity_frames * 2> samples_{};
    alignas(64) std::atomic<std::uint64_t> write_frame_{0};
    alignas(64) std::atomic<std::uint64_t> read_frame_{0};
};

} // namespace shatter::engine

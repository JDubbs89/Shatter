#include "shatter/engine/audio_ring.hpp"

#include <array>
#include <iostream>
#include <vector>

int main()
{
    shatter::engine::AudioRingBuffer ring;
    const std::array<float, 4> input{0.25F, -0.25F, 0.75F, -0.75F};
    std::array<float, 6> output{};

    if (ring.write(input.data(), 2) != 2 || ring.read(output.data(), 3) != 2) {
        std::cerr << "ring buffer did not report the transferred frame count\n";
        return 1;
    }
    if (output != std::array<float, 6>{0.25F, -0.25F, 0.75F, -0.75F, 0.0F, 0.0F}) {
        std::cerr << "ring buffer did not preserve stereo samples or silence-fill underflow\n";
        return 1;
    }

    std::vector<float> oversized(shatter::engine::AudioRingBuffer::capacity_frames * 2 + 2, 1.0F);
    if (ring.write(oversized.data(), shatter::engine::AudioRingBuffer::capacity_frames + 1) !=
        shatter::engine::AudioRingBuffer::capacity_frames) {
        std::cerr << "ring buffer did not bound writes to its preallocated capacity\n";
        return 1;
    }

    return 0;
}

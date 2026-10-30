#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

namespace shatter::engine {

// Bounded, allocation-free callback timing storage. Callback durations are
// placed in logarithmic nanosecond buckets so RT callbacks never need a lock.
class RuntimeProfiler {
public:
    enum class Callback : std::size_t { input = 0, output = 1 };
    static constexpr std::size_t histogram_buckets = 64;

    class Scope {
    public:
        Scope(RuntimeProfiler& profiler, Callback callback) noexcept
            : profiler_(&profiler), callback_(callback)
        {
            generation_ = profiler_->generation_.load(std::memory_order_acquire);
            if (!profiler_->active_.load(std::memory_order_acquire)) {
                profiler_ = nullptr;
                return;
            }
            profiler_->writers_.fetch_add(1, std::memory_order_acq_rel);
            if (!profiler_->active_.load(std::memory_order_acquire) ||
                profiler_->generation_.load(std::memory_order_acquire) != generation_) {
                profiler_->writers_.fetch_sub(1, std::memory_order_release);
                profiler_ = nullptr;
                return;
            }
            started_ = std::chrono::steady_clock::now();
        }

        ~Scope()
        {
            if (profiler_ == nullptr) return;
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started_).count();
            profiler_->record(callback_, static_cast<std::uint64_t>(elapsed), frames_, sample_rate_);
            profiler_->writers_.fetch_sub(1, std::memory_order_release);
        }

        void set_frames(std::uint64_t frames, std::uint32_t sample_rate) noexcept
        {
            frames_ = frames;
            sample_rate_ = sample_rate;
        }

        void record_input_overrun(std::uint64_t frames) noexcept
        {
            if (profiler_ != nullptr) {
                profiler_->input_overrun_frames_.fetch_add(frames, std::memory_order_relaxed);
            }
        }

        void record_unfilled_mix_frames(std::uint64_t frames) noexcept
        {
            if (profiler_ != nullptr) {
                profiler_->unfilled_mix_frames_.fetch_add(frames, std::memory_order_relaxed);
            }
        }

        void record_ring_occupancy(std::uint64_t frames) noexcept
        {
            if (profiler_ == nullptr) return;
            profiler_->ring_occupancy_sum_.fetch_add(frames, std::memory_order_relaxed);
            profiler_->ring_occupancy_samples_.fetch_add(1, std::memory_order_relaxed);
            update_max(profiler_->ring_occupancy_max_, frames);
        }

        void record_app_stream_ring_occupancy(std::uint64_t frames) noexcept
        {
            if (profiler_ == nullptr) return;
            profiler_->app_stream_ring_occupancy_sum_.fetch_add(frames, std::memory_order_relaxed);
            profiler_->app_stream_ring_occupancy_samples_.fetch_add(1, std::memory_order_relaxed);
        }

        void record_pipewire_time(std::uint64_t delay_ns, std::uint64_t queue_frames) noexcept
        {
            if (profiler_ == nullptr) return;
            update_max(profiler_->pipewire_delay_ns_, delay_ns);
            update_max(profiler_->pipewire_queue_frames_, queue_frames);
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        RuntimeProfiler* profiler_;
        Callback callback_;
        std::chrono::steady_clock::time_point started_{};
        std::uint64_t frames_ = 0;
        std::uint32_t sample_rate_ = 0;
        std::uint64_t generation_ = 0;
    };

    void start() noexcept
    {
        active_.store(false, std::memory_order_release);
        while (writers_.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
        generation_.fetch_add(1, std::memory_order_acq_rel);
        for (auto& group : groups_) {
            group.calls.store(0, std::memory_order_relaxed);
            group.total_ns.store(0, std::memory_order_relaxed);
            group.max_ns.store(0, std::memory_order_relaxed);
            group.frames.store(0, std::memory_order_relaxed);
            group.deadline_misses.store(0, std::memory_order_relaxed);
            for (auto& bucket : group.histogram) bucket.store(0, std::memory_order_relaxed);
        }
        input_overrun_frames_.store(0, std::memory_order_relaxed);
        unfilled_mix_frames_.store(0, std::memory_order_relaxed);
        ring_occupancy_sum_.store(0, std::memory_order_relaxed);
        ring_occupancy_samples_.store(0, std::memory_order_relaxed);
        ring_occupancy_max_.store(0, std::memory_order_relaxed);
        app_stream_ring_occupancy_sum_.store(0, std::memory_order_relaxed);
        app_stream_ring_occupancy_samples_.store(0, std::memory_order_relaxed);
        pipewire_delay_ns_.store(0, std::memory_order_relaxed);
        pipewire_queue_frames_.store(0, std::memory_order_relaxed);
        active_.store(true, std::memory_order_release);
    }

    void stop() noexcept
    {
        active_.store(false, std::memory_order_release);
        while (writers_.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
    }

    bool active() const noexcept { return active_.load(std::memory_order_relaxed); }

    std::string report(std::uint64_t wall_ns, double process_cpu_seconds,
                       double process_cpu_percent) const
    {
        std::ostringstream out;
        out << std::fixed << std::setprecision(2);
        out << "Shatter runtime profile\n"
            << "Duration: " << wall_ns / 1'000'000'000.0 << " s  |  Process CPU: "
            << process_cpu_seconds << " s (" << process_cpu_percent << "% of one CPU core)\n\n"
            << "Callback timing (durations in microseconds; percentiles are approximate)\n";
        out << "+----------------+--------+--------+--------+--------+--------+--------+------------+----------+\n"
            << "| Callback       | Calls  | Avg    | P50    | P95    | P99    | Max    | Frames     | Overruns |\n"
            << "+----------------+--------+--------+--------+--------+--------+--------+------------+----------+\n";
        append_group(out, "Playback input", groups_[0]);
        append_group(out, "Speaker output", groups_[1]);
        out << "+----------------+--------+--------+--------+--------+--------+--------+------------+----------+\n\n"
            << "Buffering and PipeWire timing\n"
            << '+' << std::string(42, '-') << '+' << std::string(46, '-') << "+\n"
            << "| " << std::left << std::setw(40) << "Metric" << " | "
            << std::setw(44) << "Value" << " |\n"
            << '+' << std::string(42, '-') << '+' << std::string(46, '-') << "+\n";
        const auto ring_samples = ring_occupancy_samples_.load(std::memory_order_relaxed);
        const double average_ring_frames = ring_samples == 0 ? 0.0 :
            static_cast<double>(ring_occupancy_sum_.load(std::memory_order_relaxed)) / ring_samples;
        const auto maximum_ring_frames = ring_occupancy_max_.load(std::memory_order_relaxed);
        append_metric(out, "Input ring dropped frames", std::to_string(
            input_overrun_frames_.load(std::memory_order_relaxed)));
        append_metric(out, "Frames unavailable at mix time", std::to_string(
            unfilled_mix_frames_.load(std::memory_order_relaxed)));
        std::ostringstream occupancy;
        occupancy << std::fixed << std::setprecision(1) << average_ring_frames << "/"
                  << maximum_ring_frames << " frames (" << average_ring_frames * 1000.0 / 48000.0
                  << "/" << maximum_ring_frames * 1000.0 / 48000.0 << " ms)";
        append_metric(out, "Shatter input ring occupancy avg/max", occupancy.str());
        const auto app_stream_samples = app_stream_ring_occupancy_samples_.load(std::memory_order_relaxed);
        const double app_stream_average_frames = app_stream_samples == 0 ? 0.0 :
            static_cast<double>(app_stream_ring_occupancy_sum_.load(std::memory_order_relaxed)) /
                app_stream_samples;
        std::ostringstream app_stream_latency;
        app_stream_latency << std::fixed << std::setprecision(3)
                           << app_stream_average_frames * 1000.0 / 48000.0 << " ms";
        append_metric(out, "Estimated I/O Latency", app_stream_latency.str());
        std::ostringstream device_delay;
        device_delay << std::fixed << std::setprecision(3)
                     << pipewire_delay_ns_.load(std::memory_order_relaxed) / 1'000'000.0 << " ms";
        append_metric(out, "PipeWire max device delay", device_delay.str());
        append_metric(out, "PipeWire max queued audio", std::to_string(
            pipewire_queue_frames_.load(std::memory_order_relaxed)) + " frames");
        out << '+' << std::string(42, '-') << '+' << std::string(46, '-') << "+\n"
            << "Note: unavailable mix frames can include idle/silent streams. PipeWire delay excludes queued buffers.\n"
            << "Callback percentiles are approximate log2 histogram bucket bounds.\n";
        return out.str();
    }

private:
    struct Group {
        std::atomic<std::uint64_t> calls{0};
        std::atomic<std::uint64_t> total_ns{0};
        std::atomic<std::uint64_t> max_ns{0};
        std::atomic<std::uint64_t> frames{0};
        std::atomic<std::uint64_t> deadline_misses{0};
        std::array<std::atomic<std::uint64_t>, histogram_buckets> histogram{};
    };

    static void update_max(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept
    {
        auto current = target.load(std::memory_order_relaxed);
        while (current < value && !target.compare_exchange_weak(
                   current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }

    void record(Callback callback, std::uint64_t elapsed_ns, std::uint64_t frames,
                std::uint32_t sample_rate) noexcept
    {
        auto& group = groups_[static_cast<std::size_t>(callback)];
        group.calls.fetch_add(1, std::memory_order_relaxed);
        group.total_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
        group.frames.fetch_add(frames, std::memory_order_relaxed);
        if (frames > 0 && sample_rate > 0 && elapsed_ns >= frames * 1'000'000'000ULL / sample_rate) {
            group.deadline_misses.fetch_add(1, std::memory_order_relaxed);
        }
        update_max(group.max_ns, elapsed_ns);
        std::size_t bucket = 0;
        for (std::uint64_t value = elapsed_ns; value > 1 && bucket + 1 < histogram_buckets;
             value >>= 1U, ++bucket) {}
        group.histogram[bucket].fetch_add(1, std::memory_order_relaxed);
    }

    static std::uint64_t percentile_ns(const Group& group, std::uint64_t calls, unsigned percent)
    {
        if (calls == 0) return 0;
        const std::uint64_t target = (calls * percent + 99) / 100;
        std::uint64_t seen = 0;
        for (std::size_t bucket = 0; bucket < histogram_buckets; ++bucket) {
            seen += group.histogram[bucket].load(std::memory_order_relaxed);
            if (seen >= target) return bucket == 0 ? 1 : (1ULL << (bucket + 1));
        }
        return group.max_ns.load(std::memory_order_relaxed);
    }

    static void append_group(std::ostringstream& out, const char* name, const Group& group)
    {
        const auto calls = group.calls.load(std::memory_order_relaxed);
        const auto average = calls == 0 ? 0.0 :
            static_cast<double>(group.total_ns.load(std::memory_order_relaxed)) / calls / 1000.0;
        out << "| " << std::left << std::setw(14) << name << " | "
            << std::right << std::setw(6) << calls << " | " << std::setw(6) << average << " | "
            << std::setw(6) << percentile_ns(group, calls, 50) / 1000.0 << " | "
            << std::setw(6) << percentile_ns(group, calls, 95) / 1000.0 << " | "
            << std::setw(6) << percentile_ns(group, calls, 99) / 1000.0 << " | "
            << std::setw(6) << group.max_ns.load(std::memory_order_relaxed) / 1000.0 << " | "
            << std::setw(10) << group.frames.load(std::memory_order_relaxed) << " | "
            << std::setw(8) << group.deadline_misses.load(std::memory_order_relaxed) << " |\n"
            << std::left;
    }

    static void append_metric(std::ostringstream& out, const char* name, const std::string& value)
    {
        out << "| " << std::left << std::setw(40) << name << " | "
            << std::setw(44) << value << " |\n";
    }

    std::array<Group, 2> groups_{};
    std::atomic<std::uint64_t> input_overrun_frames_{0};
    std::atomic<std::uint64_t> unfilled_mix_frames_{0};
    std::atomic<std::uint64_t> ring_occupancy_sum_{0};
    std::atomic<std::uint64_t> ring_occupancy_samples_{0};
    std::atomic<std::uint64_t> ring_occupancy_max_{0};
    std::atomic<std::uint64_t> app_stream_ring_occupancy_sum_{0};
    std::atomic<std::uint64_t> app_stream_ring_occupancy_samples_{0};
    std::atomic<std::uint64_t> pipewire_delay_ns_{0};
    std::atomic<std::uint64_t> pipewire_queue_frames_{0};
    std::atomic<std::uint32_t> writers_{0};
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<bool> active_{false};
};

} // namespace shatter::engine

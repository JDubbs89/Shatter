#include <pipewire/keys.h>
#include <pipewire/loop.h>
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kChannels = 2;

struct TestInput {
    pw_main_loop* main_loop = nullptr;
    pw_stream* stream = nullptr;
    spa_hook listener{};
    pw_stream_events events{};
    double square_sum = 0.0;
    std::uint64_t sample_count = 0;
};

void process(void* data)
{
    auto& input = *static_cast<TestInput*>(data);
    pw_buffer* buffer = pw_stream_dequeue_buffer(input.stream);
    if (buffer == nullptr || buffer->buffer == nullptr || buffer->buffer->n_datas == 0) return;

    spa_data& audio = buffer->buffer->datas[0];
    if (audio.data != nullptr && audio.chunk != nullptr) {
        const auto* samples = static_cast<const float*>(audio.data);
        const auto sample_count = static_cast<std::size_t>(audio.chunk->size / sizeof(float));
        for (std::size_t index = 0; index < sample_count; ++index) {
            const double sample = samples[index];
            input.square_sum += sample * sample;
        }
        input.sample_count += sample_count;
    }
    pw_stream_queue_buffer(input.stream, buffer);
}

void stop_after_duration(void* data, std::uint64_t)
{
    auto& input = *static_cast<TestInput*>(data);
    pw_main_loop_quit(input.main_loop);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 4) return 2;
    const char* node_name = argv[1];
    const char* result_path = argv[2];
    const long duration_ms = std::strtol(argv[3], nullptr, 10);
    if (duration_ms < 100 || duration_ms > 10000) return 2;

    pw_init(nullptr, nullptr);
    TestInput input;
    input.main_loop = pw_main_loop_new(nullptr);
    if (input.main_loop == nullptr) return 1;
    pw_context* context = pw_context_new(pw_main_loop_get_loop(input.main_loop), nullptr, 0);
    if (context == nullptr) {
        pw_main_loop_destroy(input.main_loop);
        return 1;
    }
    pw_core* core = pw_context_connect(context, nullptr, 0);
    if (core == nullptr) {
        pw_context_destroy(context);
        pw_main_loop_destroy(input.main_loop);
        return 1;
    }

    input.stream = pw_stream_new(core, "Shatter integration application input",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                         PW_KEY_MEDIA_CATEGORY, "Capture",
                         PW_KEY_MEDIA_ROLE, "Communication",
                         PW_KEY_MEDIA_CLASS, "Stream/Input/Audio",
                         PW_KEY_NODE_NAME, node_name,
                         PW_KEY_NODE_DESCRIPTION, "Shatter Integration Application Input",
                         PW_KEY_APP_NAME, "Shatter Integration Probe",
                         PW_KEY_APP_ID, "org.shatter.integration.input",
                         "node.autoconnect", "false",
                         "node.dont-fallback", "true",
                         PW_KEY_AUDIO_CHANNELS, "2",
                         nullptr));
    if (input.stream == nullptr) {
        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_main_loop_destroy(input.main_loop);
        return 1;
    }
    input.events.version = PW_VERSION_STREAM_EVENTS;
    input.events.process = process;
    pw_stream_add_listener(input.stream, &input.listener, &input.events, &input);

    std::array<std::uint8_t, 1024> storage{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = kRate;
    info.channels = kChannels;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod* params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
    const int connect_result = pw_stream_connect(input.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1);
    if (connect_result < 0) {
        pw_stream_destroy(input.stream);
        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_main_loop_destroy(input.main_loop);
        return 1;
    }

    timespec timeout{};
    timeout.tv_sec = duration_ms / 1000;
    timeout.tv_nsec = (duration_ms % 1000) * 1'000'000L;
    spa_source* timer = pw_loop_add_timer(pw_main_loop_get_loop(input.main_loop), stop_after_duration, &input);
    if (timer == nullptr || pw_loop_update_timer(pw_main_loop_get_loop(input.main_loop), timer,
                                                &timeout, nullptr, false) < 0) {
        pw_stream_disconnect(input.stream);
        pw_stream_destroy(input.stream);
        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_main_loop_destroy(input.main_loop);
        return 1;
    }

    pw_main_loop_run(input.main_loop);
    pw_loop_destroy_source(pw_main_loop_get_loop(input.main_loop), timer);
    pw_stream_disconnect(input.stream);
    pw_stream_destroy(input.stream);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(input.main_loop);

    const double rms = input.sample_count == 0 ? 0.0 : std::sqrt(input.square_sum / input.sample_count);
    std::FILE* result = std::fopen(result_path, "w");
    if (result == nullptr) return 1;
    std::fprintf(result, "%.9f\n", rms);
    return std::fclose(result) == 0 ? 0 : 1;
}

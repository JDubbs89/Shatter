#include <pipewire/keys.h>
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kChannels = 2;
constexpr double kTau = 6.28318530717958647692;

struct TestSource {
    pw_main_loop* main_loop = nullptr;
    pw_stream* stream = nullptr;
    spa_hook listener{};
    pw_stream_events events{};
    double phase = 0.0;
};

void process(void* data)
{
    auto& source = *static_cast<TestSource*>(data);
    pw_buffer* buffer = pw_stream_dequeue_buffer(source.stream);
    if (buffer == nullptr || buffer->buffer == nullptr || buffer->buffer->n_datas == 0) return;

    spa_data& audio = buffer->buffer->datas[0];
    if (audio.data == nullptr || audio.chunk == nullptr) {
        pw_stream_queue_buffer(source.stream, buffer);
        return;
    }
    const auto capacity = static_cast<std::size_t>(audio.maxsize) / (sizeof(float) * kChannels);
    const auto requested = buffer->requested == 0 ? capacity : static_cast<std::size_t>(buffer->requested);
    const auto frames = std::min(capacity, requested);
    auto* samples = static_cast<float*>(audio.data);
    constexpr double increment = kTau * 440.0 / kRate;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float sample = static_cast<float>(0.25 * std::sin(source.phase));
        samples[frame * 2] = sample;
        samples[frame * 2 + 1] = sample;
        source.phase += increment;
        if (source.phase >= kTau) source.phase -= kTau;
    }
    audio.chunk->offset = 0;
    audio.chunk->stride = sizeof(float) * kChannels;
    audio.chunk->size = frames * sizeof(float) * kChannels;
    pw_stream_queue_buffer(source.stream, buffer);
}

} // namespace

int main()
{
    pw_init(nullptr, nullptr);
    TestSource source;
    source.main_loop = pw_main_loop_new(nullptr);
    if (source.main_loop == nullptr) return 1;
    pw_context* context = pw_context_new(pw_main_loop_get_loop(source.main_loop), nullptr, 0);
    if (context == nullptr) {
        pw_main_loop_destroy(source.main_loop);
        return 1;
    }
    pw_core* core = pw_context_connect(context, nullptr, 0);
    if (core == nullptr) {
        pw_context_destroy(context);
        pw_main_loop_destroy(source.main_loop);
        return 1;
    }

    source.stream = pw_stream_new(core, "Shatter test microphone",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                         PW_KEY_MEDIA_CATEGORY, "Capture",
                         PW_KEY_MEDIA_ROLE, "Test",
                         PW_KEY_MEDIA_CLASS, "Audio/Source",
                         PW_KEY_NODE_NAME, "shatter-test-mic-source",
                         PW_KEY_NODE_DESCRIPTION, "Shatter Integration Test Source",
                         PW_KEY_APP_NAME, "Shatter Integration Test",
                         PW_KEY_AUDIO_CHANNELS, "2",
                         nullptr));
    if (source.stream == nullptr) {
        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_main_loop_destroy(source.main_loop);
        return 1;
    }
    source.events.version = PW_VERSION_STREAM_EVENTS;
    source.events.process = process;
    pw_stream_add_listener(source.stream, &source.listener, &source.events, &source);

    std::array<std::uint8_t, 1024> storage{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = kRate;
    info.channels = kChannels;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod* params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
    const int result = pw_stream_connect(source.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1);
    if (result < 0) {
        pw_stream_destroy(source.stream);
        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_main_loop_destroy(source.main_loop);
        return 1;
    }

    pw_main_loop_run(source.main_loop);
    pw_stream_disconnect(source.stream);
    pw_stream_destroy(source.stream);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(source.main_loop);
    return 0;
}

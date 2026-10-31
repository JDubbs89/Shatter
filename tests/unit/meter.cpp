#include "shatter/meter/meter_bus.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

TEST_CASE("peak and RMS meter publishes known level in dBFS", "[meter]")
{
    shatter::meter::PeakRmsMeter meter;
    std::array<float, shatter::meter::PeakRmsMeter::samples_per_window> samples;
    samples.fill(0.5F);

    meter.observe(samples.data(), samples.size());
    const auto reading = meter.reading();
    CHECK(std::abs(shatter::meter::to_dbfs(reading.peak) - (-6.0206F)) < 0.01F);
    CHECK(std::abs(shatter::meter::to_dbfs(reading.rms) - (-6.0206F)) < 0.01F);
}

TEST_CASE("peak and RMS meter reports digital silence at its documented floor", "[meter]")
{
    shatter::meter::PeakRmsMeter meter;
    std::array<float, shatter::meter::PeakRmsMeter::samples_per_window> samples{};

    meter.observe(samples.data(), samples.size());
    const auto reading = meter.reading();
    CHECK(reading.peak == 0.0F);
    CHECK(reading.rms == 0.0F);
    CHECK(shatter::meter::to_dbfs(reading.peak) == -120.0F);
    CHECK(shatter::meter::to_dbfs(reading.rms) == -120.0F);
}

TEST_CASE("peak and RMS meter ignores non-finite samples", "[meter]")
{
    shatter::meter::PeakRmsMeter meter;
    std::array<float, shatter::meter::PeakRmsMeter::samples_per_window> samples{};
    samples[0] = NAN;
    samples[1] = INFINITY;

    meter.observe(samples.data(), samples.size());
    const auto reading = meter.reading();
    CHECK(reading.peak == 0.0F);
    CHECK(reading.rms == 0.0F);
}

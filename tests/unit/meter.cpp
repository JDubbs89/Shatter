#include "mixer/meter/meter_bus.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>

TEST(MeterTest, PeakAndRmsMeterPublishesKnownLevelInDbfs)
{
    shatter::mixer::meter::PeakRmsMeter meter;
    std::array<float, shatter::mixer::meter::PeakRmsMeter::samples_per_window> samples;
    samples.fill(0.5F);

    meter.observe(samples.data(), samples.size());
    const auto reading = meter.reading();
    EXPECT_LT(std::abs(shatter::mixer::meter::to_dbfs(reading.peak) - (-6.0206F)), 0.01F);
    EXPECT_LT(std::abs(shatter::mixer::meter::to_dbfs(reading.rms) - (-6.0206F)), 0.01F);
}

TEST(MeterTest, PeakAndRmsMeterReportsDigitalSilenceAtDocumentedFloor)
{
    shatter::mixer::meter::PeakRmsMeter meter;
    std::array<float, shatter::mixer::meter::PeakRmsMeter::samples_per_window> samples{};

    meter.observe(samples.data(), samples.size());
    const auto reading = meter.reading();
    EXPECT_EQ(reading.peak, 0.0F);
    EXPECT_EQ(reading.rms, 0.0F);
    EXPECT_EQ(shatter::mixer::meter::to_dbfs(reading.peak), -120.0F);
    EXPECT_EQ(shatter::mixer::meter::to_dbfs(reading.rms), -120.0F);
}

TEST(MeterTest, PeakAndRmsMeterIgnoresNonFiniteSamples)
{
    shatter::mixer::meter::PeakRmsMeter meter;
    std::array<float, shatter::mixer::meter::PeakRmsMeter::samples_per_window> samples{};
    samples[0] = NAN;
    samples[1] = INFINITY;

    meter.observe(samples.data(), samples.size());
    const auto reading = meter.reading();
    EXPECT_EQ(reading.peak, 0.0F);
    EXPECT_EQ(reading.rms, 0.0F);
}

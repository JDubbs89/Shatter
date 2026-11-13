#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "backend/iaudio_backend.hpp"
#include "mixer/control.hpp"

namespace shatter::backend {

TEST(BackendContractTest, SelectedAdapterRejectsDiscoveryBeforeStartWithoutReplacingSnapshot)
{
    auto engine = mixer::create_engine();
    ASSERT_NE(engine, nullptr);
    IAudioBackend& backend = engine->backend();
    std::vector<Endpoint> endpoints{{"previous", "previous", "Previous device",
                                     EndpointKind::playback_device, {}, {}}};
    std::string error;

    EXPECT_FALSE(backend.available());
    EXPECT_FALSE(backend.enumerate_endpoints(endpoints, error));

    EXPECT_FALSE(error.empty());
    ASSERT_EQ(endpoints.size(), 1);
    EXPECT_EQ(endpoints[0].id, "previous");
    engine->stop();
}

} // namespace shatter::backend

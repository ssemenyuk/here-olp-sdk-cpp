#include <olp/core/client/OlpClientSettings.h>
#include <olp/core/client/OlpClientSettingsFactory.h>
#include <olp/core/client/HRN.h>
#include <olp/core/logging/Log.h>
#include <olp/dataservice/read/CatalogClient.h>

#include <gtest/gtest.h>

using namespace olp;

struct CatalogClientTestConfiguration {
  int calling_thread_count;
  int task_scheduler_threads;
};

class CatalogClientTest
    : public ::testing::TestWithParam<CatalogClientTestConfiguration> {
 public:
  static void SetUpTestSuite() {
    s_network = client::OlpClientSettingsFactory::
        CreateDefaultNetworkRequestHandler();
  }
  static void TearDownTestSuite() {
    OLP_SDK_LOG_INFO_F("CatalogClientTest", "Network use count is %d",
                       s_network.use_count());
    s_network = nullptr;
  }

  http::NetworkProxySettings localhost() {
    return olp::http::NetworkProxySettings()
        .WithHostname("http://localhost:3000/http_proxy")
        .WithUsername("test_user")
        .WithPassword("test_password")
        .WithType(olp::http::NetworkProxySettings::Type::HTTP);
  }

  std::shared_ptr<dataservice::read::CatalogClient> createCatalogClient() {
    client::HRN hrn("fake");
    auto settings = std::make_shared<client::OlpClientSettings>();
    settings->network_request_handler = s_network;
    settings->proxy_settings = localhost();
    return std::make_shared<dataservice::read::CatalogClient>(hrn, settings);
  }

 private:
  static std::shared_ptr<olp::http::Network> s_network;
};

TEST_F(CatalogClientTest, ReadNPartitions) {

}

INSTANTIATE_TEST_SUITE_P(Performance, CatalogClientTest,
                         testing::Values(CatalogClientTestConfiguration{1, 1},
                                         CatalogClientTestConfiguration{5, 5}))
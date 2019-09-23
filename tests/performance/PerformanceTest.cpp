#include <olp/authentication/TokenProvider.h>

#include <olp/core/client/HRN.h>
#include <olp/core/client/OlpClientSettings.h>
#include <olp/core/client/OlpClientSettingsFactory.h>
#include <olp/core/logging/Log.h>
#include <olp/core/porting/make_unique.h>

#include <olp/dataservice/read/CatalogClient.h>
#include <olp/dataservice/read/CatalogRequest.h>
#include <olp/dataservice/read/DataRequest.h>
#include <olp/dataservice/read/PartitionsRequest.h>

#include <gtest/gtest.h>

using namespace olp;

struct CatalogClientTestConfiguration {
  int number_of_requests;
  int calling_thread_count;
  int task_scheduler_threads;
  bool use_cache;
};

std::ostream& operator<<(std::ostream& stream,
                         const CatalogClientTestConfiguration& config) {
  return stream << "CatalogClientTestConfiguration("
                << ".calling_thread_count=" << config.calling_thread_count
                << ", .task_scheduler_threads=" << config.task_scheduler_threads
                << ", .number_of_requests=" << config.number_of_requests
                << ", .use_cache=" << config.use_cache << ")";
}

class CatalogClientTest
    : public ::testing::TestWithParam<CatalogClientTestConfiguration> {
 public:
  static void SetUpTestSuite() {
    olp::logging::Log::setLevel(olp::logging::Level::Off);
    s_network =
        client::OlpClientSettingsFactory::CreateDefaultNetworkRequestHandler();
  }
  static void TearDownTestSuite() {
    OLP_SDK_LOG_INFO_F("CatalogClientTest", "Network use count is %ld",
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

 protected:
  std::shared_ptr<olp::thread::TaskScheduler> task_scheduler_;
  static std::shared_ptr<olp::http::Network> s_network;
};

std::shared_ptr<olp::http::Network> CatalogClientTest::s_network;

namespace {
const std::string kKeyId("");
const std::string kKeySecret("");
const std::string kCatalogHRN("");
const std::string kLayerId("");
const std::string kPartitionId("");
}  // namespace

TEST_P(CatalogClientTest, ReadNPartitions) {
  auto parameter = GetParam();

  task_scheduler_ =
      olp::client::OlpClientSettingsFactory::CreateDefaultTaskScheduler(
          parameter.task_scheduler_threads);

  // Initialize authentication settings
  olp::authentication::Settings settings;
  settings.task_scheduler = task_scheduler_;
  settings.network_request_handler = s_network;

  // Setup AuthenticationSettings with a default token provider that will
  // retrieve an OAuth 2.0 token from OLP.
  olp::client::AuthenticationSettings auth_settings;
  auth_settings.provider = olp::authentication::TokenProviderDefault(
      kKeyId, kKeySecret, std::move(settings));

  // Setup OlpClientSettings and provide it to the CatalogClient.
  auto client_settings = std::make_shared<olp::client::OlpClientSettings>();
  client_settings->authentication_settings = auth_settings;
  client_settings->task_scheduler = task_scheduler_;
  client_settings->network_request_handler = s_network;

  auto service_client = std::make_unique<olp::dataservice::read::CatalogClient>(
      olp::client::HRN(kCatalogHRN), std::move(client_settings));

  std::vector<std::thread> client_threads;

  for (int client_id = 0; client_id < parameter.calling_thread_count;
       client_id++) {
    auto thread = std::thread([&]() {
      for (int iteration = 0; iteration < parameter.number_of_requests;
           iteration++) {
        auto request = olp::dataservice::read::DataRequest()
                           .WithLayerId(kLayerId)
                           .WithPartitionId(kPartitionId)
                           .WithBillingTag(boost::none);

        auto future = service_client->GetData(request);
        olp::dataservice::read::DataResponse data_response =
            future.GetFuture().get();
      }
    });
    client_threads.push_back(std::move(thread));
  }

  for (auto& thread : client_threads) {
    thread.join();
  }
}

INSTANTIATE_TEST_SUITE_P(Performance, CatalogClientTest,
                         testing::Values(
                             /// task scheduler has 1 thread
                             CatalogClientTestConfiguration{1, 1, 1, true},
                             CatalogClientTestConfiguration{100, 1, 1, true},
                             CatalogClientTestConfiguration{1, 100, 1, true},
                             CatalogClientTestConfiguration{100, 100, 1, true},
                             /// task scheduler has 10 threads
                             CatalogClientTestConfiguration{1, 1, 10, true},
                             CatalogClientTestConfiguration{100, 1, 10, true},
                             CatalogClientTestConfiguration{1, 100, 10, true},
                             CatalogClientTestConfiguration{100, 100, 10,
                                                            true}));
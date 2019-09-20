/*
 * Copyright (C) 2019 HERE Europe B.V.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 * License-Filename: LICENSE
 */

#include <chrono>
#include <string>

#include <gmock/gmock.h>

#include <olp/authentication/Settings.h>
#include <olp/authentication/TokenProvider.h>

#include <olp/core/client/OlpClientSettings.h>
#include <olp/core/client/OlpClientSettingsFactory.h>
#include <olp/core/logging/Log.h>
#include <olp/core/porting/make_unique.h>

#include <olp/dataservice/read/VersionedLayerClient.h>

#include "testutils/CustomParameters.hpp"

#include <olp/core/http/Network.h>
#include <olp/core/http/NetworkRequest.h>
#include <olp/core/http/NetworkResponse.h>

#include "HttpResponses.h"

using namespace olp::dataservice::read;
using namespace testing;

namespace {

constexpr auto kWaitTimeout = std::chrono::seconds(10);

std::function<olp::http::SendOutcome(
    olp::http::NetworkRequest request, olp::http::Network::Payload payload,
    olp::http::Network::Callback callback,
    olp::http::Network::HeaderCallback header_callback,
    olp::http::Network::DataCallback data_callback)>
ReturnHttpResponse(olp::http::NetworkResponse response,
                   const std::string& response_body) {
  return [=](olp::http::NetworkRequest request,
             olp::http::Network::Payload payload,
             olp::http::Network::Callback callback,
             olp::http::Network::HeaderCallback header_callback,
             olp::http::Network::DataCallback data_callback)
             -> olp::http::SendOutcome {
    std::thread([=]() {
      *payload << response_body;
      callback(response);
    })
        .detach();

    constexpr auto unused_request_id = 5;
    return olp::http::SendOutcome(unused_request_id);
  };
}

class NetworkMock : public olp::http::Network {
 public:
  MOCK_METHOD(olp::http::SendOutcome, Send,
              (olp::http::NetworkRequest request,
               olp::http::Network::Payload payload,
               olp::http::Network::Callback callback,
               olp::http::Network::HeaderCallback header_callback,
               olp::http::Network::DataCallback data_callback),
              (override));

  MOCK_METHOD(void, Cancel, (olp::http::RequestId id), (override));
};

}  // namespace

MATCHER_P(IsGetRequest, url, "") {
  // uri, verb, null body
  return olp::http::NetworkRequest::HttpVerb::GET == arg.GetVerb() &&
         std::string(url).substr(0, 20) == arg.GetUrl().substr(0, 20) && (!arg.GetBody() || arg.GetBody()->empty());
}

std::string GetTestCatalog() {
  return CustomParameters::getArgument("dataservice_read_test_catalog");
}

class VersionedLayerClientOfflineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    network_mock_ = std::make_shared<NetworkMock>();

    settings_ = std::make_shared<olp::client::OlpClientSettings>();
    settings_->network_request_handler = network_mock_;

    EXPECT_CALL(*network_mock_, Send(_, _, _, _, _))
        .WillOnce(
            ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(200),
                               HTTP_RESPONSE_LOOKUP_QUERY))
        .WillOnce(
            ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(200),
                               HTTP_RESPONSE_PARTITION_269))
        .WillOnce(
            ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(200),
                               HTTP_RESPONSE_LOOKUP_BLOB))
        .WillOnce(
            ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(200),
                               HTTP_RESPONSE_BLOB_DATA_269));
  }

  void TearDown() override {
    auto network = std::move(settings_->network_request_handler);
    network_mock_.reset();
    settings_.reset();
    // when test ends we must be sure that network pointer is not captured
    // anywhere
    ASSERT_EQ(network.use_count(), 1);
  }

 protected:
  std::shared_ptr<olp::client::OlpClientSettings> settings_;
  std::shared_ptr<NetworkMock> network_mock_;
};

TEST_F(VersionedLayerClientOfflineTest, GetDataFromPartitionAsync) {
  settings_->task_scheduler =
      olp::client::OlpClientSettingsFactory::CreateDefaultTaskScheduler(1);

  auto catalog = olp::client::HRN::FromString(
      CustomParameters::getArgument("dataservice_read_test_catalog"));
  auto layer = CustomParameters::getArgument("dataservice_read_test_layer");
  auto version = std::atoi(
      CustomParameters::getArgument("dataservice_read_test_layer_version")
          .c_str());

  auto catalog_client =
      std::make_unique<olp::dataservice::read::VersionedLayerClient>(
          *settings_, catalog, layer, version);
  ASSERT_TRUE(catalog_client);

  std::promise<DataResponse> promise;
  std::future<DataResponse> future = promise.get_future();
  auto partition =
      CustomParameters::getArgument("dataservice_read_test_partition");
  auto token = catalog_client->GetDataByPartitionId(
      partition,
      [&promise](DataResponse response) { promise.set_value(response); });

  //ASSERT_NE(future.wait_for(kWaitTimeout), std::future_status::timeout);
  DataResponse response = future.get();

  ASSERT_TRUE(response.IsSuccessful()) << response.GetError().GetMessage();
  ASSERT_TRUE(response.GetResult() != nullptr);
  ASSERT_NE(response.GetResult()->size(), 0u);
}

TEST_F(VersionedLayerClientOfflineTest, GetDataFromPartitionSync) {
  auto catalog = olp::client::HRN::FromString(
      CustomParameters::getArgument("dataservice_read_test_catalog"));
  auto layer = CustomParameters::getArgument("dataservice_read_test_layer");
  auto version = 0;

  auto catalog_client =
      std::make_unique<olp::dataservice::read::VersionedLayerClient>(
          *settings_, catalog, layer, version);
  ASSERT_TRUE(catalog_client);

  DataResponse response;
  auto partition =
      CustomParameters::getArgument("dataservice_read_test_partition");
  auto token = catalog_client->GetDataByPartitionId(
      partition,
      [&response](DataResponse resp) { response = std::move(resp); });
  ASSERT_TRUE(response.IsSuccessful());
  ASSERT_TRUE(response.GetResult() != nullptr);
  ASSERT_NE(response.GetResult()->size(), 0u);
}

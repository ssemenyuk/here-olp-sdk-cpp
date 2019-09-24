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

using NetworkCallback = std::function<olp::http::SendOutcome(
    olp::http::NetworkRequest, olp::http::Network::Payload,
    olp::http::Network::Callback, olp::http::Network::HeaderCallback,
    olp::http::Network::DataCallback)>;

using CancelCallback = std::function<void(olp::http::RequestId)>;

struct MockedResponseInformation {
  int status;
  const char* data;
};

std::tuple<olp::http::RequestId, NetworkCallback, CancelCallback>
generateNetworkMocks(std::shared_ptr<std::promise<void>> pre_signal,
                     std::shared_ptr<std::promise<void>> wait_for_signal,
                     MockedResponseInformation response_information,
                     std::shared_ptr<std::promise<void>> post_signal =
                         std::make_shared<std::promise<void>>()) {
  using namespace olp::http;

  static std::atomic<RequestId> s_request_id{
      static_cast<RequestId>(RequestIdConstants::RequestIdMin)};

  olp::http::RequestId request_id = s_request_id.fetch_add(1);

  auto completed = std::make_shared<std::atomic_bool>(false);

  // callback is generated when the send method is executed, in order to receive
  // the cancel callback, we need to pass it to store it somewhere and share
  // with cancel mock.
  auto callback_placeholder = std::make_shared<olp::http::Network::Callback>();

  auto mocked_send =
      [request_id, completed, pre_signal, wait_for_signal, response_information,
       post_signal, callback_placeholder](
          NetworkRequest request, Network::Payload payload,
          Network::Callback callback, Network::HeaderCallback,
          Network::DataCallback data_callback) -> olp::http::SendOutcome {
    *callback_placeholder = callback;

    auto mocked_network_block = [request, pre_signal, wait_for_signal,
                                 completed, callback, response_information,
                                 post_signal, payload]() {
      // emulate a small response delay
      std::this_thread::sleep_for(std::chrono::milliseconds(50));

      // notify waiting thread that we reached the network code
      pre_signal->set_value();

      // wait until test cancel request during execution
      wait_for_signal->get_future().get();

      // in the case request was not canceled return the expected payload
      if (!completed->exchange(true)) {
        const auto data_len = strlen(response_information.data);
        payload->write(response_information.data, data_len);
        callback(NetworkResponse().WithStatus(response_information.status));
      }

      // notify that request finished
      post_signal->set_value();
    };

    // simulate that network code is actually running in the background.
    std::thread(std::move(mocked_network_block)).detach();

    return SendOutcome(request_id);
  };

  auto mocked_cancel = [completed,
                        callback_placeholder](olp::http::RequestId id) {
    if (!completed->exchange(true)) {
      auto cancel_code = static_cast<int>(ErrorCode::CANCELLED_ERROR);
      (*callback_placeholder)(
          NetworkResponse().WithError("Cancelled").WithStatus(cancel_code));
    }
  };

  return std::make_tuple(request_id, std::move(mocked_send),
                         std::move(mocked_cancel));
}

std::function<olp::http::SendOutcome(
    olp::http::NetworkRequest request, olp::http::Network::Payload payload,
    olp::http::Network::Callback callback,
    olp::http::Network::HeaderCallback header_callback,
    olp::http::Network::DataCallback data_callback)>
ReturnHttpResponse(
    olp::http::NetworkResponse response, const std::string& response_body,
    std::shared_ptr<std::promise<void>> pre_signal = nullptr,
    std::shared_ptr<std::promise<void>> wait_for_signal = nullptr,
    std::shared_ptr<std::promise<void>> post_signal = nullptr) {
  return [=](olp::http::NetworkRequest request,
             olp::http::Network::Payload payload,
             olp::http::Network::Callback callback,
             olp::http::Network::HeaderCallback header_callback,
             olp::http::Network::DataCallback data_callback)
             -> olp::http::SendOutcome {
    std::thread([=]() {
      // notify waiting thread that we reached the network code
      if (pre_signal) {
        pre_signal->set_value();
      }

      // wait until test cancel request during execution
      if (wait_for_signal) {
        wait_for_signal->get_future().get();
      }

      *payload << response_body;
      callback(response);

      // notify that request finished
      if (post_signal) {
        post_signal->set_value();
      }
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
         std::string(url).substr(0, 20) == arg.GetUrl().substr(0, 20) &&
         (!arg.GetBody() || arg.GetBody()->empty());
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

  EXPECT_CALL(*network_mock_, Send(_, _, _, _, _))
      .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(200),
                                   HTTP_RESPONSE_LOOKUP_QUERY))
      .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(200),
                                   HTTP_RESPONSE_PARTITION_269))
      .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(200),
                                   HTTP_RESPONSE_LOOKUP_BLOB))
      .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(200),
                                   HTTP_RESPONSE_BLOB_DATA_269));

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

  // ASSERT_NE(future.wait_for(kWaitTimeout), std::future_status::timeout);
  DataResponse response = future.get();

  ASSERT_TRUE(response.IsSuccessful()) << response.GetError().GetMessage();
  ASSERT_TRUE(response.GetResult() != nullptr);
  ASSERT_NE(response.GetResult()->size(), 0u);
}

TEST_F(VersionedLayerClientOfflineTest, GetDataFromPartitionSync) {
  EXPECT_CALL(*network_mock_, Send(_, _, _, _, _))
      .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(200),
                                   HTTP_RESPONSE_LOOKUP_QUERY))
      .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(200),
                                   HTTP_RESPONSE_PARTITION_269))
      .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(200),
                                   HTTP_RESPONSE_LOOKUP_BLOB))
      .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(200),
                                   HTTP_RESPONSE_BLOB_DATA_269));

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

TEST_F(VersionedLayerClientOfflineTest, GetDataFromPartitionCancelLookup) {
  settings_->task_scheduler =
      olp::client::OlpClientSettingsFactory::CreateDefaultTaskScheduler(1);

  auto waitForCancel = std::make_shared<std::promise<void>>();
  auto pauseForCancel = std::make_shared<std::promise<void>>();
  olp::http::RequestId request_id;
  NetworkCallback send_mock;
  CancelCallback cancel_mock;
  std::tie(request_id, send_mock, cancel_mock) = generateNetworkMocks(
      waitForCancel, pauseForCancel, {200, HTTP_RESPONSE_LOOKUP_QUERY});

  EXPECT_CALL(*network_mock_, Send(_, _, _, _, _))
      .WillOnce(testing::Invoke(std::move(send_mock)));

  EXPECT_CALL(*network_mock_, Cancel(_))
      .WillOnce(testing::Invoke(std::move(cancel_mock)));

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

  waitForCancel->get_future().get();
  token.cancel();
  pauseForCancel->set_value();

  // ASSERT_NE(future.wait_for(kWaitTimeout), std::future_status::timeout);
  DataResponse response = future.get();

  ASSERT_TRUE(!response.IsSuccessful()) << response.GetError().GetMessage();
  ASSERT_TRUE(response.GetResult() == nullptr);
}

TEST_F(VersionedLayerClientOfflineTest, GetDataFromPartitionCancelPartition) {
  settings_->task_scheduler =
      olp::client::OlpClientSettingsFactory::CreateDefaultTaskScheduler(1);

  auto waitForCancel = std::make_shared<std::promise<void>>();
  auto pauseForCancel = std::make_shared<std::promise<void>>();

  olp::http::RequestId request_id;
  NetworkCallback send_mock;
  CancelCallback cancel_mock;
  std::tie(request_id, send_mock, cancel_mock) = generateNetworkMocks(
      waitForCancel, pauseForCancel, {200, HTTP_RESPONSE_PARTITION_269});

  EXPECT_CALL(*network_mock_, Send(_, _, _, _, _))
      .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(200),
                                   HTTP_RESPONSE_LOOKUP_QUERY))
      .WillOnce(testing::Invoke(std::move(send_mock)));

  EXPECT_CALL(*network_mock_, Cancel(_))
      .WillOnce(testing::Invoke(std::move(cancel_mock)));

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

  waitForCancel->get_future().get();
  token.cancel();
  pauseForCancel->set_value();

  // ASSERT_NE(future.wait_for(kWaitTimeout), std::future_status::timeout);
  DataResponse response = future.get();

  ASSERT_TRUE(!response.IsSuccessful()) << response.GetError().GetMessage();
  ASSERT_TRUE(response.GetResult() == nullptr);
}

TEST_F(VersionedLayerClientOfflineTest, GetDataFromPartitionCancelLookupBlob) {
  settings_->task_scheduler =
      olp::client::OlpClientSettingsFactory::CreateDefaultTaskScheduler(1);

  auto waitForCancel = std::make_shared<std::promise<void>>();
  auto pauseForCancel = std::make_shared<std::promise<void>>();

  olp::http::RequestId request_id;
  NetworkCallback send_mock;
  CancelCallback cancel_mock;
  std::tie(request_id, send_mock, cancel_mock) = generateNetworkMocks(
      waitForCancel, pauseForCancel, {200, HTTP_RESPONSE_LOOKUP_BLOB});

  EXPECT_CALL(*network_mock_, Send(_, _, _, _, _))
      .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(200),
                                   HTTP_RESPONSE_LOOKUP_QUERY))
      .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(200),
                                   HTTP_RESPONSE_PARTITION_269))
      .WillOnce(testing::Invoke(std::move(send_mock)));

  EXPECT_CALL(*network_mock_, Cancel(request_id))
      .WillOnce(testing::Invoke(std::move(cancel_mock)));

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

  waitForCancel->get_future().get();
  token.cancel();
  pauseForCancel->set_value();

  // ASSERT_NE(future.wait_for(kWaitTimeout), std::future_status::timeout);
  DataResponse response = future.get();

  ASSERT_TRUE(!response.IsSuccessful()) << response.GetError().GetMessage();
  ASSERT_TRUE(response.GetResult() == nullptr);
}

TEST_F(VersionedLayerClientOfflineTest, GetDataFromPartitionCancelBlobData) {
  settings_->task_scheduler =
      olp::client::OlpClientSettingsFactory::CreateDefaultTaskScheduler(1);

  auto waitForCancel = std::make_shared<std::promise<void>>();
  auto pauseForCancel = std::make_shared<std::promise<void>>();
  olp::http::RequestId request_id;
  NetworkCallback send_mock;
  CancelCallback cancel_mock;
  std::tie(request_id, send_mock, cancel_mock) = generateNetworkMocks(
      waitForCancel, pauseForCancel, {200, HTTP_RESPONSE_BLOB_DATA_269});

  EXPECT_CALL(*network_mock_, Send(_, _, _, _, _))
      .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(200),
                                   HTTP_RESPONSE_LOOKUP_QUERY))
      .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(200),
                                   HTTP_RESPONSE_PARTITION_269))
      .WillOnce(ReturnHttpResponse(olp::http::NetworkResponse().WithStatus(200),
                                   HTTP_RESPONSE_LOOKUP_BLOB))
      .WillOnce(testing::Invoke(std::move(send_mock)));

  EXPECT_CALL(*network_mock_, Cancel(_))
      .WillOnce(testing::Invoke(std::move(cancel_mock)));

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

  waitForCancel->get_future().get();
  token.cancel();
  pauseForCancel->set_value();

  // ASSERT_NE(future.wait_for(kWaitTimeout), std::future_status::timeout);
  DataResponse response = future.get();

  ASSERT_TRUE(!response.IsSuccessful()) << response.GetError().GetMessage();
  ASSERT_TRUE(response.GetResult() == nullptr);
}

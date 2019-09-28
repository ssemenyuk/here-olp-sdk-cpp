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

#include "../src/VersionedLayerClientImpl.h"
#include "../src/generated/parser/PartitionsParser.h"
#include "../src/repositories/ApiCacheRepository.h"
#include "olp/core/cache/KeyValueCache.h"
#include "olp/core/generated/parser/JsonParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace olp;
using namespace client;
using namespace dataservice::read;

class VersionedLayerClientImplMock : public VersionedLayerClientImpl {
 public:
  VersionedLayerClientImplMock(HRN catalog, std::string layer,
                               OlpClientSettings client_settings)
      : VersionedLayerClientImpl(std::move(catalog), std::move(layer),
                                 std::move(client_settings)) {}

  using VersionedLayerClientImpl::GetBlobData;
  using VersionedLayerClientImpl::GetData;
  using VersionedLayerClientImpl::GetLatestVersion;
  using VersionedLayerClientImpl::GetPartitionById;
  using VersionedLayerClientImpl::LookupApi;
};

class CacheMock : public cache::KeyValueCache {
 public:
  MOCK_METHOD(bool, Put,
              (const std::string&, const boost::any&, const cache::Encoder&,
               time_t),
              (override));

  MOCK_METHOD(bool, Put,
              (const std::string&,
               const std::shared_ptr<std::vector<unsigned char>>,
               time_t expiry),
              (override));

  MOCK_METHOD(boost::any, Get, (const std::string&, const cache::Decoder&),
              (override));

  MOCK_METHOD(std::shared_ptr<std::vector<unsigned char>>, Get,
              (const std::string&), (override));

  MOCK_METHOD(bool, Remove, (const std::string&), (override));

  MOCK_METHOD(bool, RemoveKeysWithPrefix, (const std::string&), (override));
};

class NetworkMock : public http::Network {
 public:
  MOCK_METHOD(http::SendOutcome, Send,
              (http::NetworkRequest, Payload, Callback, HeaderCallback,
               DataCallback),
              (override));

  MOCK_METHOD(void, Cancel, (http::RequestId), (override));
};

TEST(VersionedLayerClient, LookupApiMethod) {
  using namespace testing;
  using testing::Return;

  auto cache = std::make_shared<testing::StrictMock<CacheMock>>();
  auto network = std::make_shared<testing::StrictMock<NetworkMock>>();

  const std::string catalog = "hrn:here:data:::hereos-internal-test-v2";

  OlpClientSettings settings;
  settings.cache = cache;
  settings.network_request_handler = network;
  VersionedLayerClientImplMock client(HRN::FromString(catalog), "test_layer",
                                      settings);

  const std::string service_name = "random_service";
  const std::string service_url = "http://random_service.com";
  const std::string service_version = "v8";
  const std::string cache_key =
      catalog + "::" + service_name + "::" + service_version + "::api";
  {
    SCOPED_TRACE("Fetch from cache");
    EXPECT_CALL(*cache, Get(cache_key, _))
        .Times(1)
        .WillOnce(Return(service_url));
    client::CancellationContext context;
    auto response = client.LookupApi(context, service_name, service_version,
                                     FetchOptions::CacheOnly);
    EXPECT_TRUE(response.IsSuccessful());
    EXPECT_EQ(response.GetResult().GetBaseUrl(), service_url);
    Mock::VerifyAndClearExpectations(cache.get());
  }
  {
      // SCOPED_TRACE("Fetch from network");
      // EXPECT_CALL(*cache, Get(cache_key, _))
      //    .Times(1)
      //    .WillOnce(Return(boost::none));
      //// TODO
      // client::CancellationContext context;
      // auto response = client.LookupApi(context, service_name,
      // service_version); EXPECT_TRUE(response.IsSuccessful());
      // EXPECT_EQ(response.GetResult().GetBaseUrl(), service_url);
      // Mock::VerifyAndClearExpectations(cache.get());
      // TODO: expect push to the cache
  } {
      // SCOPED_TRACE("Network error propagated to the user");
  } {
      // SCOPED_TRACE("Network request cancelled by the user");
  } {
    // SCOPED_TRACE("Network error propagated to the user");
  }
}

TEST(VersionedLayerClient, QueryApiMethod) {
  using namespace testing;
  using testing::Return;

  auto cache = std::make_shared<testing::StrictMock<CacheMock>>();
  auto network = std::make_shared<testing::StrictMock<NetworkMock>>();

  const std::string catalog = "hrn:here:data:::hereos-internal-test-v2";
  const std::string layer_id = "test_layer";
  const std::string partition_id = "269";
  const int version = 4;

  OlpClientSettings settings;
  settings.cache = cache;
  settings.network_request_handler = network;

  VersionedLayerClientImplMock client(HRN::FromString(catalog), layer_id,
                                      settings);

  dataservice::read::DataRequest request;
  request.WithPartitionId(partition_id).WithVersion(version);

  const std::string cache_key = catalog + "::" + layer_id +
                                "::" + partition_id +
                                "::" + std::to_string(version) + "::partition";

  {
    SCOPED_TRACE("Fetch from cache [CacheOnly] positive");

    const std::string query_cache_response =
        R"jsonString({"version":4,"partition":"269","layer":"testlayer","dataHandle":"qwerty"})jsonString";

    EXPECT_CALL(*cache, Get(cache_key, _))
        .Times(1)
        .WillOnce(
            Return(parser::parse<model::Partition>(query_cache_response)));

    client::CancellationContext context;
    auto response =
        client.GetPartitionById(context, request.WithFetchOption(CacheOnly));

    ASSERT_TRUE(response.IsSuccessful());
    const auto& result = response.GetResult();
    const auto& partitions = result.GetPartitions();
    EXPECT_EQ(partitions.size(), 1);
    EXPECT_EQ(partitions[0].GetDataHandle(), "qwerty");
    EXPECT_EQ(partitions[0].GetVersion().value_or(0), version);
    EXPECT_EQ(partitions[0].GetPartition(), partition_id);
    Mock::VerifyAndClearExpectations(cache.get());
  }
  {
    SCOPED_TRACE("Fetch from cache [CacheOnly] negative");

    EXPECT_CALL(*cache, Get(cache_key, _))
        .Times(1)
        .WillOnce(Return(boost::any()));

    client::CancellationContext context;
    auto response =
        client.GetPartitionById(context, request.WithFetchOption(CacheOnly));

    ASSERT_FALSE(response.IsSuccessful());
    const auto& result = response.GetError();
    EXPECT_EQ(result.GetErrorCode(), ErrorCode::NotFound);
    Mock::VerifyAndClearExpectations(cache.get());
  }
}
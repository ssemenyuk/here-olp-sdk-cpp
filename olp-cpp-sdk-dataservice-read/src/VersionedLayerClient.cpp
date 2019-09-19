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

#include <olp/dataservice/read/VersionedLayerClient.h>

#include <olp/core/client/OlpClientFactory.h>
#include <olp/core/client/OlpClientSettings.h>
#include <olp/core/thread/TaskScheduler.h>

#include <olp/core/client/OlpClient.h>
#include <olp/core/context/Context.h>

#include "generated/api/BlobApi.h"
#include "generated/api/MetadataApi.h"

#include "olp/dataservice/read/Condition.h"

#include "ApiClientLookup.h"
#include "generated/model/Api.h"

#include <algorithm>

namespace olp {
namespace dataservice {
namespace read {

VersionedLayerClient::VersionedLayerClient(
    std::shared_ptr<olp::client::OlpClientSettings> client_settings,
    client::HRN hrn, std::string layer_id, std::int64_t layer_version)
    : client_settings_(client_settings),
      hrn_(std::move(hrn)),
      layer_id_(std::move(layer_id)),
      layer_version_(layer_version) {
  // TODO: Consider move olp client as constructor argument
  // TODO: nullptr
  olp_client_ = olp::client::OlpClientFactory::Create(*client_settings);
}

olp::client::CancellationToken VersionedLayerClient::GetDataByPartitionId(
    const std::string& partition_id, DataResponseCallback callback) {
  olp::client::CancellationContext context;
  olp::client::CancellationToken token(
      [=]() mutable { context.CancelOperation(); });
  Condition condition(context);
  auto wait_and_check = [&] {
    if (!condition.Wait() && !context.IsCancelled()) {
      callback({{olp::client::ErrorCode::RequestTimeout,
                 "Network request timed out.", true}});
      return false;
    }
    return true;
  };

  // Step 1. Get query service

  ApiClientLookup::ApiClientResponse apis_response;

  context.ExecuteOrCancelled([&]() {
    return ApiClientLookup::LookupApiClient(
        olp_client_, "query", "v1", hrn_,
        [&](ApiClientLookup::ApiClientResponse response) {
          apis_response = std::move(response);
          condition.Notify();
        });
  });

  //TODO: collapse these 2x4 checks into a lambda calls
  if (!wait_and_check()) {
    return token;
  }
  if (!apis_response.IsSuccessful()) {
    callback({{olp::client::ErrorCode::ServiceUnavailable,
               "Query request unsuccessful.", true}});
    return token;
  }

  auto query_client = apis_response.GetResult();

  // Step 2. Use query service to acquire metadata

  MetadataApi::PartitionsResponse partitions_response;
  context.ExecuteOrCancelled([&]() {
    return olp::dataservice::read::MetadataApi::GetPartitions(
        query_client, layer_id_, layer_version_, boost::none, boost::none,
        boost::none, [&](MetadataApi::PartitionsResponse response) {
          partitions_response = std::move(response);
          condition.Notify();
        });
  });

  if (!wait_and_check()) {
    return token;
  }
  if (!partitions_response.IsSuccessful()) {
    callback({{olp::client::ErrorCode::ServiceUnavailable,
               "Metadata request unsuccessful.", true}});
    return token;
  }

  // Step 3. Get blob service

  context.ExecuteOrCancelled([&]() {
    return ApiClientLookup::LookupApiClient(
        olp_client_, "blob", "v1", hrn_,
        [&](ApiClientLookup::ApiClientResponse response) {
          apis_response = std::move(response);
          condition.Notify();
        });
  });

  if (!wait_and_check()) {
    return token;
  }
  if (!apis_response.IsSuccessful()) {
    callback({{olp::client::ErrorCode::ServiceUnavailable,
               "Blob request unsuccessful.", true}});
    return token;
  }

  // Step 4. Use metadata in blob service to acquire data for user

  auto partitions = partitions_response.GetResult();
  auto partition_it = std::find_if(partitions.GetPartitions().begin(),
                                   partitions.GetPartitions().end(),
                                   [&](const model::Partition& p) {
                                     return p.GetPartition() == partition_id;
                                   });
  if (partition_it == partitions.GetPartitions().end()) {
    callback(DataResponse(model::Data()));
    return token;
  }
  auto data_handle = partition_it->GetDataHandle();
  auto blob_client = apis_response.GetResult();
  BlobApi::DataResponse data_response;
  context.ExecuteOrCancelled([&]() {
    return olp::dataservice::read::BlobApi::GetBlob(
        blob_client, layer_id_, data_handle, boost::none, boost::none,
        [&](BlobApi::DataResponse response) {
          data_response = std::move(response);
          condition.Notify();
        });
  });

  if (!wait_and_check()) {
    return token;
  }
  if (!data_response.IsSuccessful()) {
    callback({{olp::client::ErrorCode::ServiceUnavailable,
               "Data request unsuccessful.", true}});
    return token;
  }

  auto data = data_response.GetResult();
  callback(data);
  return token;
}

}  // namespace read
}  // namespace dataservice
}  // namespace olp

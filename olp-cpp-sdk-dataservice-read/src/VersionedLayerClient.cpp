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
#include <olp/core/thread/TaskScheduler.h>

#include <olp/core/client/OlpClient.h>
#include <olp/core/context/Context.h>

#include "generated/api/BlobApi.h"
#include "generated/api/QueryApi.h"

#include "olp/dataservice/read/Condition.h"

#include "ApiClientLookup.h"
#include "generated/model/Api.h"

#include "repositories/ExecuteOrSchedule.inl"

#include "olp/core/logging/Log.h"

#include <algorithm>

namespace olp {
namespace dataservice {
namespace read {

  namespace {
  constexpr auto kLogTag = "VersionedLayerClient";
}
class VersionedLayerClient::Impl final {
 public:
  Impl(client::OlpClientSettings client_settings, client::HRN hrn,
       std::string layer_id, std::int64_t layer_version)
      : olp_client_(olp::client::OlpClientFactory::Create(client_settings)),
        client_settings_(std::move(client_settings)),
        hrn_(std::move(hrn)),
        layer_id_(std::move(layer_id)),
        layer_version_(layer_version) {}

  ~Impl() {
    OLP_SDK_LOG_DEBUG_F(kLogTag, "~dtor");
  }

  olp::client::CancellationToken GetDataByPartitionId(
      const std::string& partition_id, DataResponseCallback callback) {
    auto context = std::make_shared<olp::client::CancellationContext>();
    olp::client::CancellationToken token(
        [context]() mutable { context->CancelOperation(); });

    auto olp_client = olp_client_;
    auto hrn = hrn_;
    auto layer_id = layer_id_;
    auto layer_version = layer_version_;

    auto task = [context, callback, partition_id, olp_client, hrn, layer_id,
                 layer_version]() {
      Condition condition(*context);
      auto wait_and_check = [&] {
        if (!condition.Wait()) {
          callback({{olp::client::ErrorCode::RequestTimeout,
                     "Network request timed out.", true}});
          return false;
        }
        if (context->IsCancelled()) {
          callback({{olp::client::ErrorCode::RequestTimeout,
                     "Network request cancelled.", true}});
          return false;
        }
        return true;
      };

      // Step 1. Get query service
      OLP_SDK_LOG_DEBUG_F(kLogTag, "step 1");

      ApiClientLookup::ApiClientResponse apis_response;

      context->ExecuteOrCancelled([&]() {
        return ApiClientLookup::LookupApiClient(
            olp_client, "query", "v1", hrn,
            [&](ApiClientLookup::ApiClientResponse response) {
              OLP_SDK_LOG_DEBUG_F(kLogTag, "step 1 completed");
              apis_response = std::move(response);
              condition.Notify();
            });
      });

      // TODO: collapse these 2x4 checks into a lambda calls
      if (context->IsCancelled()) {
        callback({{olp::client::ErrorCode::Cancelled,
                   "Request cancelled.", true}});
        return;
      }
      if (!wait_and_check()) {
        return;
      }
      if (!apis_response.IsSuccessful()) {
        callback({{olp::client::ErrorCode::ServiceUnavailable,
                   "Query request unsuccessful.", true}});
        return;
      }

      auto query_client = apis_response.GetResult();

      // Step 2. Use query service to acquire metadata
      OLP_SDK_LOG_DEBUG_F(kLogTag, "step 2");

      std::vector<std::string> paritions;
      paritions.push_back(partition_id);
      QueryApi::PartitionsResponse partitions_response;
      context->ExecuteOrCancelled([&]() {
        return olp::dataservice::read::QueryApi::GetPartitionsbyId(
            query_client, layer_id, paritions, layer_version, boost::none,
            boost::none, [&](QueryApi::PartitionsResponse response) {
              OLP_SDK_LOG_DEBUG_F(kLogTag, "step 2 completed");
              partitions_response = std::move(response);
              condition.Notify();
            });
      });

      if (context->IsCancelled()) {
        callback({{olp::client::ErrorCode::Cancelled,
                   "Request cancelled.", true}});
        return;
      }
      if (!wait_and_check()) {
        return;
      }
      if (!partitions_response.IsSuccessful()) {
        callback({{olp::client::ErrorCode::ServiceUnavailable,
                   "Metadata request unsuccessful.", true}});
        return;
      }

      // Step 3. Get blob service
      OLP_SDK_LOG_DEBUG_F(kLogTag, "step 3");

      context->ExecuteOrCancelled([&]() {
        return ApiClientLookup::LookupApiClient(
            olp_client, "blob", "v1", hrn,
            [&](ApiClientLookup::ApiClientResponse response) {
              OLP_SDK_LOG_DEBUG_F(kLogTag, "step 3 completed");
              apis_response = std::move(response);
              condition.Notify();
            });
      });

      if (context->IsCancelled()) {
        callback({{olp::client::ErrorCode::Cancelled,
                   "Request cancelled.", true}});
        return;
      }
      if (!wait_and_check()) {
        return;
      }
      if (!apis_response.IsSuccessful()) {
        callback({{olp::client::ErrorCode::ServiceUnavailable,
                   "Blob request unsuccessful.", true}});
        return;
      }
      if (context->IsCancelled()) {
        callback({{olp::client::ErrorCode::Cancelled,
                   "Request cancelled.", true}});
        return;
      }
      // Step 4. Use metadata in blob service to acquire data for user
      OLP_SDK_LOG_DEBUG_F(kLogTag, "step 4");

      auto partitions = partitions_response.GetResult().GetPartitions();
      if (partitions.empty()) {
        callback(DataResponse(model::Data()));
        return;
      }
      auto data_handle = partitions.front().GetDataHandle();
      auto blob_client = apis_response.GetResult();
      BlobApi::DataResponse data_response;
      context->ExecuteOrCancelled([&]() {
        return olp::dataservice::read::BlobApi::GetBlob(
            blob_client, layer_id, data_handle, boost::none, boost::none,
            [&](BlobApi::DataResponse response) {
              OLP_SDK_LOG_DEBUG_F(kLogTag, "step 4 completed");
              data_response = std::move(response);
              condition.Notify();
            });
      });

      if (context->IsCancelled()) {
        callback({{olp::client::ErrorCode::Cancelled,
                   "Request cancelled.", true}});
        return;
      }
      if (!wait_and_check()) {
        return;
      }
      if (!data_response.IsSuccessful()) {
        callback({{olp::client::ErrorCode::ServiceUnavailable,
                   "Data request unsuccessful.", true}});
        return;
      }

      auto data = data_response.GetResult();
      callback(data);
    };

    repository::ExecuteOrSchedule(&client_settings_, task);

    return token;
  }

 protected:
  std::shared_ptr<olp::client::OlpClient> olp_client_;
  olp::client::OlpClientSettings client_settings_;
  olp::client::HRN hrn_;
  std::string layer_id_;
  std::int64_t layer_version_;
};

////////////////////////////////////////////////////////////////////////////////////////////////////

VersionedLayerClient::VersionedLayerClient(
    client::OlpClientSettings client_settings, client::HRN hrn,
    std::string layer_id, std::int64_t layer_version)
    : impl_(std::make_unique<Impl>(std::move(client_settings), std::move(hrn),
                                   std::move(layer_id), layer_version)) {}

// Impl was forward-declared in header, but in implementation compiler clearly
// knows how to destroy unique_ptr to it.
VersionedLayerClient::~VersionedLayerClient() = default;

olp::client::CancellationToken VersionedLayerClient::GetDataByPartitionId(
    const std::string& partition_id, DataResponseCallback callback) {
  return impl_->GetDataByPartitionId(partition_id, callback);
}

}  // namespace read
}  // namespace dataservice
}  // namespace olp

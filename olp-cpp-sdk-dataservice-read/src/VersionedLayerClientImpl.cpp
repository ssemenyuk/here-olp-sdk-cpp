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

#include "VersionedLayerClientImpl.h"

#include "ApiClientLookup.h"
#include "Condition.h"

#include "olp/core/client/CancellationContext.h"
#include "olp/core/logging/Log.h"
#include "olp/core/thread/TaskScheduler.h"
#include "repositories/ApiCacheRepository.h"
#include "repositories/CatalogCacheRepository.h"
#include "repositories/DataCacheRepository.h"
#include "repositories/ExecuteOrSchedule.inl"
#include "repositories/PartitionsCacheRepository.h"

namespace olp {
namespace dataservice {
namespace read {
namespace {
constexpr auto kLogTag = "VersionedLayerClientImpl";
}

VersionedLayerClientImpl::VersionedLayerClientImpl(
    client::HRN catalog, std::string layer,
    client::OlpClientSettings client_settings)
    : catalog_(std::move(catalog)),
      layer_(std::move(layer)),
      settings_(std::move(client_settings)),
      pending_requests_(std::make_shared<PendingRequests>()) {
  safe_settings_ = settings_;
  safe_settings_.task_scheduler = nullptr;
}

VersionedLayerClientImpl::~VersionedLayerClientImpl() {
  pending_requests_->CancelPendingRequests();
}

client::CancellationToken VersionedLayerClientImpl::GetData(
    DataRequest data_request, Callback<model::Data> callback) {
  auto add_task = [this](DataRequest data_request,
                         Callback<model::Data> callback) {
    using namespace client;
    CancellationContext context;
    CancellationToken token([=]() mutable { context.CancelOperation(); });

    auto pending_requests = pending_requests_;
    int64_t request_key = pending_requests->GenerateRequestPlaceholder();

    pending_requests->Insert(token, request_key);

    repository::ExecuteOrSchedule(&settings_, [=]() {
      auto response = GetData(context, data_request);
      pending_requests->Remove(request_key);
      if (callback) {
        callback(std::move(response));
      }
    });

    return token;
  };

  auto fetch_option = data_request.GetFetchOption();
  if (fetch_option == CacheWithUpdate) {
    auto cache_token =
        add_task(data_request.WithFetchOption(CacheOnly), callback);
    auto online_token =
        add_task(data_request.WithFetchOption(OnlineIfNotFound), nullptr);
    return client::CancellationToken([cache_token, online_token]() {
      cache_token.cancel();
      online_token.cancel();
    });
  } else {
    return add_task(data_request, callback);
  }
}

VersionedLayerClientImpl::CallbackResponse<model::Data>
VersionedLayerClientImpl::GetData(client::CancellationContext context,
                                  DataRequest request) const {
  if (context.IsCancelled()) {
    return client::ApiError(client::ErrorCode::Cancelled,
                            "Operation cancelled.");
  }

  if (!request.GetPartitionId() && !request.GetDataHandle()) {
    auto key = request.CreateKey();
    OLP_SDK_LOG_INFO_F(kLogTag, "getData for '%s' failed", key.c_str());
    return client::ApiError(client::ErrorCode::InvalidArgument,
                            "A data handle or a partition id must be defined.");
  }

  if (!request.GetDataHandle()) {
    if (!request.GetVersion()) {
      auto version = GetLatestVersion(context, request);
      if (!version.IsSuccessful()) {
        return version.GetError();
      }

      request.WithVersion(version.GetResult().GetVersion());
    }

    auto query_response = GetPartitionById(context, request);

    if (!query_response.IsSuccessful()) {
      return query_response.GetError();
    }

    const auto& result = query_response.GetResult();
    const auto& partitions = result.GetPartitions();
    if (partitions.empty()) {
      return client::ApiError(client::ErrorCode::NotFound,
                              "Requested partition not found.");
    }

    request.WithDataHandle(partitions.at(0).GetDataHandle());
  }

  return GetBlobData(context, request);
}

VersionedLayerClientImpl::CallbackResponse<client::OlpClient>
VersionedLayerClientImpl::LookupApi(
    client::CancellationContext cancellation_context, std::string service,
    std::string service_version, FetchOptions options) const {
  repository::ApiCacheRepository repository(catalog_, safe_settings_.cache);
  using namespace client;
  if (options != OnlineOnly) {
    auto url = repository.Get(service, service_version);
    if (url) {
      OLP_SDK_LOG_INFO_F(kLogTag, "getApiClient(%s, %s) -> from cache",
                         service.c_str(), service_version.c_str());
      OlpClient client;
      client.SetSettings(safe_settings_);
      client.SetBaseUrl(*url);
      return std::move(client);
    } else if (options == CacheOnly) {
      return ApiError(ErrorCode::NotFound,
                      "Cache only resource not found in cache (loopup api).");
    }
  }

  auto client = std::make_shared<client::OlpClient>();
  client->SetSettings(safe_settings_);

  Condition condition(cancellation_context);

  // when the network operation took too much time we cancel it and exit
  // execution, to make sure that network callback will not access dangling
  // references we protect them with atomic bool flag.
  auto interest_flag = std::make_shared<std::atomic_bool>(true);

  ApiClientLookup::ApiClientResponse api_response;
  cancellation_context.ExecuteOrCancelled2([&]() {
    return ApiClientLookup::LookupApiClient(
        client, service, service_version, catalog_,
        [&,
         interest_flag](ApiClientLookup::ApiClientResponse response) mutable {
          if (interest_flag->exchange(false)) {
            api_response = std::move(response);
            condition.Notify();
          }
        });
  });

  if (!condition.Wait()) {
    // We are just about exit the execution.
    interest_flag->store(false);

    if (cancellation_context.IsCancelled()) {
      // We can't use api response here because it could potentially be
      // uninitialized.
      return client::ApiError(client::ErrorCode::Cancelled,
                              "Operation cancelled.");
    } else {
      cancellation_context.CancelOperation();
      return client::ApiError(client::ErrorCode::RequestTimeout,
                              "Network request timed out.");
    }
  }

  if (api_response.IsSuccessful()) {
    OLP_SDK_LOG_INFO_F(kLogTag, "getApiClient(%s, %s) -> into cache",
                       service.c_str(), service_version.c_str());
    repository.Put(service, service_version,
                   api_response.GetResult().GetBaseUrl());
  }

  return api_response;
}

MetadataApi::CatalogVersionResponse VersionedLayerClientImpl::GetLatestVersion(
    client::CancellationContext cancellation_context,
    const DataRequest& data_request) const {
  using namespace client;

  repository::CatalogCacheRepository repository(catalog_, safe_settings_.cache);

  auto fetch_option = data_request.GetFetchOption();
  if (fetch_option != OnlineOnly) {
    auto cached_version = repository.GetVersion();
    if (cached_version) {
      auto key = data_request.CreateKey();
      OLP_SDK_LOG_INFO_F(kLogTag, "cache catalog '%s' found!", key.c_str());
      return cached_version.value();
    } else if (fetch_option == CacheOnly) {
      auto key = data_request.CreateKey();
      OLP_SDK_LOG_INFO_F(kLogTag, "cache catalog '%s' not found!", key.c_str());
      return ApiError(
          ErrorCode::NotFound,
          "Cache only resource not found in cache (catalog version).");
    }
  }

  auto metadata_api =
      LookupApi(cancellation_context, "metadata", "v1", fetch_option);
  if (!metadata_api.IsSuccessful()) {
    return metadata_api.GetError();
  }

  const client::OlpClient& client = metadata_api.GetResult();

  Condition condition(cancellation_context);

  // when the network operation took too much time we cancel it and exit
  // execution, to make sure that network callback will not access dangling
  // references we protect them with atomic bool flag.
  auto interest_flag = std::make_shared<std::atomic_bool>(true);

  MetadataApi::CatalogVersionResponse version_response;
  cancellation_context.ExecuteOrCancelled2([&]() {
    return MetadataApi::GetLatestCatalogVersion(
        client, -1, data_request.GetBillingTag(),
        [&, interest_flag](MetadataApi::CatalogVersionResponse response) {
          if (interest_flag->exchange(false)) {
            version_response = std::move(response);
            condition.Notify();
          }
        });
  });

  if (!condition.Wait()) {
    // We are just about exit the execution.
    interest_flag->store(false);

    if (cancellation_context.IsCancelled()) {
      // We can't use version response here because it could potentially be
      // uninitialized.
      return ApiError(ErrorCode::Cancelled, "Operation cancelled.");
    } else {
      cancellation_context.CancelOperation();
      return ApiError(ErrorCode::RequestTimeout, "Network request timed out.");
    }
  }

  if (version_response.IsSuccessful()) {
    repository.PutVersion(version_response.GetResult());
  } else {
    const auto& error = version_response.GetError();
    if (error.GetHttpStatusCode() == http::HttpStatusCode::FORBIDDEN) {
      repository.Clear();
    }
  }

  return version_response;
}

QueryApi::PartitionsResponse VersionedLayerClientImpl::GetPartitionById(
    client::CancellationContext cancellation_context,
    const DataRequest& data_request) const {
  using namespace client;
  const auto& partitionId = data_request.GetPartitionId();
  if (!partitionId) {
    return ApiError(ErrorCode::PreconditionFailed, "Partition Id is missing");
  }

  const auto& version = data_request.GetVersion();
  if (!version) {
    return ApiError(ErrorCode::PreconditionFailed, "Version is not identified");
  }

  repository::PartitionsCacheRepository repository(catalog_,
                                                   safe_settings_.cache);

  std::vector<std::string> partitions{partitionId.value()};
  PartitionsRequest partition_request;
  partition_request.WithLayerId(layer_)
      .WithBillingTag(data_request.GetBillingTag())
      .WithVersion(version);

  auto fetch_option = data_request.GetFetchOption();

  if (fetch_option != OnlineOnly) {
    auto cached_partitions = repository.Get(partition_request, partitions);
    if (cached_partitions.GetPartitions().size() == partitions.size()) {
      auto key = data_request.CreateKey();
      OLP_SDK_LOG_INFO_F(kLogTag, "cache data '%s' found!", key.c_str());
      return cached_partitions;
    } else if (fetch_option == CacheOnly) {
      auto key = data_request.CreateKey();
      OLP_SDK_LOG_INFO_F(kLogTag, "cache catalog '%s' not found!", key.c_str());
      return ApiError(ErrorCode::NotFound,
                      "Cache only resource not found in cache (partition).");
    }
  }

  auto query_api = LookupApi(cancellation_context, "query", "v1", fetch_option);

  if (!query_api.IsSuccessful()) {
    return query_api.GetError();
  }

  const client::OlpClient& client = query_api.GetResult();

  Condition condition(cancellation_context);

  // when the network operation took too much time we cancel it and exit
  // execution, to make sure that network callback will not access dangling
  // references we protect them with atomic bool flag.
  auto interest_flag = std::make_shared<std::atomic_bool>(true);

  QueryApi::PartitionsResponse query_response;
  cancellation_context.ExecuteOrCancelled2([&]() {
    return QueryApi::GetPartitionsbyId(
        client, layer_, partitions, version, boost::none,
        data_request.GetBillingTag(),
        [&, interest_flag](QueryApi::PartitionsResponse response) {
          if (interest_flag->exchange(false)) {
            query_response = std::move(response);
            condition.Notify();
          }
        });
  });

  if (!condition.Wait()) {
    // We are just about exit the execution.
    interest_flag->store(false);

    if (cancellation_context.IsCancelled()) {
      // We can't use query response here because it could potentially be
      // uninitialized.
      return ApiError(ErrorCode::Cancelled, "Operation cancelled.");
    } else {
      cancellation_context.CancelOperation();
      return ApiError(ErrorCode::RequestTimeout, "Network request timed out.");
    }
  }

  if (query_response.IsSuccessful()) {
    auto key = data_request.CreateKey();
    OLP_SDK_LOG_INFO_F(kLogTag, "put '%s' to cache", key.c_str());
    repository.Put(partition_request, query_response.GetResult(), 0);
  } else {
    const auto& error = query_response.GetError();
    if (error.GetHttpStatusCode() == http::HttpStatusCode::FORBIDDEN) {
      auto key = data_request.CreateKey();
      OLP_SDK_LOG_INFO_F(kLogTag, "clear '%s' cache", key.c_str());
      // Delete partitions only but not the layer
      repository.ClearPartitions(partition_request, partitions);
    }
  }

  return query_response;
}

BlobApi::DataResponse VersionedLayerClientImpl::GetBlobData(
    client::CancellationContext cancellation_context,
    const DataRequest& data_request) const {
  using namespace client;

  auto fetch_option = data_request.GetFetchOption();
  const auto& data_handle = data_request.GetDataHandle();

  if (!data_handle) {
    auto key = data_request.CreateKey();
    OLP_SDK_LOG_INFO_F(kLogTag, "clear '%s' cache", key.c_str());
    return ApiError(ErrorCode::PreconditionFailed, "Data handle is missing");
  }

  repository::DataCacheRepository repository(catalog_, safe_settings_.cache);

  if (fetch_option != OnlineOnly) {
    auto cached_data = repository.Get(layer_, data_handle.value());
    if (cached_data) {
      auto key = data_request.CreateKey();
      OLP_SDK_LOG_INFO_F(kLogTag, "cache data '%s' found!", key.c_str());
      return cached_data.value();
    } else if (fetch_option == CacheOnly) {
      auto key = data_request.CreateKey();
      OLP_SDK_LOG_INFO_F(kLogTag, "cache data '%s' not found!", key.c_str());
      return ApiError(ErrorCode::NotFound,
                      "Cache only resource not found in cache (data).");
    }
  }

  auto blob_api = LookupApi(cancellation_context, "blob", "v1", fetch_option);

  if (!blob_api.IsSuccessful()) {
    return blob_api.GetError();
  }

  const client::OlpClient& client = blob_api.GetResult();

  Condition condition(cancellation_context);

  // when the network operation took too much time we cancel it and exit
  // execution, to make sure that network callback will not access dangling
  // references we protect them with atomic bool flag.
  auto interest_flag = std::make_shared<std::atomic_bool>(true);

  BlobApi::DataResponse blob_response;
  cancellation_context.ExecuteOrCancelled2([&]() {
    return BlobApi::GetBlob(client, layer_, data_handle.value(),
                            data_request.GetBillingTag(), boost::none,
                            [&, interest_flag](BlobApi::DataResponse response) {
                              if (interest_flag->exchange(false)) {
                                blob_response = std::move(response);
                                condition.Notify();
                              }
                            });
  });

  if (!condition.Wait()) {
    // We are just about exit the execution.
    interest_flag->store(false);

    if (cancellation_context.IsCancelled()) {
      // We can't use blob response here because it could potentially be
      // uninitialized.
      return ApiError(ErrorCode::Cancelled, "Operation cancelled.");
    } else {
      cancellation_context.CancelOperation();
      return ApiError(ErrorCode::RequestTimeout, "Network request timed out.");
    }
  }

  if (blob_response.IsSuccessful()) {
    repository.Put(blob_response.GetResult(), layer_, data_handle.value());
  } else {
    const auto& error = blob_response.GetError();
    if (error.GetHttpStatusCode() == http::HttpStatusCode::FORBIDDEN) {
      auto key = data_request.CreateKey();
      OLP_SDK_LOG_INFO_F(kLogTag, "clear '%s' cache", key.c_str());
      repository.Clear(layer_, data_handle.value());
    }
  }

  return blob_response;
}

}  // namespace read
}  // namespace dataservice
}  // namespace olp

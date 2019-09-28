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

#pragma once

#include <memory>

#include <olp/core/client/ApiError.h>
#include <olp/core/client/ApiResponse.h>
#include <olp/core/client/CancellationToken.h>
#include <olp/core/client/HRN.h>
#include <olp/core/client/OlpClientSettings.h>
#include <olp/dataservice/read/DataRequest.h>
#include <olp/dataservice/read/DataServiceReadApi.h>
#include <olp/dataservice/read/model/Data.h>

namespace olp {
namespace dataservice {
namespace read {

class VersionedLayerClientImpl;

/**
 * @brief The VersionedLayerClient aimed to acquire data from OLP services.
 */
class DATASERVICE_READ_API VersionedLayerClient final {
 public:
  /// ApiResponse template alias
  template <typename Result>
  using CallbackResponse = client::ApiResponse<Result, client::ApiError>;
  /// Callback template alias
  template <typename Response>
  using Callback = std::function<void(CallbackResponse<Response> response)>;

  /**
   * @brief Sets up \c OlpClientSettings and \c HRN to be used during requests.
   */
  VersionedLayerClient(olp::client::HRN catalog, std::string layer,
                       olp::client::OlpClientSettings client_settings);

  /**
   * @brief Fetches data for a partition or data handle asynchronously.
   *
   * TODO: Specify scenarios.
   *
   * @param data_request contains the complete set of request parameters.
   * @param callback will be invoked once the DataResult is available, or an
   * error is encountered.
   * @return A token that can be used to cancel this request
   */
  olp::client::CancellationToken GetData(DataRequest data_request,
                                         Callback<model::Data> callback);

 private:
  std::shared_ptr<VersionedLayerClientImpl> impl_;
};

}  // namespace read
}  // namespace dataservice
}  // namespace olp

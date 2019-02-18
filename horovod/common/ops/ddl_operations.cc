// Copyright 2016 The TensorFlow Authors. All Rights Reserved.
// Modifications copyright (C) 2019 Uber Technologies, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include "ddl_operations.h"

namespace horovod {
namespace common {

DDL_Type GetDDLDataType(const std::shared_ptr<Tensor> tensor) {
  switch (tensor->dtype()) {
    case HOROVOD_FLOAT32:
      return DDL_TYPE_FLOAT;
    default:
      throw std::logic_error("Type " + DataType_Name(tensor->dtype()) +
                             " is not supported in DDL mode.");
  }
}

DDLAllreduce::DDLAllreduce(DDLContext* ddl_context,
                           CUDAContext* cuda_context,
                           CommunicationContext* comm_context,
                           HorovodGlobalState* global_state)
                           : CUDAAllreduceAsync(cuda_context, comm_context, global_state),
                             ddl_context_(ddl_context) {}

void DDLAllreduce::InitComm(std::vector<TensorTableEntry>& entries, const std::vector<int32_t>& devices) {
  auto& timeline = global_state_->timeline;
  auto& first_entry = entries[0];
  if (!ddl_context_->ddl_initialized) {
    // Initialize DDL
    auto ddl_options = std::getenv("DDL_OPTIONS");
    if (ddl_options == nullptr) {
      throw std::logic_error("DDL_OPTIONS env variable needs to be set to use DDL.");
    }

    auto ddl_result = ddl_init(ddl_options);
    if (ddl_result != DDL_SUCCESS) {
      throw std::logic_error("ddl_init failed.");
    }
    ddl_context_->ddl_initialized = true;
    ddl_context_->ddl_local_device_id = first_entry.device;
  } else if (ddl_context_->ddl_local_device_id != first_entry.device) {
    throw std::logic_error("DDL does not support more than one GPU device per process.");
  }
}

void DDLAllreduce::DoAllreduce(std::vector<TensorTableEntry>& entries
                               const void* fused_input_data, void* buffer_data,
                               int64_t& num_elements, size_t& buffer_len) {
  if (entries.size() == 1) {
    // Copy input buffer content to output buffer
    // because DDL only supports in-place allreduce
    auto cuda_result = cudaMemcpyAsync(buffer_data, fused_input_data, buffer_len,
                                       cudaMemcpyDeviceToDevice, *stream_);
    cuda_context_->ErrorCheck("cudaMemcpyAsync", cuda_result);
    cuda_context_->RecordEvent(event_queue, MEMCPY_IN_FUSION_BUFFER, stream);
  }

  // Synchronize.
  auto& timeline = global_state_->timeline;
  cuda_context_->WaitForEvents(event_queue_, entries, timeline);

  auto& first_entry = entries[0];
  DDL_Type ddl_data_type = GetDDLDataType(first_entry.tensor);
  auto ddl_result = ddl_allreduce(buffer_data, (size_t)num_elements, ddl_data_type,
                                  DDL_OP_SUM);
  if (ddl_result != DDL_SUCCESS) {
    throw std::logic_error("ddl_allreduce failed.");
  }
}

} // namespace common
} // namespace horovod

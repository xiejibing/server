// Copyright 2023-2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "stream_infer_handler.h"

#include <regex>

namespace triton { namespace server { namespace grpc {

// Make sure to keep InferResponseAlloc and OutputBufferQuery logic in sync
TRITONSERVER_Error*
StreamInferResponseAlloc(
    TRITONSERVER_ResponseAllocator* allocator, const char* tensor_name,
    size_t byte_size, TRITONSERVER_MemoryType preferred_memory_type,
    int64_t preferred_memory_type_id, void* userp, void** buffer,
    void** buffer_userp, TRITONSERVER_MemoryType* actual_memory_type,
    int64_t* actual_memory_type_id)
{
  AllocPayload<inference::ModelStreamInferResponse>* payload =
      reinterpret_cast<AllocPayload<inference::ModelStreamInferResponse>*>(
          userp);

  auto response = payload->response_queue_->GetLastAllocatedResponse();

  if (response == nullptr) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        "Unable to access the last allocated response");
  }

  return ResponseAllocatorHelper(
      allocator, tensor_name, byte_size, preferred_memory_type,
      preferred_memory_type_id, response->mutable_infer_response(),
      payload->shm_map_, buffer, buffer_userp, actual_memory_type,
      actual_memory_type_id);
}

TRITONSERVER_Error*
StreamInferResponseStart(TRITONSERVER_ResponseAllocator* allocator, void* userp)
{
  AllocPayload<inference::ModelStreamInferResponse>* payload =
      reinterpret_cast<AllocPayload<inference::ModelStreamInferResponse>*>(
          userp);

  // Move to the next response object
  payload->response_queue_->AllocateResponse();

  return nullptr;  // success
}

// Make sure to keep InferResponseAlloc and OutputBufferQuery logic in sync
TRITONSERVER_Error*
StreamOutputBufferQuery(
    TRITONSERVER_ResponseAllocator* allocator, void* userp,
    const char* tensor_name, size_t* byte_size,
    TRITONSERVER_MemoryType* memory_type, int64_t* memory_type_id)
{
  AllocPayload<inference::ModelStreamInferResponse>* payload =
      reinterpret_cast<AllocPayload<inference::ModelStreamInferResponse>*>(
          userp);
  return OutputBufferQueryHelper(
      allocator, tensor_name, byte_size, payload->shm_map_, memory_type,
      memory_type_id);
}

// Make sure to keep InferResponseAlloc, OutputBufferQuery, and
// OutputBufferAttributes logic in sync
TRITONSERVER_Error*
StreamOutputBufferAttributes(
    TRITONSERVER_ResponseAllocator* allocator, const char* tensor_name,
    TRITONSERVER_BufferAttributes* buffer_attributes, void* userp,
    void* buffer_userp)
{
  AllocPayload<inference::ModelStreamInferResponse>* payload =
      reinterpret_cast<AllocPayload<inference::ModelStreamInferResponse>*>(
          userp);

  return OutputBufferAttributesHelper(
      allocator, tensor_name, payload->shm_map_, buffer_attributes);
}

//=============================================================================
//  The following section contains the handling mechanism for ModelStreamInfer
//  RPC. This implementation is tuned towards performance and reducing latency.
//=============================================================================

void
ModelStreamInferHandler::StartNewRequest()
{
  auto context = std::make_shared<State::Context>(cq_, NEXT_UNIQUE_ID);
  context->SetCompressionLevel(compression_level_);
  State* state = StateNew(tritonserver_.get(), context);

#ifdef TRITON_ENABLE_TRACING
  // Can't create trace as we don't know the model to be requested,
  // track timestamps in 'state'
  state->trace_timestamps_.emplace_back(
      std::make_pair("GRPC_WAITREAD_START", TraceManager::CaptureTimestamp()));
#endif  // TRITON_ENABLE_TRACING

  service_->RequestModelStreamInfer(
      state->context_->ctx_.get(), state->context_->responder_.get(), cq_, cq_,
      state);

  LOG_VERBOSE(1) << "New request handler for " << Name() << ", "
                 << state->unique_id_;
}

bool
ModelStreamInferHandler::Process(
    InferHandler::State* state, bool rpc_ok, bool is_notification)
{
  if (is_notification) {
    state->context_->SetReceivedNotification(true);
  }
  // Because gRPC doesn't allow concurrent writes on the
  // the stream we only have a single handler thread that
  // reads from the completion queue. Hence, cancellation
  // notification will be received on the same handler
  // thread.
  // This means that we only need to take care of
  // synchronizing this thread and the ResponseComplete
  // threads.
  if (state->context_->ReceivedNotification()) {
    std::lock_guard<std::recursive_mutex> lock(state->step_mtx_);
    if (state->IsGrpcContextCancelled()) {
      if (is_notification) {
        // This is the cancellation notification
        LOG_VERBOSE(1) << "Cancellation notification received for " << Name()
                       << ", rpc_ok=" << rpc_ok << ", context "
                       << state->context_->unique_id_ << " step "
                       << state->context_->step_ << ", state "
                       << state->unique_id_ << " step " << state->step_;
      }

      bool resume = state->context_->HandleCancellation(state, rpc_ok, Name());
      return resume;
    } else {
      if (state->context_->HandleCompletion()) {
        return true;
      }
    }
  }

  LOG_VERBOSE(1) << "Process for " << Name() << ", rpc_ok=" << rpc_ok
                 << ", context " << state->context_->unique_id_ << ", "
                 << state->unique_id_ << " step " << state->step_;

  // We need an explicit finish indicator. Can't use 'state->step_'
  // because we launch an async thread that could update 'state's
  // step_ to be FINISH before this thread exits this function.
  bool finished = false;

  if (state->step_ == Steps::START) {
    // A new stream connection... If RPC failed on a new request then
    // the server is shutting down and so we should do nothing.
    if (!rpc_ok) {
      state->step_ = Steps::FINISH;
      return false;
    }

    // Start a new request to replace this one...
    StartNewRequest();

    if (ExecutePrecondition(state)) {
      // Since this is the start of a connection, 'state' hasn't been
      // used yet so use it to read a request off the connection.
      state->context_->step_ = Steps::READ;
      state->step_ = Steps::READ;
      state->context_->responder_->Read(&state->request_, state);
    } else {
      // Precondition is not satisfied, cancel the stream
      state->context_->step_ = Steps::COMPLETE;
      state->step_ = Steps::PARTIAL_COMPLETION;
      ::grpc::Status status = ::grpc::Status(
          ::grpc::StatusCode::UNAVAILABLE,
          std::string("This protocol is restricted, expecting header '") +
              restricted_kv_.first + "'");
      state->context_->responder_->Finish(status, state);
      return !finished;
    }
    state->context_->ExtractStateFromHeaders(state);
  } else if (state->step_ == Steps::READ) {
    TRITONSERVER_Error* err = nullptr;
    const inference::ModelInferRequest& request = state->request_;
#ifdef TRITON_ENABLE_TRACING
    state->trace_timestamps_.emplace_back(
        std::make_pair("GRPC_WAITREAD_END", TraceManager::CaptureTimestamp()));
#endif  // TRITON_ENABLE_TRACING

    // If done reading and no in-flight requests then can finish the
    // entire stream. Otherwise just finish this state.
    if (!rpc_ok) {
      state->context_->step_ = Steps::WRITEREADY;
      if (state->context_->IsRequestsCompleted()) {
        state->context_->step_ = Steps::COMPLETE;
        state->step_ = Steps::PARTIAL_COMPLETION;
        LOG_VERBOSE(2) << "Finishing responder from state "
                       << state->unique_id_;
        state->context_->responder_->Finish(
            state->context_->finish_ok_ ? ::grpc::Status::OK
                                        : ::grpc::Status::CANCELLED,
            state);
      } else {
        state->step_ = Steps::FINISH;
        finished = true;
      }

      return !finished;
    }

    std::shared_lock<std::shared_mutex> lk1(*conn_mtx_);

    if (!*accepting_new_conn_) {
      err = TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_UNAVAILABLE,
          "GRPC server is shutting down and has stopped accepting new "
          "requests.");
    }

    int64_t requested_model_version;
    if (err == nullptr) {
      err = GetModelVersionFromString(
          request.model_version(), &requested_model_version);
    }

    // Record the transaction policy of the model into the current state
    // object.
    if (err == nullptr) {
      uint32_t txn_flags;
      err = TRITONSERVER_ServerModelTransactionProperties(
          tritonserver_.get(), request.model_name().c_str(),
          requested_model_version, &txn_flags, nullptr /* voidp */);
      if (err == nullptr) {
        state->is_decoupled_ = ((txn_flags & TRITONSERVER_TXN_DECOUPLED) != 0);
      }
    }

    // Request has been successfully read, increment the context request
    // counter.
    state->context_->IncrementRequestCounter();

    // If the request is not for a model with decoupled transaction policy
    // then put it in the context queue so that its response is sent in
    // the same order as the request was received.
    if (!state->is_decoupled_) {
      state->context_->EnqueueForResponse(state);
    }

    // Need to get context here as it is needed below. 'state' can
    // complete inference, write response, and finish (which releases
    // context) before we make any forward progress.... so need to
    // hold onto context here while we know it is good.
    std::shared_ptr<StateContext> context = state->context_;

    // Issue the inference request into server...
    auto response_queue_ = state->response_queue_;

    // Create the inference request which contains all the
    // input information needed for an inference.
    TRITONSERVER_InferenceRequest* irequest = nullptr;
    if (err == nullptr) {
      err = TRITONSERVER_InferenceRequestNew(
          &irequest, tritonserver_.get(), request.model_name().c_str(),
          requested_model_version);
    }

    if (err == nullptr) {
      state->inference_request_ = {
          irequest, [](TRITONSERVER_InferenceRequest* request) {
            LOG_TRITONSERVER_ERROR(
                TRITONSERVER_InferenceRequestDelete(request),
                "deleting gRPC inference request");
          }};
      err = SetInferenceRequestMetadata(irequest, request, state->parameters_);
    }

    if (err == nullptr) {
      err = ForwardHeadersAsParameters(irequest, state);
    }

    // Will be used to hold the serialized data in case explicit string
    // tensors are present in the request.
    std::list<std::string> serialized_data;

    // Maintain shared pointers(read-only reference) to the shared memory
    // block's information for the shared memory regions used by the request.
    // These pointers will automatically increase the usage count, preventing
    // unregistration of the shared memory. This vector must be cleared in the
    // `StreamInferResponseComplete` callback (after inference) to decrease the
    // count and permit unregistration. The vector will be included in
    // `response_release_payload` for the callback.
    std::vector<std::shared_ptr<const SharedMemoryManager::SharedMemoryInfo>>
        shm_regions_info;

    if (err == nullptr) {
      err = InferGRPCToInput(
          tritonserver_, shm_manager_, request, &serialized_data, irequest,
          &shm_regions_info);
    }
    if (err == nullptr) {
      err = InferAllocatorPayload<inference::ModelStreamInferResponse>(
          tritonserver_, shm_manager_, request, std::move(serialized_data),
          response_queue_, &state->alloc_payload_, &shm_regions_info);
    }

    auto request_release_payload =
        std::make_unique<RequestReleasePayload>(state->inference_request_);
    auto response_release_payload = std::make_unique<ResponseReleasePayload>(
        state, std::move(shm_regions_info), shm_manager_);

    if (err == nullptr) {
      err = TRITONSERVER_InferenceRequestSetReleaseCallback(
          irequest, InferRequestComplete,
          request_release_payload.get() /* request_release_userp */);
    }
    if (err == nullptr) {
      err = TRITONSERVER_InferenceRequestSetResponseCallback(
          irequest, allocator_,
          &state->alloc_payload_ /* response_allocator_userp */,
          StreamInferResponseComplete,
          response_release_payload.get() /* response_userp */);
    }

    if (err == nullptr) {
      TRITONSERVER_InferenceTrace* triton_trace = nullptr;
#ifdef TRITON_ENABLE_TRACING
      if (trace_manager_ != nullptr) {
        GrpcServerCarrier carrier(state->context_->ctx_.get());
        auto start_options =
            trace_manager_->GetTraceStartOptions(carrier, request.model_name());
        state->trace_ = std::move(trace_manager_->SampleTrace(start_options));
        if (state->trace_ != nullptr) {
          triton_trace = state->trace_->trace_;
        }
      }
#endif  // TRITON_ENABLE_TRACING

      state->step_ = ISSUED;
      err = TRITONSERVER_ServerInferAsync(
          tritonserver_.get(), irequest, triton_trace);
    }

    // If there was not an error in issuing the 'state' request then
    // state->step_ == ISSUED and inference request has
    // initiated... the completion callback will transition to
    // WRITEREADY or WRITTEN or CANCELLED. Recording the state and the
    // irequest to handle gRPC stream cancellation.
    if (err == nullptr) {
      state->context_->InsertInflightState(state);
      // The payload will be cleaned in release callback.
      request_release_payload.release();
      response_release_payload.release();
    } else {
      // If there was an error then enqueue the error response and show
      // it to be ready for writing.
      inference::ModelStreamInferResponse* response;
      if (state->is_decoupled_) {
        state->response_queue_->AllocateResponse();
        response = state->response_queue_->GetLastAllocatedResponse();
      } else {
        response = state->response_queue_->GetNonDecoupledResponse();
      }

      // Get request ID for logging in case of error.
      std::string log_request_id = request.id();
      if (log_request_id.empty()) {
        log_request_id = "<id_unknown>";
      }
      LOG_VERBOSE(1) << "[request id: " << log_request_id << "] "
                     << "Infer failed: " << TRITONSERVER_ErrorMessage(err);

      ::grpc::Status status;
      GrpcStatusUtil::Create(&status, err);
      TRITONSERVER_ErrorDelete(err);
      response->set_error_message(status.error_message());
      response->mutable_infer_response()->Clear();
      // repopulate the id so that client knows which request failed.
      response->mutable_infer_response()->set_id(request.id());
      if (!state->is_decoupled_) {
        state->step_ = Steps::WRITEREADY;
        state->context_->WriteResponseIfReady(state);
      } else {
        InferHandler::State* writing_state = nullptr;
        std::lock_guard<std::recursive_mutex> lk2(state->context_->mu_);
        {
          std::lock_guard<std::recursive_mutex> lk3(state->step_mtx_);
          state->response_queue_->MarkNextResponseComplete();
          state->context_->ready_to_write_states_.push(state);
          if (!state->context_->ongoing_write_) {
            // Only one write is allowed per gRPC stream / context at any time.
            // If the stream is not currently writing, start writing the next
            // ready to write response from the next ready to write state from
            // 'ready_to_write_states_'. If there are other responses on the
            // state ready to be written after starting the write, the state
            // will be placed at the back of the 'ready_to_write_states_'. If
            // there are no other response, the state will be marked as 'ISSUED'
            // if complete final flag is not received yet from the backend or
            // completed if complete final flag is received.
            // The 'ongoing_write_' will reset once the completion queue returns
            // a written state and no additional response on the stream is ready
            // to be written.
            state->context_->ongoing_write_ = true;
            writing_state = state->context_->ready_to_write_states_.front();
            state->context_->ready_to_write_states_.pop();
          }
          state->complete_ = true;
        }
        if (writing_state != nullptr) {
          StateWriteResponse(writing_state);
        }
      }
    }

    // Now that the inference request is in flight, create a copy of
    // 'state' and use it to attempt another read from the connection
    // (i.e the next request in the stream).
    State* next_read_state =
        StateNew(tritonserver_.get(), context, Steps::READ);

#ifdef TRITON_ENABLE_TRACING
    // Capture a timestamp for the time when we start waiting for this
    // next request to read.
    // Can't create trace as we don't know the model to be requested,
    // track timestamps in 'state'
    next_read_state->trace_timestamps_.emplace_back(std::make_pair(
        "GRPC_WAITREAD_START", TraceManager::CaptureTimestamp()));
#endif  // TRITON_ENABLE_TRACING

    next_read_state->context_->responder_->Read(
        &next_read_state->request_, next_read_state);
  } else if (state->step_ == Steps::PARTIAL_COMPLETION) {
    state->step_ = Steps::COMPLETE;
  } else if (state->step_ == Steps::COMPLETE) {
    state->step_ = Steps::FINISH;
  } else if (state->step_ == Steps::FINISH) {
    // The RPC execution is finished hence the state
    // can be released.
    finished = true;
  } else if (!state->is_decoupled_) {
    // We handle the WRITTEN and WRITEREADY states little
    // differently depending whether the inference request
    // is for a decoupled model or not. This is because the
    // grpc contract requires us to call Write() only once
    // on a task. Hence, for decoupled writes, we call only
    // one write and then wait for another notification from
    // the completion queue to execute pending Write()'s, if
    // any.

    //
    // Non-Decoupled state transitions
    //
    if (state->step_ == Steps::WRITTEN) {
      state->context_->ongoing_write_ = false;
#ifdef TRITON_ENABLE_TRACING
      state->trace_timestamps_.emplace_back(
          std::make_pair("GRPC_SEND_END", TraceManager::CaptureTimestamp()));
#endif  // TRITON_ENABLE_TRACING

      // If the write failed (for example, client closed the stream)
      // mark that the stream did not complete successfully but don't
      // cancel right away... need to wait for any pending reads,
      // inferences and writes to complete.
      if (!rpc_ok) {
        LOG_VERBOSE(1) << "Write for " << Name() << ", rpc_ok=" << rpc_ok
                       << ", context " << state->context_->unique_id_ << ", "
                       << state->unique_id_ << " step " << state->step_
                       << ", failed";
        state->context_->finish_ok_ = false;
      }

      // Log an error if 'state' is not the expected next response. Mark
      // that the stream did not complete successfully but don't cancel
      // right away... need to wait for any pending reads, inferences
      // and writes to complete.
      if (!state->context_->PopCompletedResponse(state)) {
        LOG_ERROR << "Unexpected response for " << Name()
                  << ", rpc_ok=" << rpc_ok << ", context "
                  << state->context_->unique_id_ << ", " << state->unique_id_
                  << " step " << state->step_;
        state->context_->finish_ok_ = false;
      }

      // Write the next response if it is ready...
      state->context_->WriteResponseIfReady(nullptr);

      // The response for the request has been written completely.
      // The counter can be safely decremented.
      state->context_->DecrementRequestCounter();
      finished = Finish(state);
    }
  } else {
    //
    //  Decoupled state transitions
    //
    if (state->step_ == Steps::WRITTEN) {
#ifdef TRITON_ENABLE_TRACING
      state->trace_timestamps_.emplace_back(
          std::make_pair("GRPC_SEND_END", TraceManager::CaptureTimestamp()));
#endif  // TRITON_ENABLE_TRACING

      // If the write failed (for example, client closed the stream)
      // mark that the stream did not complete successfully but don't
      // cancel right away... need to wait for any pending reads,
      // inferences and writes to complete.
      if (!rpc_ok) {
        LOG_VERBOSE(1) << "Write for " << Name() << ", rpc_ok=" << rpc_ok
                       << ", context " << state->context_->unique_id_ << ", "
                       << state->unique_id_ << " step " << state->step_
                       << ", failed";
        state->context_->finish_ok_ = false;
      }

      {
        InferHandler::State* writing_state = nullptr;
        std::lock_guard<std::recursive_mutex> lk2(state->context_->mu_);
        {
          std::lock_guard<std::recursive_mutex> lk3(state->step_mtx_);
          if (!state->context_->ready_to_write_states_.empty()) {
            writing_state = state->context_->ready_to_write_states_.front();
            state->context_->ready_to_write_states_.pop();
          } else {
            state->context_->ongoing_write_ = false;
          }
          // Finish the state if all the transactions associated with
          // the state have completed.
          if (state != writing_state) {
            if (state->IsComplete()) {
              state->context_->DecrementRequestCounter();
              finished = Finish(state);
            } else {
              state->step_ = Steps::ISSUED;
            }
          }
        }
        if (writing_state != nullptr) {
          StateWriteResponse(writing_state);
        }
      }
    } else if (state->step_ == Steps::WRITEREADY) {
      // Finish the state if all the transactions associated with
      // the state have completed.
      std::lock_guard<std::recursive_mutex> lk2(state->context_->mu_);
      {
        if (state->IsComplete()) {
          state->context_->DecrementRequestCounter();
          finished = Finish(state);
        } else {
          LOG_ERROR << "Should not print this! Decoupled should NOT write via "
                       "WRITEREADY!";
          // Remove the state from the completion queue
          std::lock_guard<std::recursive_mutex> lock(state->step_mtx_);
          state->step_ = Steps::ISSUED;
        }
      }
    }
  }

  return !finished;
}

// For decoupled only. Caller must ensure exclusive write.
void
ModelStreamInferHandler::StateWriteResponse(InferHandler::State* state)
{
  if (state->delay_response_ms_ != 0) {
    // Will delay the write of the response by the specified time.
    // This can be used to test the flow where there are other
    // responses available to be written.
    LOG_INFO << "Delaying the write of the response by "
             << state->delay_response_ms_ << " ms...";
    std::this_thread::sleep_for(
        std::chrono::milliseconds(state->delay_response_ms_));
  }
  {
    std::lock_guard<std::recursive_mutex> lock(state->step_mtx_);
    state->step_ = Steps::WRITTEN;
    // gRPC doesn't allow to issue another write till the notification from
    // previous write has been delivered.
    state->context_->DecoupledWriteResponse(state);
    if (state->response_queue_->HasReadyResponse()) {
      state->context_->ready_to_write_states_.push(state);
    }
  }
}

bool
ModelStreamInferHandler::Finish(InferHandler::State* state)
{
  // If done reading and no in-flight requests then can finish the
  // entire stream.
  if (state->context_->IsRequestsCompleted()) {
    state->context_->step_ = Steps::COMPLETE;
    state->step_ = Steps::PARTIAL_COMPLETION;
    LOG_VERBOSE(2) << "Finishing responder from state " << state->unique_id_;
    state->context_->responder_->Finish(
        state->context_->finish_ok_ ? ::grpc::Status::OK
                                    : ::grpc::Status::CANCELLED,
        state);
  } else if (state->IsAsyncNotifyState()) {
    // Should only mark the state complete as the state has been sent
    // to AsyncNotifyWhenDone() tag and the completion event should take
    // care of finally releasing the state object.
    state->step_ = Steps::COMPLETE;
  } else {
    // Can finish this state.
    state->step_ = Steps::FINISH;
    return true;
  }

  return false;
}

void
ModelStreamInferHandler::StreamInferResponseComplete(
    TRITONSERVER_InferenceResponse* iresponse, const uint32_t flags,
    void* userp)
{
  ResponseReleasePayload* response_release_payload(
      static_cast<ResponseReleasePayload*>(userp));
  auto state = response_release_payload->state_;

  // Ignore Response from CORE in case GRPC Strict as we dont care about
  if (state->context_->gRPCErrorTracker_->triton_grpc_error_) {
    std::lock_guard<std::recursive_mutex> lock(state->context_->mu_);
    if (state->context_->gRPCErrorTracker_->GRPCErrorEncountered()) {
      return;
    }
  }
  // Increment the callback index
  uint32_t response_index = state->cb_count_++;

  LOG_VERBOSE(1) << "ModelStreamInferHandler::StreamInferComplete, context "
                 << state->context_->unique_id_ << ", " << state->unique_id_
                 << " step " << state->step_ << ", callback index "
                 << state->cb_count_ << ", flags " << flags
                 << ", response is nullptr " << (iresponse == nullptr);

#ifdef TRITON_ENABLE_TRACING
  if (state->cb_count_ == 1) {
    state->trace_timestamps_.emplace_back(std::make_pair(
        "INFER_RESPONSE_COMPLETE", TraceManager::CaptureTimestamp()));
  }
#endif  // TRITON_ENABLE_TRACING

  bool is_complete =
      state->complete_ || (flags & TRITONSERVER_RESPONSE_COMPLETE_FINAL) != 0;

  // If receiving the final callback then erase the state from the inflight
  // state data structure to prevent cancellation being called on the request.
  // Also make sure that if this state was sent to gRPC async notification
  // mechanism then the state is not removed as it would be needed for handling
  // the cancellation if detected.
  if (is_complete && (!state->IsAsyncNotifyState())) {
    state->context_->EraseInflightState(state);
  }

  if (state->IsGrpcContextCancelled()) {
    std::lock_guard<std::recursive_mutex> lock(state->step_mtx_);
    // Clean-up the received response object.
    LOG_TRITONSERVER_ERROR(
        TRITONSERVER_InferenceResponseDelete(iresponse),
        "deleting GRPC inference response");

    LOG_VERBOSE(1) << "ModelStreamInferHandler::StreamInferResponseComplete, "
                   << state->unique_id_
                   << ", skipping response generation as grpc transaction was "
                      "cancelled... ";

    // If this was the final callback for the state
    // then cycle through the completion queue so
    // that state object can be released.
    if (is_complete) {
      state->step_ = Steps::CANCELLED;
      state->context_->PutTaskBackToQueue(state);
      delete response_release_payload;
    }

    state->complete_ = is_complete;
    return;
  }

  auto& response_queue = state->response_queue_;
  std::string log_request_id = state->request_.id();
  if (log_request_id.empty()) {
    log_request_id = "<id_unknown>";
  }

  inference::ModelStreamInferResponse* response = nullptr;
  bool failed = false;
  if (iresponse) {
    // Backend returned a non-null response
    TRITONSERVER_Error* err = nullptr;
    response = response_queue->GetResponseAt(response_index);
    if (response) {
      inference::ModelInferResponse& infer_response =
          *(response->mutable_infer_response());
      // Validate Triton iresponse and set grpc/protobuf response fields from it
      err = InferResponseCompleteCommon<inference::ModelStreamInferResponse>(
          state->tritonserver_, iresponse, infer_response,
          state->alloc_payload_);
    } else {
      LOG_ERROR << "expected the response allocator to have added the response";
    }
    if (err != nullptr) {
      failed = true;
      ::grpc::Status status;
      // Converts CORE errors to GRPC error codes
      GrpcStatusUtil::Create(&status, err);
      response->mutable_infer_response()->Clear();
      response->set_error_message(status.error_message());
      LOG_VERBOSE(1) << "Failed for ID: " << log_request_id << std::endl;
      if (state->context_->gRPCErrorTracker_->triton_grpc_error_) {
        state->status_ = status;
        // Finish only once, if backend ignores cancellation
        LOG_VERBOSE(1) << "GRPC streaming error detected with status: "
                       << status.error_code() << "Closing stream connection."
                       << std::endl;
        state->context_->WriteGRPCErrorResponse(state);
        TRITONSERVER_ErrorDelete(err);
        LOG_TRITONSERVER_ERROR(
            TRITONSERVER_InferenceResponseDelete(iresponse),
            "deleting GRPC inference response");
        delete response_release_payload;
        return;
      }
    }

    TRITONSERVER_ErrorDelete(err);
    LOG_TRITONSERVER_ERROR(
        TRITONSERVER_InferenceResponseDelete(iresponse),
        "deleting GRPC inference response");
  }

  // Decoupled backends can return a null response via
  // TRITONBACKEND_ResponseFactorySendFlags. By default, these null
  // "empty" responses are not sent back to the client. Clients can
  // opt-in to receiving these empty responses via request parameters.
  // NOTE: The complete flag is the only flag used for this case at this time.
  const bool empty_final = !iresponse && state->is_decoupled_ && is_complete;
  const bool enable_empty_final =
      state->parameters_.enable_empty_final_response_;

  const bool create_empty_response = (empty_final && enable_empty_final);
  if (create_empty_response) {
    // Assume decoupled here based on prior checks.
    state->response_queue_->AllocateResponse();
    response = state->response_queue_->GetLastAllocatedResponse();
    if (response) {
      LOG_VERBOSE(1) << "[request id: " << log_request_id << "] "
                     << "Creating empty final response";
      response->mutable_infer_response()->Clear();
    } else {
      LOG_ERROR << "expected the response allocator to have added the response";
    }
  }

  if (response) {
    auto& infer_response = *(response->mutable_infer_response());
    // Set response metadata to associate it with request. These will be set
    // by InferResponseCompleteCommon for successful inference.
    if (create_empty_response || failed) {
      infer_response.set_id(state->request_.id());
      infer_response.set_model_name(state->request_.model_name());
      infer_response.set_model_version(state->request_.model_version());
    }
    auto& params = *(infer_response.mutable_parameters());
    params["triton_final_response"].set_bool_param(is_complete);
  }

  if (state->delay_complete_ms_ != 0) {
    // Delay updating the state. This is useful for testing race condition with
    // the thread that runs Process().
    LOG_INFO << "Delaying the completion of reporting response / flag by "
             << state->delay_complete_ms_ << " ms...";
    void* context_ptr_before_delay = (void*)state->context_.get();
    std::this_thread::sleep_for(
        std::chrono::milliseconds(state->delay_complete_ms_));
    void* context_ptr_after_delay = (void*)state->context_.get();
    if (context_ptr_before_delay != context_ptr_after_delay) {
      LOG_ERROR << "Should not print this! The state context object has "
                   "changed after delay, pointer before: "
                << context_ptr_before_delay
                << ", pointer after: " << context_ptr_after_delay;
    }
  }

  if (state->IsGrpcContextCancelled()) {
    // Need to hold lock because the handler thread processing context
    // cancellation might have cancelled or marked the state for cancellation.
    std::lock_guard<std::recursive_mutex> lock(state->step_mtx_);

    LOG_VERBOSE(1)
        << "ModelStreamInferHandler::StreamInferResponseComplete, "
        << state->unique_id_
        << ", skipping writing response because of transaction was cancelled";

    // If this was the final callback for the state
    // then cycle through the completion queue so
    // that state object can be released.
    if (is_complete) {
      state->step_ = Steps::CANCELLED;
      state->context_->PutTaskBackToQueue(state);
      delete response_release_payload;
    }

    state->complete_ = is_complete;
    return;
  }

  if (state->is_decoupled_) {
    InferHandler::State* writing_state = nullptr;
    std::lock_guard<std::recursive_mutex> lk1(state->context_->mu_);
    {
      std::lock_guard<std::recursive_mutex> lk2(state->step_mtx_);
      bool has_prev_ready_response = state->response_queue_->HasReadyResponse();
      if (response) {
        state->response_queue_->MarkNextResponseComplete();
      }
      if (!has_prev_ready_response && response) {
        state->context_->ready_to_write_states_.push(state);
      }
      if (!state->context_->ongoing_write_ &&
          !state->context_->ready_to_write_states_.empty()) {
        state->context_->ongoing_write_ = true;
        writing_state = state->context_->ready_to_write_states_.front();
        state->context_->ready_to_write_states_.pop();
      }
      if (is_complete && state->response_queue_->IsEmpty() &&
          state->step_ == Steps::ISSUED) {
        // The response queue is empty and complete final flag is received, so
        // mark the state as 'WRITEREADY' so it can be cleaned up later.
        state->step_ = Steps::WRITEREADY;
        state->context_->PutTaskBackToQueue(state);
      }
      state->complete_ = is_complete;
    }
    if (writing_state != nullptr) {
      StateWriteResponse(writing_state);
    }
  } else {  // non-decoupled
    std::lock_guard<std::recursive_mutex> lock(state->step_mtx_);
    state->step_ = Steps::WRITEREADY;
    if (is_complete) {
      state->context_->WriteResponseIfReady(state);
    }
    state->complete_ = is_complete;
  }

  if (is_complete) {
    delete response_release_payload;
  }
}

// Changes the state of grpc_stream_error_state_ to ERROR_HANDLING_COMPLETE,
// indicating we have closed the stream and initiated the cancel flow
void
gRPCErrorTracker::MarkGRPCErrorHandlingComplete()
{
  grpc_stream_error_state_ = TritonGRPCErrorSteps::ERROR_HANDLING_COMPLETE;
}

// Returns true ONLY when GRPC_ERROR from CORE is waiting to be processed.
bool
gRPCErrorTracker::CheckAndUpdateGRPCError()
{
  if (grpc_stream_error_state_ == TritonGRPCErrorSteps::ERROR_ENCOUNTERED) {
    // Change the state to ERROR_HANDLING_COMPLETE as we have called
    // HandleCancellation
    MarkGRPCErrorHandlingComplete();
    return true;
  }
  return false;
}

// Marks error after it has been responded to
void
gRPCErrorTracker::MarkGRPCErrorEncountered()
{
  grpc_stream_error_state_ = TritonGRPCErrorSteps::ERROR_ENCOUNTERED;
}

// Checks if error already responded to in triton_grpc_error mode
bool
gRPCErrorTracker::GRPCErrorEncountered()
{
  if (grpc_stream_error_state_ == TritonGRPCErrorSteps::NONE) {
    return false;
  }
  return true;
}

}}}  // namespace triton::server::grpc

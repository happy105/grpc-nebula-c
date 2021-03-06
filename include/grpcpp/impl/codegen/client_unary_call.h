/*
 *
 * Copyright 2015 gRPC authors.
 * Modifications 2019 Orient Securities Co., Ltd.
 * Modifications 2019 BoCloud Inc.
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
 */

#ifndef GRPCPP_IMPL_CODEGEN_CLIENT_UNARY_CALL_H
#define GRPCPP_IMPL_CODEGEN_CLIENT_UNARY_CALL_H

#include <grpcpp/impl/codegen/call.h>
#include <grpcpp/impl/codegen/channel_interface.h>
#include <grpcpp/impl/codegen/config.h>
#include <grpcpp/impl/codegen/core_codegen_interface.h>
#include <grpcpp/impl/codegen/status.h>
//----begin----- add for hash algo
#include <grpc/grpc.h>
#include "../../orientsec/orientsec_common/orientsec_grpc_string_op.h"   // move string operation to common module by jianbin
#include  "../../orientsec/orientsec_consumer/orientsec_grpc_consumer_control_requests.h"
//----end----

namespace grpc {

class Channel;
class ClientContext;
class CompletionQueue;

namespace internal {

class RpcMethod;
/// Wrapper that performs a blocking unary call
template <class InputMessage, class OutputMessage>
Status BlockingUnaryCall(ChannelInterface* channel, const RpcMethod& method,
                         ClientContext* context, const InputMessage& request,
                         OutputMessage* result) {
  return BlockingUnaryCallImpl<InputMessage, OutputMessage>(
             channel, method, context, request, result)
      .status();
}

template <class InputMessage, class OutputMessage>
class BlockingUnaryCallImpl {
 public:
  BlockingUnaryCallImpl(ChannelInterface* channel, const RpcMethod& method,
                        ClientContext* context, const InputMessage& request,
                        OutputMessage* result) {
    CompletionQueue cq(grpc_completion_queue_attributes{
        GRPC_CQ_CURRENT_VERSION, GRPC_CQ_PLUCK, GRPC_CQ_DEFAULT_POLLING,
        nullptr});  // Pluckable completion queue
    Call call(channel->CreateCall(method, context, &cq));
    CallOpSet<CallOpSendInitialMetadata, CallOpSendMessage,
              CallOpRecvInitialMetadata, CallOpRecvMessage<OutputMessage>,
              CallOpClientSendClose, CallOpClientRecvStatus>
        ops;
    status_ = ops.SendMessage(request);
    if (!status_.ok()) {
      return;
    }

    //----begin----
    //获得hash_arg,用于hash 算法
    char buf[ORIENTSEC_GRPC_PROPERTY_KEY_MAX_LEN] = {0};

    orientsec_grpc_properties_get_value(ORIENTSEC_GRPC_PROPERTIES_C_CONSISTENT_HASH_ARG,
                                   NULL, buf);
    // add by yang
    // style s = "name:\"heiden111111\"\n"
    std::string str = ops.GetMessageName(request);
    //多个值要用map值存储，需要查找
    std::map<std::string, std::string> map_;
    std::vector<std::string> buf_vec;
    orientsec_grpc_split_to_vec(buf, buf_vec, ",");
    orientsec_grpc_split_to_map(str, map_, "\n");

    std::string hash_arg;
    orientsec_grpc_joint_hash_input(map_, buf_vec, hash_arg);

    // Get method name
    std::string m_fullname = method.name();
    std::vector<std::string> ret;
    orientsec_grpc_split_to_vec(m_fullname,ret ,"/");
    std::string m_name = ret.back();
    orientsec_grpc_setcall_methodname(call.call(), m_name.c_str());

    //传递给call 对象
    orientsec_grpc_transfer_setcall_hashinfo(call.call(), hash_arg.c_str());
    
    //判断是否大于最大允许请求数
    //----debug use----
    //const char* name = method.name();
    if (orientsec_grpc_consumer_control_requests(method.name()) == -1) {
      Status state(StatusCode::EXCEEDING_REQUESTS,
                   "Exceeding maximum requests");
      return;
    }


    //----end----
    ops.SendInitialMetadata(&context->send_initial_metadata_,
                            context->initial_metadata_flags());
    ops.RecvInitialMetadata(context);
    ops.RecvMessage(result);
    ops.AllowNoMessage();
    ops.ClientSendClose();
    ops.ClientRecvStatus(context, &status_);
    call.PerformOps(&ops);
    if (cq.Pluck(&ops)) {
      if (!ops.got_message && status_.ok()) {
        status_ = Status(StatusCode::UNIMPLEMENTED,
                         "No message returned for unary request");
      }
    } else {
      GPR_CODEGEN_ASSERT(!status_.ok());
    }
  }
  Status status() { return status_; }
  //----begin----
  
  void orientsec_grpc_transfer_setcall_hashinfo(grpc_call* call, const char* s) {
    orientsec_grpc_setcall_hashinfo(call, s);
  }
  char* orientsec_grpc_transfer_getcall_hashinfo(grpc_call* call) {
    return orientsec_grpc_getcall_hashinfo(call);
  }
  //----end----

 private:
  Status status_;
};

}  // namespace internal
}  // namespace grpc

#endif  // GRPCPP_IMPL_CODEGEN_CLIENT_UNARY_CALL_H

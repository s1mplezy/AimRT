// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "iceoryx_plugin/iceoryx_channel_backend.h"
#include "aimrt_module_cpp_interface/util/buffer_array_allocator.h"
#include "iceoryx_plugin/global.h"
#include "util/buffer_util.h"
#include "util/url_encode.h"

namespace YAML {
template <>
struct convert<aimrt::plugins::iceoryx_plugin::IceoryxChannelBackend::Options> {
  using Options = aimrt::plugins::iceoryx_plugin::IceoryxChannelBackend::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["listener_thread_name"] = rhs.listener_thread_name;
    node["listener_thread_sched_policy"] = rhs.listener_thread_sched_policy;
    node["listener_thread_bind_cpu"] = rhs.listener_thread_bind_cpu;

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["listener_thread_name"])
      rhs.listener_thread_name = node["listener_thread_name"].as<std::string>();

    if (node["listener_thread_sched_policy"])
      rhs.listener_thread_sched_policy = node["listener_thread_sched_policy"].as<std::string>();

    if (node["listener_thread_bind_cpu"])
      rhs.listener_thread_bind_cpu = node["listener_thread_bind_cpu"].as<std::vector<uint32_t>>();

    return true;
  }
};
}  // namespace YAML

namespace aimrt::plugins::iceoryx_plugin {

namespace {

class SubscriberPayloadGuard {
 public:
  SubscriberPayloadGuard(
      iox::popo::UntypedSubscriber* subscriber,
      const void* payload) noexcept
      : subscriber_(subscriber), payload_(payload) {}
  ~SubscriberPayloadGuard() {
    if (subscriber_ != nullptr && payload_ != nullptr)
      subscriber_->release(payload_);
  }

  SubscriberPayloadGuard(const SubscriberPayloadGuard&) = delete;
  SubscriberPayloadGuard& operator=(const SubscriberPayloadGuard&) = delete;

 private:
  iox::popo::UntypedSubscriber* subscriber_;
  const void* payload_;
};

}  // namespace

std::string IceoryxChannelBackend::MakeSerializedPattern(
    const runtime::core::channel::TopicInfo& info) {
  namespace util = aimrt::common::util;
  return std::string("/channel/") + util::UrlEncode(info.topic_name) + "/" +
         util::UrlEncode(info.msg_type);
}

std::string IceoryxChannelBackend::MakeNativePattern(
    const runtime::core::channel::TopicInfo& info) {
  namespace util = aimrt::common::util;
  return std::string("/channel/") + util::UrlEncode(info.topic_name) + "/" +
         util::UrlEncode(info.msg_type + "@aimrt_native_v1");
}

const aimrt_native_loan_type_support_t*
IceoryxChannelBackend::GetDdsNativeTypeSupport(
    const runtime::core::channel::TopicInfo& info) noexcept {
  if (!info.msg_type.starts_with("dds:")) return nullptr;
  const auto* type_support = info.msg_type_support_ref.NativeLoanTypeSupportPtr();
  if (type_support == nullptr || type_support->size == 0 ||
      type_support->alignment == 0 ||
      (type_support->alignment & (type_support->alignment - 1)) != 0 ||
      type_support->construct == nullptr || type_support->destroy == nullptr)
    return nullptr;
  return type_support;
}

void IceoryxChannelBackend::SetListenerThreadOptionsOnce() {
  if (sched_info_set_) return;
  sched_info_set_ = true;

  if (!options_.listener_thread_name.empty())
    runtime::core::util::SetNameForCurrentThread(
        options_.listener_thread_name);
  if (!options_.listener_thread_bind_cpu.empty())
    runtime::core::util::BindCpuForCurrentThread(
        options_.listener_thread_bind_cpu);
  if (!options_.listener_thread_sched_policy.empty())
    runtime::core::util::SetCpuSchedForCurrentThread(
        options_.listener_thread_sched_policy);
}

IceoryxChannelBackend::NativeSubscriptionRoute*
IceoryxChannelBackend::GetOrCreateNativeSubscriptionRoute(
    const runtime::core::channel::TopicInfo& info) {
  const auto pattern = MakeNativePattern(info);
  if (const auto itr = native_subscription_route_map_.find(pattern);
      itr != native_subscription_route_map_.end())
    return itr->second.get();

  const auto* native_type_support = GetDdsNativeTypeSupport(info);
  AIMRT_CHECK_ERROR_THROW(
      native_type_support != nullptr,
      "DDS native-memory type support is unavailable for topic '{}' type '{}'.",
      info.topic_name, info.msg_type);

  auto route = std::make_unique<NativeSubscriptionRoute>();
  route->msg_type_support_ref = info.msg_type_support_ref;
  auto* route_ptr = route.get();
  native_subscription_route_map_.emplace(pattern, std::move(route));

  try {
    iceoryx_manager_.RegisterSubscriber(
        pattern,
        [this, route_ptr](iox::popo::UntypedSubscriber* subscriber) {
          try {
            SetListenerThreadOptionsOnce();
            while (subscriber->hasData()) {
              subscriber->take()
                  .and_then([&](const void* payload) {
                    SubscriberPayloadGuard release_guard(subscriber, payload);
                    auto ctx_ptr = std::make_shared<aimrt::channel::Context>(
                        aimrt_channel_context_type_t::AIMRT_CHANNEL_SUBSCRIBER_CONTEXT);
                    ctx_ptr->SetSerializationType("dds_xcdr2");
                    ctx_ptr->SetMetaValue(
                        AIMRT_CHANNEL_CONTEXT_KEY_BACKEND, Name());

                    if (route_ptr->has_loaned_subscriber)
                      route_ptr->loaned_subscribe_tool.DoSubscribeCallback(
                          ctx_ptr, payload);

                    if (route_ptr->has_ordinary_subscriber) {
                      auto message =
                          route_ptr->msg_type_support_ref.CreateSharedPtr();
                      AIMRT_CHECK_ERROR_THROW(
                          message != nullptr,
                          "Create DDS native subscriber message failed.");
                      route_ptr->msg_type_support_ref.Copy(
                          payload, message.get());
                      route_ptr->ordinary_subscribe_tool.DoSubscribeCallback(
                          ctx_ptr,
                          *route_ptr->ordinary_subscribe_tool.FirstSubscribeWrapper(),
                          message);
                    }
                  })
                  .or_else([](auto&) {});
            }
          } catch (const std::exception& e) {
            AIMRT_WARN(
                "Handle Iceoryx native channel msg failed, exception info: {}",
                e.what());
          }
        });
  } catch (...) {
    native_subscription_route_map_.erase(pattern);
    throw;
  }

  AIMRT_INFO("Register native subscribe type to iceoryx channel, url: {}",
             pattern);
  return route_ptr;
}

void IceoryxChannelBackend::Initialize(YAML::Node options_node) {
  // todo: check options_node->shm_init_size

  AIMRT_CHECK_ERROR_THROW(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Iceoryx channel backend can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  options_node = options_;
}

void IceoryxChannelBackend::Start() {
  AIMRT_CHECK_ERROR_THROW(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");
}

void IceoryxChannelBackend::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  iceoryx_manager_.Shutdown();
}

bool IceoryxChannelBackend::RegisterPublishType(
    const runtime::core::channel::PublishTypeWrapper& publish_type_wrapper) noexcept {
  try {
    AIMRT_CHECK_ERROR_THROW(state_.load() == State::kInit,
                            "Method can only be called when state is 'Init'.");
    const auto& info = publish_type_wrapper.info;
    const auto* native_type_support = GetDdsNativeTypeSupport(info);
    const std::string pattern = native_type_support != nullptr
                                    ? MakeNativePattern(info)
                                    : MakeSerializedPattern(info);

    // register publisher with url to iceoryx
    iceoryx_manager_.RegisterPublisher(pattern);

    if (native_type_support != nullptr &&
        !native_publisher_route_map_.contains(pattern)) {
      auto* publisher = iceoryx_manager_.GetPublisher(pattern);
      AIMRT_CHECK_ERROR_THROW(
          publisher != nullptr,
          "Native iceoryx publisher '{}' was not registered.", pattern);
      native_publisher_route_map_.emplace(
          pattern,
          std::make_unique<NativePublisherRoute>(NativePublisherRoute{
              .publisher = publisher,
              .type_support = native_type_support}));
    }

    AIMRT_INFO("Register publish type to iceoryx channel, url: {}", pattern);

    return true;
  } catch (const std::exception& e) {
    AIMRT_ERROR("{}", e.what());
    return false;
  }
}

bool IceoryxChannelBackend::Subscribe(
    const runtime::core::channel::SubscribeWrapper& subscribe_wrapper) noexcept {
  try {
    AIMRT_CHECK_ERROR_THROW(state_.load() == State::kInit,
                            "Method can only be called when state is 'Init'.");

    namespace util = aimrt::common::util;

    const auto& info = subscribe_wrapper.info;
    if (GetDdsNativeTypeSupport(info) != nullptr) {
      auto* route = GetOrCreateNativeSubscriptionRoute(info);
      route->ordinary_subscribe_tool.AddSubscribeWrapper(&subscribe_wrapper);
      route->has_ordinary_subscriber = true;
      return true;
    }

    const std::string pattern = MakeSerializedPattern(info);

    auto find_itr = subscribe_wrapper_map_.find(pattern);
    if (find_itr != subscribe_wrapper_map_.end()) {
      find_itr->second->AddSubscribeWrapper(&subscribe_wrapper);
      return true;
    }

    // if not registered, register subscriber with url bind to handle to iceoryx
    auto sub_tool_unique_ptr = std::make_unique<runtime::core::channel::SubscribeTool>();
    sub_tool_unique_ptr->AddSubscribeWrapper(&subscribe_wrapper);

    auto* sub_tool_ptr = sub_tool_unique_ptr.get();

    subscribe_wrapper_map_.emplace(pattern, std::move(sub_tool_unique_ptr));

    auto handle =
        [this, topic_name = info.topic_name, sub_tool_ptr](iox::popo::UntypedSubscriber* subscriber) {
          try {
            SetListenerThreadOptionsOnce();
            // read data from shared memory : pkg_size | serialization_type | ctx_num | ctx_key1 | ctx_val1 | ... | ctx_keyN | ctx_valN | msg_buffer
            // use while struck to make sure all packages are read
            while (subscriber->hasData()) {
              subscriber->take()
                  .and_then([&](const void* payload) {
                    auto ctx_ptr = std::make_shared<aimrt::channel::Context>(aimrt_channel_context_type_t::AIMRT_CHANNEL_SUBSCRIBER_CONTEXT);

                    uint32_t pkg_size = util::GetUint32FromBuf(static_cast<const char*>(payload));

                    util::ConstBufferOperator buf_oper(static_cast<const char*>(payload) + 4, pkg_size - 4);

                    // get serialization type
                    std::string serialization_type(buf_oper.GetString(util::BufferLenType::kUInt8));
                    ctx_ptr->SetSerializationType(serialization_type);

                    //  get context meta
                    size_t ctx_num = buf_oper.GetUint8();
                    for (size_t ii = 0; ii < ctx_num; ++ii) {
                      auto key = buf_oper.GetString(util::BufferLenType::kUInt16);
                      auto val = buf_oper.GetString(util::BufferLenType::kUInt16);
                      ctx_ptr->SetMetaValue(key, val);
                    }

                    ctx_ptr->SetMetaValue(AIMRT_CHANNEL_CONTEXT_KEY_BACKEND, Name());

                    // get msg buffer
                    auto remaining_buf = buf_oper.GetRemainingBuffer();

                    sub_tool_ptr->DoSubscribeCallback(
                        ctx_ptr, serialization_type, static_cast<const void*>(remaining_buf.data()), remaining_buf.size());

                    // release shm
                    subscriber->release(payload);
                  })
                  .or_else([](auto& error) {
                    ;  // data has not been ready
                  });
            }
          } catch (const std::exception& e) {
            AIMRT_WARN("Handle Iceoryx channel msg failed, exception info: {}", e.what());
          }
        };

    iceoryx_manager_.RegisterSubscriber(pattern, std::move(handle));

    AIMRT_INFO("Register subscribe type  to iceoryx channel, url: {}", pattern);

    return true;
  } catch (const std::exception& e) {
    AIMRT_ERROR("{}", e.what());
    return false;
  }
}

// dynamic allocation for loaned shm:
//
//         .---------------if not enough -----------------.
//         |                                              |
//         v                                              |
// release old shm   ——> loan double size   ——> try to write msg on shm  ——> if enough then publish
//
void IceoryxChannelBackend::Publish(runtime::core::channel::MsgWrapper& msg_wrapper) noexcept {
  try {
    AIMRT_CHECK_ERROR_THROW(state_.load() == State::kStart,
                            "Method can only be called when state is 'Start'.");

    namespace util = aimrt::common::util;

    const auto& info = msg_wrapper.info;

    if (const auto* native_type_support = GetDdsNativeTypeSupport(info);
        native_type_support != nullptr) {
      const auto native_pattern = MakeNativePattern(info);
      const auto route_itr = native_publisher_route_map_.find(native_pattern);
      AIMRT_CHECK_ERROR_THROW(
          route_itr != native_publisher_route_map_.end(),
          "Native iceoryx publisher '{}' is not registered.", native_pattern);
      auto& route = *route_itr->second;

      CheckMsg(msg_wrapper);
      auto raw_loan = route.publisher->LoanRaw(
          native_type_support->size, native_type_support->alignment);
      AIMRT_CHECK_ERROR_THROW(
          raw_loan,
          "Loan native iceoryx memory failed for topic '{}' type '{}'.",
          info.topic_name, info.msg_type);

      bool constructed = false;
      try {
        AIMRT_CHECK_ERROR_THROW(
            native_type_support->construct(raw_loan.ptr),
            "Construct DDS native message failed for topic '{}' type '{}'.",
            info.topic_name, info.msg_type);
        constructed = true;
        info.msg_type_support_ref.Copy(msg_wrapper.msg_ptr, raw_loan.ptr);
        route.publisher->PublishRaw(raw_loan.ptr);
        return;
      } catch (...) {
        if (constructed) native_type_support->destroy(raw_loan.ptr);
        route.publisher->ReleaseRaw(raw_loan.ptr);
        throw;
      }
    }

    std::string iceoryx_pub_topic = MakeSerializedPattern(info);

    // find publisher
    auto* iox_pub_ctx_ptr = iceoryx_manager_.GetPublisher(iceoryx_pub_topic);
    AIMRT_CHECK_ERROR_THROW(iox_pub_ctx_ptr != nullptr,
                            "Url: {} not registered for publishing!", iceoryx_pub_topic);

    auto publish_type_support_ref = info.msg_type_support_ref;

    // get serialization type
    std::string_view serialization_type = msg_wrapper.ctx_ref.GetSerializationType();
    if (serialization_type.empty()) {
      serialization_type = publish_type_support_ref.DefaultSerializationType();
    }

    // statistics context meta
    auto [meta_key_vals_array, meta_key_vals_array_len] = msg_wrapper.ctx_ref.GetMetaKeyValsArray();
    AIMRT_CHECK_ERROR_THROW(meta_key_vals_array_len / 2 <= 255,
                            "Too much context meta, require less than 255, but actually {}.", meta_key_vals_array_len / 2);

    size_t context_meta_kv_size = 1;
    for (size_t ii = 0; ii < meta_key_vals_array_len; ++ii) {
      context_meta_kv_size += (2 + meta_key_vals_array[ii].len);
    }

    // check serialization cache
    auto& serialization_cache = msg_wrapper.serialization_cache;
    auto finditr = serialization_cache.find(serialization_type);
    if (finditr != serialization_cache.end()) [[unlikely]] {
      // publish with cache
      auto buffer_array_view_ptr = finditr->second;

      const auto* buffer_array_data = buffer_array_view_ptr->Data();
      const size_t buffer_array_len = buffer_array_view_ptr->Size();
      size_t msg_size = buffer_array_view_ptr->BufferSize();

      size_t shn_size = 4 + 1 + serialization_type.size() + context_meta_kv_size + msg_size;
      auto loaned_shm = iox_pub_ctx_ptr->LoanShm(shn_size);
      util::BufferOperator buf_oper(static_cast<char*>(loaned_shm.Ptr()), loaned_shm.Size());

      buf_oper.SetUint32(shn_size);

      buf_oper.SetString(serialization_type, util::BufferLenType::kUInt8);

      buf_oper.SetUint8(static_cast<uint8_t>(meta_key_vals_array_len / 2));
      for (size_t ii = 0; ii < meta_key_vals_array_len; ++ii) {
        buf_oper.SetString(aimrt::util::ToStdStringView(meta_key_vals_array[ii]), util::BufferLenType::kUInt16);
      }

      for (size_t ii = 0; ii < buffer_array_len; ++ii) {
        buf_oper.SetBuffer(
            static_cast<const char*>(buffer_array_data[ii].data),
            buffer_array_data[ii].len);
      }

      iox_pub_ctx_ptr->PublishShm(loaned_shm);

      AIMRT_TRACE("Iceoryx publish to '{}'", iceoryx_pub_topic);

      return;
    }

    // publish without cache
    CheckMsg(msg_wrapper);

    size_t min_shm_size = 4 + 1 + serialization_type.size() + context_meta_kv_size;
    auto loaned_shm = iox_pub_ctx_ptr->LoanShm(min_shm_size);

    while (true) {
      util::BufferOperator buf_oper(static_cast<char*>(loaned_shm.Ptr()), loaned_shm.Size());

      // skip pkg_size
      buf_oper.Skip(4);

      // write serialization type on loaned shm
      buf_oper.SetString(serialization_type, util::BufferLenType::kUInt8);

      // write context meta on loaned shm
      buf_oper.SetUint8(static_cast<uint8_t>(meta_key_vals_array_len / 2));
      for (size_t ii = 0; ii < meta_key_vals_array_len; ++ii) {
        buf_oper.SetString(aimrt::util::ToStdStringView(meta_key_vals_array[ii]), util::BufferLenType::kUInt16);
      }

      // write msg on loaned shm
      aimrt::util::FlatBufferArrayAllocator allocator(
          static_cast<char*>(loaned_shm.Ptr()) + min_shm_size, loaned_shm.Size() - min_shm_size);
      aimrt::util::BufferArray buffer_array(allocator.NativeHandle());

      bool serialize_ret = info.msg_type_support_ref.Serialize(
          serialization_type,
          msg_wrapper.msg_ptr,
          buffer_array.AllocatorNativeHandle(),
          buffer_array.BufferArrayNativeHandle());

      if (!serialize_ret) [[unlikely]] {
        if (allocator.IsOutOfMemory()) {
          // release old shm and loan a new size shm
          iox_pub_ctx_ptr->UpdateLoanShm(loaned_shm, loaned_shm.Size() * 2);
          continue;
        }

        AIMRT_ERROR_THROW("Serialize failed.");
      }

      // write info pkg length on loaned shm
      buf_oper.JumpTo(0);
      buf_oper.SetUint32(min_shm_size + buffer_array.BufferSize());

      break;
    }

    iox_pub_ctx_ptr->PublishShm(loaned_shm);

    AIMRT_TRACE("Iceoryx publish to '{}'", iceoryx_pub_topic);

    return;
  } catch (const std::exception& e) {
    AIMRT_ERROR("{}", e.what());
  }
}

aimrt_channel_loan_status_t IceoryxChannelBackend::PrepareLoanedPublisher(
    const runtime::core::channel::PublishTypeWrapper& publish_type_wrapper,
    runtime::core::channel::BackendLoanedPublisher& loaned_publisher) noexcept {
  loaned_publisher = {};
  try {
    if (state_.load() != State::kStart)
      return AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE;

    const auto& info = publish_type_wrapper.info;
    if (!info.msg_type.starts_with("dds:"))
      return AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE;
    if (GetDdsNativeTypeSupport(info) == nullptr)
      return AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE;

    const auto pattern = MakeNativePattern(info);
    const auto route_itr = native_publisher_route_map_.find(pattern);
    if (route_itr == native_publisher_route_map_.end())
      return AIMRT_CHANNEL_LOAN_STATUS_UNREGISTERED_MESSAGE_TYPE;

    loaned_publisher.impl = route_itr->second.get();
    loaned_publisher.borrow = &BorrowNativeMessage;
    loaned_publisher.publish = &PublishNativeMessage;
    return AIMRT_CHANNEL_LOAN_STATUS_OK;
  } catch (...) {
    return AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR;
  }
}

aimrt_channel_loan_status_t IceoryxChannelBackend::BorrowNativeMessage(
    void* impl,
    aimrt_channel_loaned_message_base_t& loaned_msg) noexcept {
  loaned_msg = {};
  auto* route = static_cast<NativePublisherRoute*>(impl);
  if (route == nullptr || route->publisher == nullptr ||
      route->type_support == nullptr)
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;

  auto raw_loan = route->publisher->LoanRaw(
      route->type_support->size, route->type_support->alignment);
  if (!raw_loan) return AIMRT_CHANNEL_LOAN_STATUS_LOAN_UNAVAILABLE;
  if (!route->type_support->construct(raw_loan.ptr)) {
    route->publisher->ReleaseRaw(raw_loan.ptr);
    return AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR;
  }

  loaned_msg.msg_ptr = raw_loan.ptr;
  loaned_msg.impl = route;
  loaned_msg.release = &ReleaseNativeMessage;
  return AIMRT_CHANNEL_LOAN_STATUS_OK;
}

aimrt_channel_loan_status_t IceoryxChannelBackend::PublishNativeMessage(
    void* impl,
    aimrt::channel::ContextRef,
    aimrt_channel_loaned_message_base_t& loaned_msg) noexcept {
  auto* route = static_cast<NativePublisherRoute*>(impl);
  if (route == nullptr || route->publisher == nullptr ||
      loaned_msg.msg_ptr == nullptr || loaned_msg.impl != route ||
      loaned_msg.release != &ReleaseNativeMessage)
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;

  route->publisher->PublishRaw(loaned_msg.msg_ptr);
  loaned_msg = {};
  return AIMRT_CHANNEL_LOAN_STATUS_OK;
}

aimrt_channel_loan_status_t IceoryxChannelBackend::ReleaseNativeMessage(
    void* impl, void* msg_ptr) noexcept {
  auto* route = static_cast<NativePublisherRoute*>(impl);
  if (route == nullptr || route->publisher == nullptr ||
      route->type_support == nullptr || msg_ptr == nullptr)
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;

  route->type_support->destroy(msg_ptr);
  route->publisher->ReleaseRaw(msg_ptr);
  return AIMRT_CHANNEL_LOAN_STATUS_OK;
}

aimrt_channel_loan_status_t IceoryxChannelBackend::SubscribeLoaned(
    const runtime::core::channel::LoanedSubscribeWrapper& subscribe_wrapper) noexcept {
  try {
    if (state_.load() != State::kInit)
      return AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE;

    const auto& info = subscribe_wrapper.info;
    if (!info.msg_type.starts_with("dds:") ||
        GetDdsNativeTypeSupport(info) == nullptr)
      return AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE;

    auto* route = GetOrCreateNativeSubscriptionRoute(info);
    route->loaned_subscribe_tool.AddSubscribeWrapper(&subscribe_wrapper);
    route->has_loaned_subscriber = true;
    return AIMRT_CHANNEL_LOAN_STATUS_OK;
  } catch (const std::exception& e) {
    AIMRT_ERROR("Register Iceoryx loaned subscriber failed: {}", e.what());
    return AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR;
  } catch (...) {
    return AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR;
  }
}

}  // namespace aimrt::plugins::iceoryx_plugin

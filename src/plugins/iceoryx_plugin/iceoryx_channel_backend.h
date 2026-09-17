// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "core/channel/channel_backend_base.h"
#include "core/channel/channel_backend_tools.h"
#include "core/util/thread_tools.h"
#include "iceoryx_plugin/iceoryx_manager.h"

namespace aimrt::plugins::iceoryx_plugin {

class IceoryxChannelBackend : public runtime::core::channel::ChannelBackendBase {
 public:
  struct Options {
    std::string listener_thread_name;
    std::string listener_thread_sched_policy;
    std::vector<uint32_t> listener_thread_bind_cpu;
  };

 public:
  IceoryxChannelBackend(IceoryxManager& iceoryx_manager)
      : iceoryx_manager_(iceoryx_manager) {}

  ~IceoryxChannelBackend() override = default;

  std::string_view Name() const noexcept override { return "iceoryx"; }

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  void SetChannelRegistry(const runtime::core::channel::ChannelRegistry* channel_registry_ptr) noexcept override {
    channel_registry_ptr_ = channel_registry_ptr;
  }

  bool RegisterPublishType(
      const runtime::core::channel::PublishTypeWrapper& publish_type_wrapper) noexcept override;
  bool Subscribe(const runtime::core::channel::SubscribeWrapper& subscribe_wrapper) noexcept override;
  void Publish(runtime::core::channel::MsgWrapper& msg_wrapper) noexcept override;
  aimrt_channel_loan_status_t PrepareLoanedPublisher(
      const runtime::core::channel::PublishTypeWrapper& publish_type_wrapper,
      runtime::core::channel::BackendLoanedPublisher& loaned_publisher) noexcept override;
  aimrt_channel_loan_status_t SubscribeLoaned(
      const runtime::core::channel::LoanedSubscribeWrapper& subscribe_wrapper) noexcept override;

 private:
  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

  struct NativePublisherRoute {
    IoxPublisher* publisher = nullptr;
    const aimrt_native_loan_type_support_t* type_support = nullptr;
  };

  struct NativeSubscriptionRoute {
    aimrt::util::TypeSupportRef msg_type_support_ref;
    runtime::core::channel::SubscribeTool ordinary_subscribe_tool;
    runtime::core::channel::LoanedSubscribeTool loaned_subscribe_tool;
    bool has_ordinary_subscriber = false;
    bool has_loaned_subscriber = false;
  };

  static std::string MakeSerializedPattern(
      const runtime::core::channel::TopicInfo& info);
  static std::string MakeNativePattern(
      const runtime::core::channel::TopicInfo& info);
  static const aimrt_native_loan_type_support_t* GetDdsNativeTypeSupport(
      const runtime::core::channel::TopicInfo& info) noexcept;

  void SetListenerThreadOptionsOnce();
  NativeSubscriptionRoute* GetOrCreateNativeSubscriptionRoute(
      const runtime::core::channel::TopicInfo& info);

  static aimrt_channel_loan_status_t BorrowNativeMessage(
      void* impl,
      aimrt_channel_loaned_message_base_t& loaned_msg) noexcept;
  static aimrt_channel_loan_status_t PublishNativeMessage(
      void* impl,
      aimrt::channel::ContextRef ctx_ref,
      aimrt_channel_loaned_message_base_t& loaned_msg) noexcept;
  static aimrt_channel_loan_status_t ReleaseNativeMessage(
      void* impl, void* msg_ptr) noexcept;

  Options options_;
  std::atomic<State> state_ = State::kPreInit;
  bool sched_info_set_ = false;

  const runtime::core::channel::ChannelRegistry* channel_registry_ptr_ = nullptr;

  IceoryxManager& iceoryx_manager_;

  std::unordered_map<
      std::string,
      std::unique_ptr<aimrt::runtime::core::channel::SubscribeTool>>
      subscribe_wrapper_map_;
  std::unordered_map<std::string, std::unique_ptr<NativePublisherRoute>>
      native_publisher_route_map_;
  std::unordered_map<std::string, std::unique_ptr<NativeSubscriptionRoute>>
      native_subscription_route_map_;
};

}  // namespace aimrt::plugins::iceoryx_plugin

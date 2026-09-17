// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include "aimrt_module_c_interface/channel/channel_handle_base.h"
#include "core/channel/channel_registry.h"

#include "yaml-cpp/yaml.h"

namespace aimrt::runtime::core::channel {

struct BackendLoanedPublisher {
  void* impl = nullptr;
  aimrt_channel_loan_status_t (*borrow)(
      void* impl,
      aimrt_channel_loaned_message_base_t& loaned_msg) noexcept = nullptr;
  aimrt_channel_loan_status_t (*publish)(
      void* impl,
      aimrt::channel::ContextRef ctx_ref,
      aimrt_channel_loaned_message_base_t& loaned_msg) noexcept = nullptr;
};

class ChannelBackendBase {
 public:
  ChannelBackendBase() = default;
  virtual ~ChannelBackendBase() = default;

  ChannelBackendBase(const ChannelBackendBase&) = delete;
  ChannelBackendBase& operator=(const ChannelBackendBase&) = delete;

  virtual std::string_view Name() const noexcept = 0;  // It should always return the same value

  virtual void Initialize(YAML::Node options_node) = 0;
  virtual void Start() = 0;
  virtual void Shutdown() = 0;

  virtual std::list<std::pair<std::string, std::string>> GenInitializationReport() const noexcept { return {}; }

  /**
   * @brief Set the Channel Registry to backend
   * @note
   * 1. This method will only be called once before 'Initialize'.
   *
   * @param channel_registry_ptr
   */
  virtual void SetChannelRegistry(const ChannelRegistry* channel_registry_ptr) noexcept {}

  /**
   * @brief Register publish type
   * @note
   * 1. This method will only be called after 'Initialize' and before 'Start'.
   *
   * @param publish_type_wrapper
   * @return Register result
   */
  virtual bool RegisterPublishType(
      const PublishTypeWrapper& publish_type_wrapper) noexcept = 0;

  /**
   * @brief Subscribe
   * @note
   * 1. This method will only be called after 'Initialize' and before 'Start'.
   *
   * @param subscribe_wrapper
   * @return Subscribe result
   */
  virtual bool Subscribe(const SubscribeWrapper& subscribe_wrapper) noexcept = 0;

  /**
   * @brief Publish
   * @note
   * 1. This method will only be called after 'Start' and before 'Shutdown'.
   *
   * @param publish_wrapper
   */
  virtual void Publish(MsgWrapper& msg_wrapper) noexcept = 0;

  /**
   * @brief Borrow a backend-owned native message.
   * @note Default implementation keeps existing backends source-compatible.
   */
  virtual aimrt_channel_loan_status_t PrepareLoanedPublisher(
      const PublishTypeWrapper& publish_type_wrapper,
      BackendLoanedPublisher& loaned_publisher) noexcept {
    return AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_BACKEND;
  }

  /**
   * @brief Register a callback-scoped loaned subscription.
   */
  virtual aimrt_channel_loan_status_t SubscribeLoaned(
      const LoanedSubscribeWrapper& subscribe_wrapper) noexcept {
    return AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_BACKEND;
  }
};

}  // namespace aimrt::runtime::core::channel

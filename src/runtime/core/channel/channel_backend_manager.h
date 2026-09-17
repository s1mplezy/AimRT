// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "aimrt_module_c_interface/util/function_base.h"
#include "core/channel/channel_backend_base.h"
#include "core/channel/channel_framework_async_filter.h"
#include "core/channel/channel_registry.h"
#include "util/log_util.h"

namespace aimrt::runtime::core::channel {

struct RegisterPublishTypeProxyInfoWrapper {
  std::string_view pkg_path;
  std::string_view module_name;
  std::string_view topic_name;

  const aimrt_type_support_base_t* msg_type_support;
};

struct PublishProxyInfoWrapper {
  std::string_view pkg_path;
  std::string_view module_name;
  std::string_view topic_name;

  aimrt_string_view_t msg_type;
  const aimrt_channel_context_base_t* ctx_ptr;
  const void* msg_ptr;
};

struct SubscribeProxyInfoWrapper {
  std::string_view pkg_path;
  std::string_view module_name;
  std::string_view topic_name;

  const aimrt_type_support_base_t* msg_type_support;
  aimrt_function_base_t* callback;
};

struct PrepareLoanedPublisherProxyInfoWrapper {
  std::string_view pkg_path;
  std::string_view module_name;
  std::string_view topic_name;
  aimrt_string_view_t msg_type;
  aimrt_channel_loaned_publisher_base_t* output;
};

struct SubscribeLoanedProxyInfoWrapper {
  std::string_view pkg_path;
  std::string_view module_name;
  std::string_view topic_name;
  const aimrt_type_support_base_t* msg_type_support;
  aimrt_function_base_t* callback;
};

struct LoanedMessageDiagnostics {
  uint64_t outstanding_loans = 0;
  uint64_t borrow_failures = 0;
  uint64_t max_hold_duration_ns = 0;
};

class ChannelBackendManager {
 public:
  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

 public:
  ChannelBackendManager()
      : logger_ptr_(std::make_shared<aimrt::common::util::LoggerWrapper>()) {}
  ~ChannelBackendManager() = default;

  ChannelBackendManager(const ChannelBackendManager&) = delete;
  ChannelBackendManager& operator=(const ChannelBackendManager&) = delete;

  void Initialize();
  void Start();
  void Shutdown();

  State GetState() const { return state_.load(); }

  void SetLogger(const std::shared_ptr<aimrt::common::util::LoggerWrapper>& logger_ptr) { logger_ptr_ = logger_ptr; }
  const aimrt::common::util::LoggerWrapper& GetLogger() const { return *logger_ptr_; }

  void SetChannelRegistry(ChannelRegistry* channel_registry_ptr);

  void SetPublishFiltersRules(
      const std::vector<std::pair<std::string, std::vector<std::string>>>& rules);
  void SetSubscribeFiltersRules(
      const std::vector<std::pair<std::string, std::vector<std::string>>>& rules);

  void SetPublishFrameworkAsyncChannelFilterManager(FrameworkAsyncChannelFilterManager* ptr);
  void SetSubscribeFrameworkAsyncChannelFilterManager(FrameworkAsyncChannelFilterManager* ptr);

  void SetPubTopicsBackendsRules(
      const std::vector<std::pair<std::string, std::vector<std::string>>>& rules);
  void SetSubTopicsBackendsRules(
      const std::vector<std::pair<std::string, std::vector<std::string>>>& rules);

  void RegisterChannelBackend(ChannelBackendBase* channel_backend_ptr);

  // for proxy
  bool Subscribe(SubscribeProxyInfoWrapper&& wrapper);
  bool RegisterPublishType(RegisterPublishTypeProxyInfoWrapper&& wrapper);
  void Publish(PublishProxyInfoWrapper&& wrapper);
  aimrt_channel_loan_status_t PrepareLoanedPublisher(
      PrepareLoanedPublisherProxyInfoWrapper&& wrapper);
  aimrt_channel_loan_status_t SubscribeLoaned(
      SubscribeLoanedProxyInfoWrapper&& wrapper);

  // for framework
  bool Subscribe(SubscribeWrapper&& wrapper);
  bool RegisterPublishType(PublishTypeWrapper&& wrapper);
  void Publish(MsgWrapper&& wrapper);

  using TopicBackendInfoMap = std::unordered_map<std::string_view, std::vector<std::string_view>>;
  TopicBackendInfoMap GetPubTopicBackendInfo() const;
  TopicBackendInfoMap GetSubTopicBackendInfo() const;
  LoanedMessageDiagnostics GetLoanedMessageDiagnostics() const noexcept;

 private:
  std::vector<ChannelBackendBase*> GetBackendsByRules(
      std::string_view topic_name,
      const std::vector<std::pair<std::string, std::vector<std::string>>>& rules);

  std::vector<std::string> GetFilterRules(
      std::string_view topic_name,
      const std::vector<std::pair<std::string, std::vector<std::string>>>& rules);

  struct PreparedLoanedPublisherRoute {
    ChannelBackendManager* manager_ptr;
    BackendLoanedPublisher backend_route;
    std::atomic<uint32_t>* sequence_ptr;
    std::atomic<uint64_t> outstanding_loans = 0;
    std::atomic<uint64_t> borrow_failures = 0;
    std::atomic<uint64_t> max_hold_duration_ns = 0;
  };

  struct TrackedPublisherLoan {
    PreparedLoanedPublisherRoute* route;
    void* backend_impl;
    aimrt_channel_loan_status_t (*backend_release)(void*, void*);
    bool release_operation_held = false;
  };

  class LoanOperationGuard {
   public:
    LoanOperationGuard() = default;
    explicit LoanOperationGuard(ChannelBackendManager* manager) : manager_(manager) {}
    ~LoanOperationGuard();
    LoanOperationGuard(const LoanOperationGuard&) = delete;
    LoanOperationGuard& operator=(const LoanOperationGuard&) = delete;
    LoanOperationGuard(LoanOperationGuard&& rhs) noexcept
        : manager_(std::exchange(rhs.manager_, nullptr)) {}
    explicit operator bool() const noexcept { return manager_ != nullptr; }
    void Disarm() noexcept { manager_ = nullptr; }

   private:
    ChannelBackendManager* manager_ = nullptr;
  };

  LoanOperationGuard AcquireLoanOperation() noexcept;
  void FinishLoanOperation() noexcept;

  static aimrt_channel_loan_status_t BorrowFromPreparedRoute(
      void* impl,
      aimrt_channel_loaned_message_base_t* output) noexcept;
  static aimrt_channel_loan_status_t PublishThroughPreparedRoute(
      void* impl,
      const aimrt_channel_context_base_t* ctx_ptr,
      aimrt_channel_loaned_message_base_t* loaned_msg) noexcept;
  static aimrt_channel_loan_status_t ReleaseThroughPreparedRoute(
      void* impl, void* msg_ptr) noexcept;
  static void FinishTrackedLoan(
      void* impl, uint64_t borrowed_timestamp_ns) noexcept;

 private:
  std::atomic<State> state_ = State::kPreInit;
  std::mutex loan_operation_mutex_;
  std::condition_variable loan_operation_cv_;
  bool loan_operations_open_ = false;
  uint64_t loan_operations_in_flight_ = 0;
  uint64_t sub_topic_index_ = 0;
  uint64_t pub_topic_index_ = 0;

  std::shared_ptr<aimrt::common::util::LoggerWrapper> logger_ptr_;

  ChannelRegistry* channel_registry_ptr_;

  // filter
  std::vector<std::pair<std::string, std::vector<std::string>>> publish_filters_rules_;
  std::vector<std::pair<std::string, std::vector<std::string>>> subscribe_filters_rules_;

  FrameworkAsyncChannelFilterManager* publish_filter_manager_ptr_ = nullptr;
  FrameworkAsyncChannelFilterManager* subscribe_filter_manager_ptr_ = nullptr;

  // backend
  std::vector<ChannelBackendBase*> channel_backend_index_vec_;

  std::vector<std::pair<std::string, std::vector<std::string>>> pub_topics_backends_rules_;
  std::vector<std::pair<std::string, std::vector<std::string>>> sub_topics_backends_rules_;

  std::unordered_map<
      std::string,
      std::vector<ChannelBackendBase*>,
      aimrt::common::util::StringHash,
      std::equal_to<>>
      pub_topics_backend_index_map_;

  std::unordered_map<
      std::string,
      std::vector<ChannelBackendBase*>,
      aimrt::common::util::StringHash,
      std::equal_to<>>
      sub_topics_backend_index_map_;

  std::unordered_map<
      std::string,
      std::atomic<uint32_t>,
      aimrt::common::util::StringHash,
      std::equal_to<>>
      pub_topic_seq_map_;

  std::vector<std::unique_ptr<LoanedSubscribeWrapper>> loaned_subscribe_wrapper_vec_;

  mutable std::mutex prepared_loan_route_mutex_;
  std::vector<std::unique_ptr<PreparedLoanedPublisherRoute>> prepared_loan_route_vec_;
  std::unordered_map<const PublishTypeWrapper*, PreparedLoanedPublisherRoute*>
      prepared_loan_route_map_;
  std::atomic<uint64_t> subscriber_outstanding_loans_ = 0;
  std::atomic<uint64_t> subscriber_max_hold_duration_ns_ = 0;
};

}  // namespace aimrt::runtime::core::channel

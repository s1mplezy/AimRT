// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "core/channel/channel_backend_manager.h"

#include <array>
#include <charconv>
#include <chrono>
#include <exception>
#include <new>
#include <regex>
#include <string>
#include <vector>

#include "aimrt_module_c_interface/channel/channel_context_base.h"
#include "aimrt_module_cpp_interface/channel/channel_handle.h"
#include "core/channel/channel_backend_tools.h"
#include "util/time_util.h"

namespace aimrt::runtime::core::channel {

namespace {

void UpdateMaximum(std::atomic<uint64_t>& maximum, uint64_t value) noexcept {
  auto current = maximum.load(std::memory_order_relaxed);
  while (current < value &&
         !maximum.compare_exchange_weak(
             current, value, std::memory_order_relaxed)) {
  }
}

uint64_t GetSteadyTimestampNs() noexcept {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void SetUint64Meta(
    aimrt::channel::ContextRef ctx_ref,
    std::string_view key,
    uint64_t value) {
  std::array<char, 32> buffer{};
  const auto [end, error] =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (error == std::errc())
    ctx_ref.SetMetaValue(
        key,
        std::string_view(
            buffer.data(), static_cast<size_t>(end - buffer.data())));
}

class LoanTrackingScope {
 public:
  LoanTrackingScope(
      std::atomic<uint64_t>& outstanding,
      std::atomic<uint64_t>& max_hold_duration) noexcept
      : outstanding_(outstanding),
        max_hold_duration_(max_hold_duration),
        begin_timestamp_ns_(GetSteadyTimestampNs()) {
    outstanding_.fetch_add(1, std::memory_order_relaxed);
  }

  ~LoanTrackingScope() {
    outstanding_.fetch_sub(1, std::memory_order_relaxed);
    const auto end_timestamp_ns = GetSteadyTimestampNs();
    UpdateMaximum(
        max_hold_duration_,
        end_timestamp_ns >= begin_timestamp_ns_
            ? end_timestamp_ns - begin_timestamp_ns_
            : 0);
  }

 private:
  std::atomic<uint64_t>& outstanding_;
  std::atomic<uint64_t>& max_hold_duration_;
  uint64_t begin_timestamp_ns_;
};

}  // namespace

void ChannelBackendManager::Initialize() {
  AIMRT_CHECK_ERROR_THROW(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Channel backend manager can only be initialized once.");
}

void ChannelBackendManager::Start() {
  AIMRT_CHECK_ERROR_THROW(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");

  {
    std::lock_guard guard(loan_operation_mutex_);
    loan_operations_open_ = true;
  }
  try {
    for (auto& backend : channel_backend_index_vec_) {
      AIMRT_TRACE("Start channel backend '{}'.", backend->Name());
      backend->Start();
    }
  } catch (...) {
    std::lock_guard guard(loan_operation_mutex_);
    loan_operations_open_ = false;
    throw;
  }
}

void ChannelBackendManager::Shutdown() {
  {
    std::unique_lock lock(loan_operation_mutex_);
    if (state_.exchange(State::kShutdown) == State::kShutdown) return;
    loan_operations_open_ = false;
    loan_operation_cv_.wait(
        lock, [this] { return loan_operations_in_flight_ == 0; });
  }

  const auto loan_diagnostics = GetLoanedMessageDiagnostics();
  if (loan_diagnostics.outstanding_loans != 0) {
    AIMRT_FATAL(
        "Channel backend shutdown refused: {} loaned message(s) have not been "
        "returned or published. Release all loans before runtime shutdown.",
        loan_diagnostics.outstanding_loans);
    std::terminate();
  }

  for (auto& backend : channel_backend_index_vec_) {
    AIMRT_TRACE("Shutdown channel backend '{}'.", backend->Name());
    backend->Shutdown();
  }

  // Backend shutdown releases all references to loaned subscription wrappers.
  // Destroy their module-owned callback objects before module libraries unload.
  for (const auto& wrapper : loaned_subscribe_wrapper_vec_)
    channel_registry_ptr_->UnregisterLoanedSubscribe(*wrapper);
  loaned_subscribe_wrapper_vec_.clear();
}

ChannelBackendManager::LoanOperationGuard::~LoanOperationGuard() {
  if (manager_ != nullptr) manager_->FinishLoanOperation();
}

ChannelBackendManager::LoanOperationGuard
ChannelBackendManager::AcquireLoanOperation() noexcept {
  std::lock_guard guard(loan_operation_mutex_);
  if (!loan_operations_open_ || state_.load() != State::kStart) return {};
  ++loan_operations_in_flight_;
  return LoanOperationGuard(this);
}

void ChannelBackendManager::FinishLoanOperation() noexcept {
  std::lock_guard guard(loan_operation_mutex_);
  if (--loan_operations_in_flight_ == 0) loan_operation_cv_.notify_all();
}

void ChannelBackendManager::SetChannelRegistry(ChannelRegistry* channel_registry_ptr) {
  AIMRT_CHECK_ERROR_THROW(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  channel_registry_ptr_ = channel_registry_ptr;
}

void ChannelBackendManager::SetPublishFrameworkAsyncChannelFilterManager(
    FrameworkAsyncChannelFilterManager* ptr) {
  AIMRT_CHECK_ERROR_THROW(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  publish_filter_manager_ptr_ = ptr;
}

void ChannelBackendManager::SetSubscribeFrameworkAsyncChannelFilterManager(
    FrameworkAsyncChannelFilterManager* ptr) {
  AIMRT_CHECK_ERROR_THROW(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  subscribe_filter_manager_ptr_ = ptr;
}

void ChannelBackendManager::SetPublishFiltersRules(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& rules) {
  AIMRT_CHECK_ERROR_THROW(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  publish_filters_rules_ = rules;
}

void ChannelBackendManager::SetSubscribeFiltersRules(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& rules) {
  AIMRT_CHECK_ERROR_THROW(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  subscribe_filters_rules_ = rules;
}

void ChannelBackendManager::SetPubTopicsBackendsRules(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& rules) {
  AIMRT_CHECK_ERROR_THROW(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  pub_topics_backends_rules_ = rules;
}

void ChannelBackendManager::SetSubTopicsBackendsRules(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& rules) {
  AIMRT_CHECK_ERROR_THROW(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  sub_topics_backends_rules_ = rules;
}

void ChannelBackendManager::RegisterChannelBackend(
    ChannelBackendBase* channel_backend_ptr) {
  AIMRT_CHECK_ERROR_THROW(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  channel_backend_index_vec_.emplace_back(channel_backend_ptr);
}

bool ChannelBackendManager::Subscribe(SubscribeProxyInfoWrapper&& wrapper) {
  if (state_.load() != State::kInit) [[unlikely]] {
    AIMRT_ERROR("Msg can only be subscribed when state is 'Init'.");
    return false;
  }

  if (wrapper.msg_type_support == nullptr) [[unlikely]] {
    AIMRT_ERROR("Msg type support is null.");
    return false;
  }

  if (wrapper.callback == nullptr) [[unlikely]] {
    AIMRT_ERROR("Callback is null.");
    return false;
  }

  auto topic_name = wrapper.topic_name;
  auto msg_type_support_ref = aimrt::util::TypeSupportRef(wrapper.msg_type_support);
  auto msg_type = msg_type_support_ref.TypeName();

  // create sub wrapper
  auto sub_wrapper_ptr = std::make_unique<SubscribeWrapper>();
  sub_wrapper_ptr->info = TopicInfo{
      .msg_type = std::string(msg_type),
      .topic_name = std::string(topic_name),
      .pkg_path = std::string(wrapper.pkg_path),
      .module_name = std::string(wrapper.module_name),
      .index = ++sub_topic_index_,
      .msg_type_support_ref = msg_type_support_ref};

  // create filter
  auto filter_name_vec = GetFilterRules(topic_name, subscribe_filters_rules_);
  subscribe_filter_manager_ptr_->CreateFilterCollectorIfNotExist(topic_name, filter_name_vec);

  // set callback
  const auto& filter_collector = subscribe_filter_manager_ptr_->GetFilterCollector(topic_name);

  auto sub_func_shared_ptr = std::make_shared<aimrt::channel::SubscriberCallback>(wrapper.callback);

  sub_wrapper_ptr->callback =
      [this, &filter_collector, sub_func_shared_ptr](
          MsgWrapper& msg_wrapper, std::function<void()>&& input_release_callback) {
        auto release_callback_shared_ptr = std::shared_ptr<std::function<void()>>(
            new std::function<void()>(std::move(input_release_callback)),
            [](std::function<void()>* f) {
              (*f)();
              delete f;
            });

        filter_collector.InvokeChannel(
            [this,
             sub_func_ptr = sub_func_shared_ptr.get(),
             release_callback_shared_ptr](MsgWrapper& msg_wrapper) {
              try {
                CheckMsg(msg_wrapper);

                aimrt::channel::SubscriberReleaseCallback release_callback(
                    [release_callback_shared_ptr, msg_cache_ptr{msg_wrapper.msg_cache_ptr}]() {});

                (*sub_func_ptr)(
                    msg_wrapper.ctx_ref.NativeHandle(),
                    msg_wrapper.msg_ptr,
                    release_callback.NativeHandle());

              } catch (const std::exception& e) {
                AIMRT_ERROR("{}", e.what());
              }
            },
            msg_wrapper);
      };

  // register sub wrapper
  const auto& sub_wrapper_ref = *sub_wrapper_ptr;

  if (!channel_registry_ptr_->Subscribe(std::move(sub_wrapper_ptr)))
    return false;

  auto backend_itr = sub_topics_backend_index_map_.find(topic_name);
  if (backend_itr == sub_topics_backend_index_map_.end()) {
    auto backend_ptr_vec = GetBackendsByRules(topic_name, sub_topics_backends_rules_);
    auto emplace_ret = sub_topics_backend_index_map_.emplace(topic_name, std::move(backend_ptr_vec));
    backend_itr = emplace_ret.first;
  }

  bool ret = true;
  for (auto& itr : backend_itr->second) {
    AIMRT_TRACE("Subscribe type '{}' for topic '{}' to backend '{}'.",
                msg_type, topic_name, itr->Name());
    ret &= itr->Subscribe(sub_wrapper_ref);
  }
  return ret;
}

bool ChannelBackendManager::RegisterPublishType(RegisterPublishTypeProxyInfoWrapper&& wrapper) {
  if (state_.load() != State::kInit) [[unlikely]] {
    AIMRT_ERROR("Publish type can only be registered when state is 'Init'.");
    return false;
  }

  if (wrapper.msg_type_support == nullptr) [[unlikely]] {
    AIMRT_ERROR("Msg type support is null.");
    return false;
  }

  auto topic_name = wrapper.topic_name;
  auto msg_type_support_ref = aimrt::util::TypeSupportRef(wrapper.msg_type_support);
  auto msg_type = msg_type_support_ref.TypeName();

  // create pub wrapper
  auto pub_type_wrapper_ptr = std::make_unique<PublishTypeWrapper>();
  pub_type_wrapper_ptr->info = TopicInfo{
      .msg_type = std::string(msg_type),
      .topic_name = std::string(topic_name),
      .pkg_path = std::string(wrapper.pkg_path),
      .module_name = std::string(wrapper.module_name),
      .index = ++pub_topic_index_,
      .msg_type_support_ref = msg_type_support_ref};

  // initialize publish sequence for this topic (only in Init state)
  pub_topic_seq_map_.try_emplace(std::string(topic_name), 0);

  // create filter
  auto filter_name_vec = GetFilterRules(topic_name, publish_filters_rules_);
  publish_filter_manager_ptr_->CreateFilterCollectorIfNotExist(topic_name, filter_name_vec);

  // register pub wrapper
  const auto& pub_type_wrapper_ref = *pub_type_wrapper_ptr;

  if (!channel_registry_ptr_->RegisterPublishType(std::move(pub_type_wrapper_ptr)))
    return false;

  auto backend_itr = pub_topics_backend_index_map_.find(topic_name);
  if (backend_itr == pub_topics_backend_index_map_.end()) {
    auto backend_ptr_vec = GetBackendsByRules(topic_name, pub_topics_backends_rules_);
    auto emplace_ret = pub_topics_backend_index_map_.emplace(topic_name, std::move(backend_ptr_vec));
    backend_itr = emplace_ret.first;
  }

  bool ret = true;
  for (auto& itr : backend_itr->second) {
    AIMRT_TRACE("Register publish type '{}' for topic '{}' to backend '{}'.",
                msg_type, topic_name, itr->Name());
    ret &= itr->RegisterPublishType(pub_type_wrapper_ref);
  }
  return ret;
}

void ChannelBackendManager::Publish(PublishProxyInfoWrapper&& wrapper) {
  if (state_.load() != State::kStart) [[unlikely]] {
    AIMRT_WARN("Method can only be called when state is 'Start'.");
    return;
  }

  auto msg_type = util::ToStdStringView(wrapper.msg_type);

  aimrt::channel::ContextRef ctx_ref(wrapper.ctx_ptr);

  // Find the registered publisher type
  const auto* pub_type_wrapper_ptr = channel_registry_ptr_->GetPublishTypeWrapperPtr(
      msg_type, wrapper.topic_name, wrapper.pkg_path, wrapper.module_name);

  // If publish type is not registered
  if (pub_type_wrapper_ptr == nullptr) {
    AIMRT_WARN(
        "Publish type unregistered, msg_type: {}, topic_name: {}, pkg_path: {}, module_name: {}.",
        msg_type, wrapper.topic_name, wrapper.pkg_path, wrapper.module_name);
    return;
  }

  // Check ctx
  if (ctx_ref.GetType() != aimrt_channel_context_type_t::AIMRT_CHANNEL_PUBLISHER_CONTEXT ||
      ctx_ref.CheckUsed()) {
    AIMRT_WARN("Publish context has been used!");
    return;
  }

  ctx_ref.SetUsed();

  // set publish sequence into context if available
  auto it = pub_topic_seq_map_.find(wrapper.topic_name);
  if (it != pub_topic_seq_map_.end()) {
    uint32_t seq = ++(it->second);
    ctx_ref.SetMetaValue(AIMRT_CHANNEL_CONTEXT_KEY_PUB_SEQ, std::to_string(seq));
  }
  ctx_ref.SetMetaValue(AIMRT_CHANNEL_CONTEXT_KEY_PUB_TIMESTAMP, std::to_string(aimrt::common::util::GetCurTimestampNs()));

  // Find filter
  const auto& filter_collector = publish_filter_manager_ptr_->GetFilterCollector(wrapper.topic_name);

  // Create a wrapper
  auto publish_msg_wrapper_ptr = std::make_shared<MsgWrapper>(
      MsgWrapper{
          .info = pub_type_wrapper_ptr->info,
          .msg_ptr = wrapper.msg_ptr,
          .ctx_ref = ctx_ref});

  // Start publish
  filter_collector.InvokeChannel(
      [this](MsgWrapper& msg_wrapper) {
        std::string_view topic_name = msg_wrapper.info.topic_name;

        auto find_itr = pub_topics_backend_index_map_.find(topic_name);

        if (find_itr == pub_topics_backend_index_map_.end()) [[unlikely]] {
          AIMRT_WARN("Channel msg has no backend, topic: '{}'.", topic_name);
          return;
        }

        for (auto& itr : find_itr->second) {
          AIMRT_TRACE("Publish msg '{}' for topic '{}' to channel backend '{}'",
                      msg_wrapper.info.msg_type, topic_name, itr->Name());
          itr->Publish(msg_wrapper);
        }
      },
      *publish_msg_wrapper_ptr);
}

aimrt_channel_loan_status_t ChannelBackendManager::PrepareLoanedPublisher(
    PrepareLoanedPublisherProxyInfoWrapper&& wrapper) {
  if (wrapper.output == nullptr) [[unlikely]]
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;
  *wrapper.output = {};

  if (state_.load() != State::kStart) [[unlikely]]
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE;
  auto operation = AcquireLoanOperation();
  if (!operation) return AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE;

  const auto msg_type = util::ToStdStringView(wrapper.msg_type);
  const auto* publish_type_ptr = channel_registry_ptr_->GetPublishTypeWrapperPtr(
      msg_type, wrapper.topic_name, wrapper.pkg_path, wrapper.module_name);
  if (publish_type_ptr == nullptr)
    return AIMRT_CHANNEL_LOAN_STATUS_UNREGISTERED_MESSAGE_TYPE;

  const auto backend_itr = pub_topics_backend_index_map_.find(wrapper.topic_name);
  if (backend_itr == pub_topics_backend_index_map_.end() ||
      backend_itr->second.size() != 1)
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_BACKEND_COUNT;
  if (msg_type.starts_with("pb:"))
    return AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE;
  if (publish_filter_manager_ptr_->HasFilters(wrapper.topic_name))
    return AIMRT_CHANNEL_LOAN_STATUS_INCOMPATIBLE_BACKEND_CONFIG;

  std::lock_guard guard(prepared_loan_route_mutex_);
  auto prepared_itr = prepared_loan_route_map_.find(publish_type_ptr);
  if (prepared_itr == prepared_loan_route_map_.end()) {
    BackendLoanedPublisher backend_route;
    const auto status = backend_itr->second.front()->PrepareLoanedPublisher(
        *publish_type_ptr, backend_route);
    if (status != AIMRT_CHANNEL_LOAN_STATUS_OK)
      return status;
    if (backend_route.impl == nullptr || backend_route.borrow == nullptr ||
        backend_route.publish == nullptr)
      return AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR;

    auto route_ptr = std::make_unique<PreparedLoanedPublisherRoute>();
    route_ptr->manager_ptr = this;
    route_ptr->backend_route = backend_route;
    const auto sequence_itr = pub_topic_seq_map_.find(wrapper.topic_name);
    route_ptr->sequence_ptr = sequence_itr == pub_topic_seq_map_.end()
                                  ? nullptr
                                  : &sequence_itr->second;
    auto* route = route_ptr.get();
    prepared_loan_route_vec_.emplace_back(std::move(route_ptr));
    prepared_itr = prepared_loan_route_map_.emplace(publish_type_ptr, route).first;
  }

  wrapper.output->impl = prepared_itr->second;
  wrapper.output->borrow_loaned_message = &BorrowFromPreparedRoute;
  wrapper.output->publish_loaned_message = &PublishThroughPreparedRoute;
  return AIMRT_CHANNEL_LOAN_STATUS_OK;
}

aimrt_channel_loan_status_t ChannelBackendManager::BorrowFromPreparedRoute(
    void* impl, aimrt_channel_loaned_message_base_t* output) noexcept {
  if (impl == nullptr || output == nullptr)
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;
  *output = {};
  auto& route = *static_cast<PreparedLoanedPublisherRoute*>(impl);
  auto operation = route.manager_ptr->AcquireLoanOperation();
  if (!operation) return AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE;

  route.outstanding_loans.fetch_add(1, std::memory_order_relaxed);
  auto tracked_loan = std::unique_ptr<TrackedPublisherLoan>(
      new (std::nothrow) TrackedPublisherLoan{.route = &route});
  if (!tracked_loan) {
    route.outstanding_loans.fetch_sub(1, std::memory_order_relaxed);
    route.borrow_failures.fetch_add(1, std::memory_order_relaxed);
    return AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR;
  }

  const auto status = route.backend_route.borrow(
      route.backend_route.impl, *output);
  if (status != AIMRT_CHANNEL_LOAN_STATUS_OK) {
    route.borrow_failures.fetch_add(1, std::memory_order_relaxed);
    route.outstanding_loans.fetch_sub(1, std::memory_order_relaxed);
    if (output->release != nullptr) {
      const auto cleanup_status = output->release(output->impl, output->msg_ptr);
      if (cleanup_status != AIMRT_CHANNEL_LOAN_STATUS_OK) std::terminate();
    }
    *output = {};
    return status;
  }
  if (output->msg_ptr == nullptr || output->impl == nullptr ||
      output->release == nullptr) {
    route.borrow_failures.fetch_add(1, std::memory_order_relaxed);
    route.outstanding_loans.fetch_sub(1, std::memory_order_relaxed);
    if (output->release != nullptr) {
      const auto cleanup_status = output->release(output->impl, output->msg_ptr);
      if (cleanup_status != AIMRT_CHANNEL_LOAN_STATUS_OK) std::terminate();
    }
    *output = {};
    return AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR;
  }

  tracked_loan->backend_impl = output->impl;
  tracked_loan->backend_release = output->release;
  output->impl = tracked_loan.get();
  output->release = &ReleaseThroughPreparedRoute;
  output->owner = &route;
  output->tracking_impl = tracked_loan.get();
  output->track_release = &FinishTrackedLoan;
  output->borrowed_timestamp_ns = GetSteadyTimestampNs();
  tracked_loan.release();
  return AIMRT_CHANNEL_LOAN_STATUS_OK;
}

aimrt_channel_loan_status_t ChannelBackendManager::PublishThroughPreparedRoute(
    void* impl,
    const aimrt_channel_context_base_t* ctx_ptr,
    aimrt_channel_loaned_message_base_t* loaned_msg) noexcept {
  if (impl == nullptr || loaned_msg == nullptr || loaned_msg->msg_ptr == nullptr ||
      loaned_msg->impl == nullptr || loaned_msg->release == nullptr)
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;

  auto& route = *static_cast<PreparedLoanedPublisherRoute*>(impl);
  if (loaned_msg->owner != &route ||
      loaned_msg->release != &ReleaseThroughPreparedRoute ||
      loaned_msg->tracking_impl != loaned_msg->impl)
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;
  auto& tracked_loan = *static_cast<TrackedPublisherLoan*>(loaned_msg->impl);
  if (tracked_loan.route != &route)
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;

  auto operation = route.manager_ptr->AcquireLoanOperation();
  if (!operation) return AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE;

  aimrt::channel::ContextRef ctx_ref(ctx_ptr);
  if (!ctx_ref ||
      ctx_ref.GetType() != AIMRT_CHANNEL_PUBLISHER_CONTEXT ||
      ctx_ref.CheckUsed())
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;

  ctx_ref.SetUsed();
  if (route.sequence_ptr != nullptr) {
    const uint32_t sequence = ++(*route.sequence_ptr);
    SetUint64Meta(ctx_ref, AIMRT_CHANNEL_CONTEXT_KEY_PUB_SEQ, sequence);
  }
  SetUint64Meta(
      ctx_ref,
      AIMRT_CHANNEL_CONTEXT_KEY_PUB_TIMESTAMP,
      aimrt::common::util::GetCurTimestampNs());

  auto backend_loaned_msg = *loaned_msg;
  backend_loaned_msg.impl = tracked_loan.backend_impl;
  backend_loaned_msg.release = tracked_loan.backend_release;
  const auto status = route.backend_route.publish(
      route.backend_route.impl, ctx_ref, backend_loaned_msg);

  const bool terminal = backend_loaned_msg.msg_ptr == nullptr &&
                        backend_loaned_msg.impl == nullptr &&
                        backend_loaned_msg.release == nullptr;
  const bool retained = backend_loaned_msg.msg_ptr == loaned_msg->msg_ptr &&
                        backend_loaned_msg.impl == tracked_loan.backend_impl &&
                        backend_loaned_msg.release == tracked_loan.backend_release;
  if (terminal) {
    const auto tracking_impl = loaned_msg->tracking_impl;
    const auto track_release = loaned_msg->track_release;
    const auto borrowed_timestamp_ns = loaned_msg->borrowed_timestamp_ns;
    if (track_release != nullptr)
      track_release(tracking_impl, borrowed_timestamp_ns);
    *loaned_msg = {};
  } else if (!retained || status == AIMRT_CHANNEL_LOAN_STATUS_OK) {
    AIMRT_HANDLE_LOG(
        route.manager_ptr->GetLogger(), aimrt::common::util::kLogLevelError,
        "Loaned channel backend returned an invalid ownership disposition");
    return AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR;
  }
  return status;
}

aimrt_channel_loan_status_t ChannelBackendManager::ReleaseThroughPreparedRoute(
    void* impl, void* msg_ptr) noexcept {
  if (impl == nullptr || msg_ptr == nullptr)
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;
  auto& tracked_loan = *static_cast<TrackedPublisherLoan*>(impl);
  if (tracked_loan.route == nullptr || tracked_loan.backend_impl == nullptr ||
      tracked_loan.backend_release == nullptr)
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;
  auto operation = tracked_loan.route->manager_ptr->AcquireLoanOperation();
  if (!operation) return AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE;
  const auto status =
      tracked_loan.backend_release(tracked_loan.backend_impl, msg_ptr);
  if (status == AIMRT_CHANNEL_LOAN_STATUS_OK) {
    tracked_loan.release_operation_held = true;
    operation.Disarm();
  }
  return status;
}

void ChannelBackendManager::FinishTrackedLoan(
    void* impl, uint64_t borrowed_timestamp_ns) noexcept {
  if (impl == nullptr) return;
  std::unique_ptr<TrackedPublisherLoan> tracked_loan(
      static_cast<TrackedPublisherLoan*>(impl));
  auto& route = *tracked_loan->route;
  auto* manager = route.manager_ptr;
  const bool release_operation_held = tracked_loan->release_operation_held;
  route.outstanding_loans.fetch_sub(1, std::memory_order_relaxed);
  const auto now = GetSteadyTimestampNs();
  const auto duration = now >= borrowed_timestamp_ns
                            ? now - borrowed_timestamp_ns
                            : 0;
  UpdateMaximum(route.max_hold_duration_ns, duration);
  if (release_operation_held) manager->FinishLoanOperation();
}

aimrt_channel_loan_status_t ChannelBackendManager::SubscribeLoaned(
    SubscribeLoanedProxyInfoWrapper&& wrapper) {
  if (state_.load() != State::kInit) [[unlikely]]
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE;
  if (wrapper.msg_type_support == nullptr || wrapper.callback == nullptr)
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;

  auto backend_itr = sub_topics_backend_index_map_.find(wrapper.topic_name);
  if (backend_itr == sub_topics_backend_index_map_.end()) {
    auto backend_ptr_vec = GetBackendsByRules(
        wrapper.topic_name, sub_topics_backends_rules_);
    auto emplace_ret = sub_topics_backend_index_map_.emplace(
        wrapper.topic_name, std::move(backend_ptr_vec));
    backend_itr = emplace_ret.first;
  }
  if (backend_itr->second.size() != 1)
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_BACKEND_COUNT;

  auto msg_type_support_ref = aimrt::util::TypeSupportRef(wrapper.msg_type_support);
  auto msg_type = msg_type_support_ref.TypeName();
  if (msg_type.starts_with("pb:"))
    return AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE;

  auto filter_names = GetFilterRules(wrapper.topic_name, subscribe_filters_rules_);
  if (!filter_names.empty())
    return AIMRT_CHANNEL_LOAN_STATUS_INCOMPATIBLE_BACKEND_CONFIG;

  auto callback_ptr =
      std::make_shared<aimrt::channel::SubscriberLoanedCallback>(wrapper.callback);
  auto loaned_wrapper_ptr = std::make_unique<LoanedSubscribeWrapper>();
  loaned_wrapper_ptr->info = TopicInfo{
      .msg_type = std::string(msg_type),
      .topic_name = std::string(wrapper.topic_name),
      .pkg_path = std::string(wrapper.pkg_path),
      .module_name = std::string(wrapper.module_name),
      .index = ++sub_topic_index_,
      .msg_type_support_ref = msg_type_support_ref};
  loaned_wrapper_ptr->callback =
      [this, callback_ptr](aimrt::channel::ContextRef ctx_ref, const void* msg_ptr) {
        auto operation = AcquireLoanOperation();
        if (!operation) return;
        LoanTrackingScope tracking_scope(
            subscriber_outstanding_loans_, subscriber_max_hold_duration_ns_);
        try {
          (*callback_ptr)(ctx_ref.NativeHandle(), msg_ptr);
        } catch (const std::exception& e) {
          AIMRT_ERROR("Loaned subscriber callback failed: {}", e.what());
        } catch (...) {
          AIMRT_ERROR("Loaned subscriber callback failed with an unknown exception.");
        }
      };

  if (!channel_registry_ptr_->RegisterLoanedSubscribe(*loaned_wrapper_ptr))
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;

  auto status = backend_itr->second.front()->SubscribeLoaned(*loaned_wrapper_ptr);
  if (status == AIMRT_CHANNEL_LOAN_STATUS_OK) {
    loaned_subscribe_wrapper_vec_.emplace_back(std::move(loaned_wrapper_ptr));
  } else {
    channel_registry_ptr_->UnregisterLoanedSubscribe(*loaned_wrapper_ptr);
  }
  return status;
}

bool ChannelBackendManager::Subscribe(SubscribeWrapper&& wrapper) {
  if (state_.load() != State::kInit) [[unlikely]] {
    AIMRT_ERROR("Msg can only be subscribed when state is 'Init'.");
    return false;
  }

  auto sub_wrapper_ptr = std::make_unique<SubscribeWrapper>(std::move(wrapper));

  std::string_view topic_name = sub_wrapper_ptr->info.topic_name;
  std::string_view msg_type = sub_wrapper_ptr->info.msg_type;

  // create filter
  auto filter_name_vec = GetFilterRules(topic_name, subscribe_filters_rules_);
  subscribe_filter_manager_ptr_->CreateFilterCollectorIfNotExist(topic_name, filter_name_vec);

  // set callback
  const auto& filter_collector = subscribe_filter_manager_ptr_->GetFilterCollector(topic_name);

  sub_wrapper_ptr->callback =
      [&filter_collector, callback{std::move(sub_wrapper_ptr->callback)}](
          MsgWrapper& msg_wrapper, std::function<void()>&& input_release_callback) {
        auto release_callback_shared_ptr = std::shared_ptr<std::function<void()>>(
            new std::function<void()>(std::move(input_release_callback)),
            [](std::function<void()>* f) {
              (*f)();
              delete f;
            });

        filter_collector.InvokeChannel(
            [&callback, release_callback_shared_ptr](MsgWrapper& msg_wrapper) {
              callback(
                  msg_wrapper,
                  [release_callback_shared_ptr, msg_cache_ptr{msg_wrapper.msg_cache_ptr}]() {});
            },
            msg_wrapper);
      };

  // register sub wrapper
  const auto& sub_wrapper_ref = *sub_wrapper_ptr;

  if (!channel_registry_ptr_->Subscribe(std::move(sub_wrapper_ptr)))
    return false;

  auto backend_itr = sub_topics_backend_index_map_.find(topic_name);
  if (backend_itr == sub_topics_backend_index_map_.end()) {
    auto backend_ptr_vec = GetBackendsByRules(topic_name, sub_topics_backends_rules_);
    auto emplace_ret = sub_topics_backend_index_map_.emplace(topic_name, std::move(backend_ptr_vec));
    backend_itr = emplace_ret.first;
  }

  bool ret = true;
  for (auto& itr : backend_itr->second) {
    AIMRT_TRACE("Subscribe type '{}' for topic '{}' to backend '{}'.",
                msg_type, topic_name, itr->Name());
    ret &= itr->Subscribe(sub_wrapper_ref);
  }
  return ret;
}

bool ChannelBackendManager::RegisterPublishType(PublishTypeWrapper&& wrapper) {
  if (state_.load() != State::kInit) [[unlikely]] {
    AIMRT_ERROR("Publish type can only be registered when state is 'Init'.");
    return false;
  }

  auto pub_type_wrapper_ptr = std::make_unique<PublishTypeWrapper>(std::move(wrapper));

  std::string_view topic_name = pub_type_wrapper_ptr->info.topic_name;
  std::string_view msg_type = pub_type_wrapper_ptr->info.msg_type;

  // initialize publish sequence for this topic (only in Init state)
  pub_topic_seq_map_.try_emplace(std::string(topic_name), 0);

  // create filter
  auto filter_name_vec = GetFilterRules(topic_name, publish_filters_rules_);
  publish_filter_manager_ptr_->CreateFilterCollectorIfNotExist(topic_name, filter_name_vec);

  // register pub wrapper
  const auto& pub_type_wrapper_ref = *pub_type_wrapper_ptr;

  if (!channel_registry_ptr_->RegisterPublishType(std::move(pub_type_wrapper_ptr)))
    return false;

  auto backend_itr = pub_topics_backend_index_map_.find(topic_name);
  if (backend_itr == pub_topics_backend_index_map_.end()) {
    auto backend_ptr_vec = GetBackendsByRules(topic_name, pub_topics_backends_rules_);
    auto emplace_ret = pub_topics_backend_index_map_.emplace(topic_name, std::move(backend_ptr_vec));
    backend_itr = emplace_ret.first;
  }

  bool ret = true;
  for (auto& itr : backend_itr->second) {
    AIMRT_TRACE("Register publish type '{}' for topic '{}' to backend '{}'.",
                msg_type, topic_name, itr->Name());
    ret &= itr->RegisterPublishType(pub_type_wrapper_ref);
  }
  return ret;
}

void ChannelBackendManager::Publish(MsgWrapper&& wrapper) {
  if (state_.load() != State::kStart) [[unlikely]] {
    AIMRT_WARN("Method can only be called when state is 'Start'.");
    return;
  }

  auto publish_msg_wrapper_ptr = std::make_shared<MsgWrapper>(std::move(wrapper));

  auto ctx_ref = publish_msg_wrapper_ptr->ctx_ref;

  // Check ctx
  if (ctx_ref.GetType() != aimrt_channel_context_type_t::AIMRT_CHANNEL_PUBLISHER_CONTEXT ||
      ctx_ref.CheckUsed()) {
    AIMRT_WARN("Publish context has been used!");
    return;
  }

  ctx_ref.SetUsed();

  // set publish sequence into context if available
  auto it = pub_topic_seq_map_.find(publish_msg_wrapper_ptr->info.topic_name);
  if (it != pub_topic_seq_map_.end()) {
    uint32_t seq = ++(it->second);
    ctx_ref.SetMetaValue(AIMRT_CHANNEL_CONTEXT_KEY_PUB_SEQ, std::to_string(seq));
  }

  ctx_ref.SetMetaValue(AIMRT_CHANNEL_CONTEXT_KEY_PUB_TIMESTAMP, std::to_string(aimrt::common::util::GetCurTimestampNs()));

  // Find filter
  const auto& filter_collector =
      publish_filter_manager_ptr_->GetFilterCollector(publish_msg_wrapper_ptr->info.topic_name);

  // Start publish
  filter_collector.InvokeChannel(
      [this](MsgWrapper& msg_wrapper) {
        std::string_view topic_name = msg_wrapper.info.topic_name;

        auto find_itr = pub_topics_backend_index_map_.find(topic_name);

        if (find_itr == pub_topics_backend_index_map_.end()) [[unlikely]] {
          AIMRT_WARN("Channel msg has no backend, topic: '{}'.", topic_name);
          return;
        }

        for (auto& itr : find_itr->second) {
          AIMRT_TRACE("Publish msg '{}' for topic '{}' to channel backend '{}'",
                      msg_wrapper.info.msg_type, topic_name, itr->Name());
          itr->Publish(msg_wrapper);
        }
      },
      *publish_msg_wrapper_ptr);
}

ChannelBackendManager::TopicBackendInfoMap ChannelBackendManager::GetPubTopicBackendInfo() const {
  std::unordered_map<std::string_view, std::vector<std::string_view>> result;
  for (const auto& itr : pub_topics_backend_index_map_) {
    std::vector<std::string_view> backends_name;
    backends_name.reserve(itr.second.size());
    for (const auto& item : itr.second)
      backends_name.emplace_back(item->Name());

    result.emplace(itr.first, std::move(backends_name));
  }

  return result;
}

ChannelBackendManager::TopicBackendInfoMap ChannelBackendManager::GetSubTopicBackendInfo() const {
  std::unordered_map<std::string_view, std::vector<std::string_view>> result;
  for (const auto& itr : sub_topics_backend_index_map_) {
    std::vector<std::string_view> backends_name;
    backends_name.reserve(itr.second.size());
    for (const auto& item : itr.second)
      backends_name.emplace_back(item->Name());

    result.emplace(itr.first, std::move(backends_name));
  }

  return result;
}

LoanedMessageDiagnostics
ChannelBackendManager::GetLoanedMessageDiagnostics() const noexcept {
  LoanedMessageDiagnostics result{
      .outstanding_loans =
          subscriber_outstanding_loans_.load(std::memory_order_relaxed),
      .borrow_failures = 0,
      .max_hold_duration_ns =
          subscriber_max_hold_duration_ns_.load(std::memory_order_relaxed)};
  std::lock_guard guard(prepared_loan_route_mutex_);
  for (const auto& route_ptr : prepared_loan_route_vec_) {
    result.outstanding_loans +=
        route_ptr->outstanding_loans.load(std::memory_order_relaxed);
    result.borrow_failures +=
        route_ptr->borrow_failures.load(std::memory_order_relaxed);
    result.max_hold_duration_ns = std::max(
        result.max_hold_duration_ns,
        route_ptr->max_hold_duration_ns.load(std::memory_order_relaxed));
  }
  return result;
}

std::vector<ChannelBackendBase*> ChannelBackendManager::GetBackendsByRules(
    std::string_view topic_name,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& rules) {
  for (const auto& item : rules) {
    const auto& topic_regex = item.first;
    const auto& enable_backends = item.second;

    try {
      if (std::regex_match(topic_name.begin(), topic_name.end(), std::regex(topic_regex, std::regex::ECMAScript))) {
        std::vector<ChannelBackendBase*> backend_ptr_vec;

        for (const auto& backend_name : enable_backends) {
          auto itr = std::find_if(
              channel_backend_index_vec_.begin(), channel_backend_index_vec_.end(),
              [&backend_name](const ChannelBackendBase* backend_ptr) -> bool {
                return backend_ptr->Name() == backend_name;
              });

          if (itr == channel_backend_index_vec_.end()) [[unlikely]] {
            AIMRT_WARN("Can not find '{}' in backend list.", backend_name);
            continue;
          }

          backend_ptr_vec.emplace_back(*itr);
        }

        return backend_ptr_vec;
      }
    } catch (const std::exception& e) {
      AIMRT_WARN("Regex get exception, expr: {}, string: {}, exception info: {}",
                 topic_regex, topic_name, e.what());
    }
  }

  return {};
}

std::vector<std::string> ChannelBackendManager::GetFilterRules(
    std::string_view topic_name,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& rules) {
  for (const auto& item : rules) {
    const auto& topic_regex = item.first;
    const auto& filters = item.second;

    try {
      if (std::regex_match(topic_name.begin(), topic_name.end(), std::regex(topic_regex, std::regex::ECMAScript))) {
        return filters;
      }
    } catch (const std::exception& e) {
      AIMRT_WARN("Regex get exception, expr: {}, string: {}, exception info: {}",
                 topic_regex, topic_name, e.what());
    }
  }

  return {};
}
}  // namespace aimrt::runtime::core::channel

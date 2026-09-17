// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <atomic>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <utility>

#include "aimrt_module_c_interface/channel/channel_handle_base.h"
#include "aimrt_module_cpp_interface/channel/channel_context.h"
#include "aimrt_module_cpp_interface/util/function.h"
#include "aimrt_module_cpp_interface/util/string.h"
#include "util/exception.h"

namespace aimrt::channel {

using SubscriberReleaseCallback = aimrt::util::Function<aimrt_function_subscriber_release_callback_ops_t>;
using SubscriberCallback = aimrt::util::Function<aimrt_function_subscriber_callback_ops_t>;
using SubscriberLoanedCallback = aimrt::util::Function<aimrt_function_subscriber_loaned_callback_ops_t>;

using LoanStatus = aimrt_channel_loan_status_t;

inline bool LoanSucceeded(LoanStatus status) {
  return status == AIMRT_CHANNEL_LOAN_STATUS_OK;
}

namespace details {

inline void ReportLoanedSubscriberCallbackException(
    const char* message) noexcept {
  std::fprintf(
      stderr,
      "AimRT loaned subscriber callback threw an exception: %s\n",
      message != nullptr ? message : "unknown exception");
  std::fflush(stderr);
}

}  // namespace details

inline LoanStatus ReleaseLoanedMessage(
    aimrt_channel_loaned_message_base_t& loaned_msg) noexcept {
  if (loaned_msg.msg_ptr == nullptr || loaned_msg.impl == nullptr ||
      loaned_msg.release == nullptr) {
    return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;
  }
  const auto tracking_impl = loaned_msg.tracking_impl;
  const auto track_release = loaned_msg.track_release;
  const auto borrowed_timestamp_ns = loaned_msg.borrowed_timestamp_ns;
  const auto status = loaned_msg.release(loaned_msg.impl, loaned_msg.msg_ptr);
  if (status != AIMRT_CHANNEL_LOAN_STATUS_OK) return status;
  if (track_release != nullptr) {
    track_release(tracking_impl, borrowed_timestamp_ns);
  }
  loaned_msg = {};
  return AIMRT_CHANNEL_LOAN_STATUS_OK;
}

template <typename MsgType>
class LoanedMessage {
 public:
  LoanedMessage() = default;

  ~LoanedMessage() noexcept {
    if (!HasLoan()) return;
    const auto status = Reset();
    if (status != AIMRT_CHANNEL_LOAN_STATUS_OK) {
      std::fprintf(
          stderr,
          "AimRT loaned message destruction failed before ownership was returned: status=%d\n",
          static_cast<int>(status));
      std::fflush(stderr);
      std::terminate();
    }
  }

  LoanedMessage(const LoanedMessage&) = delete;
  LoanedMessage& operator=(const LoanedMessage&) = delete;

  LoanedMessage(LoanedMessage&& rhs) noexcept
      : status_(rhs.status_),
        route_id_(std::exchange(rhs.route_id_, nullptr)),
        loaned_msg_(std::exchange(rhs.loaned_msg_, {})) {}

  LoanedMessage& operator=(LoanedMessage&& rhs) noexcept {
    if (this == &rhs) return *this;
    if (HasLoan() && Reset() != AIMRT_CHANNEL_LOAN_STATUS_OK) {
      std::fprintf(
          stderr,
          "AimRT loaned message move assignment failed before ownership was returned: status=%d\n",
          static_cast<int>(status_));
      std::fflush(stderr);
      std::terminate();
    }
    status_ = rhs.status_;
    route_id_ = std::exchange(rhs.route_id_, nullptr);
    loaned_msg_ = std::exchange(rhs.loaned_msg_, {});
    return *this;
  }

  explicit operator bool() const noexcept {
    return HasLoan();
  }

  LoanStatus Status() const noexcept { return status_; }
  MsgType& operator*() const { return *static_cast<MsgType*>(loaned_msg_.msg_ptr); }
  MsgType* operator->() const { return static_cast<MsgType*>(loaned_msg_.msg_ptr); }

  LoanStatus Reset() noexcept {
    status_ = ReleaseLoanedMessage(loaned_msg_);
    if (status_ == AIMRT_CHANNEL_LOAN_STATUS_OK) route_id_ = nullptr;
    return status_;
  }

 private:
  explicit LoanedMessage(LoanStatus status) : status_(status) {}
  LoanedMessage(LoanStatus status,
                const void* route_id,
                aimrt_channel_loaned_message_base_t loaned_msg)
      : status_(status), route_id_(route_id), loaned_msg_(loaned_msg) {}

  const void* RouteId() const noexcept {
    return route_id_;
  }

  aimrt_channel_loaned_message_base_t* LoanNativeHandle() noexcept {
    return &loaned_msg_;
  }

  bool HasLoan() const noexcept {
    return loaned_msg_.msg_ptr != nullptr && loaned_msg_.impl != nullptr &&
           loaned_msg_.release != nullptr;
  }

  void MarkPublishResult(LoanStatus status) noexcept {
    status_ = status;
    if (!HasLoan()) route_id_ = nullptr;
  }

  template <typename>
  friend class LoanedPublisher;
  friend class PublisherProxyBase;

  LoanStatus status_ = AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;
  const void* route_id_ = nullptr;
  aimrt_channel_loaned_message_base_t loaned_msg_{};
};

template <typename ConstMsgType>
class LoanedMessageView {
 public:
  static_assert(std::is_const_v<ConstMsgType>, "LoanedMessageView must be read-only.");

  explicit LoanedMessageView(const void* msg_ptr)
      : msg_ptr_(static_cast<const std::remove_const_t<ConstMsgType>*>(msg_ptr)) {}

  LoanedMessageView(const LoanedMessageView&) = delete;
  LoanedMessageView& operator=(const LoanedMessageView&) = delete;
  LoanedMessageView(LoanedMessageView&&) = delete;
  LoanedMessageView& operator=(LoanedMessageView&&) = delete;

  const std::remove_const_t<ConstMsgType>& operator*() const { return *msg_ptr_; }
  const std::remove_const_t<ConstMsgType>* operator->() const { return msg_ptr_; }

 private:
  const std::remove_const_t<ConstMsgType>* msg_ptr_;
};

class PublisherRef {
 public:
  PublisherRef() = default;
  explicit PublisherRef(const aimrt_channel_publisher_base_t* base_ptr)
      : base_ptr_(base_ptr) {}
  ~PublisherRef() = default;

  explicit operator bool() const { return (base_ptr_ != nullptr); }

  const aimrt_channel_publisher_base_t* NativeHandle() const {
    return base_ptr_;
  }

  /**
   * @brief Register a type to be published
   *
   * @param msg_type_support
   * @return Register result
   */
  bool RegisterPublishType(const aimrt_type_support_base_t* msg_type_support) {
    AIMRT_ASSERT(base_ptr_, "Reference is null.");
    return base_ptr_->register_publish_type(base_ptr_->impl, msg_type_support);
  }

  /**
   * @brief Publish a msg
   *
   * @param msg_type
   * @param msg_ptr
   */
  void Publish(std::string_view msg_type, ContextRef ctx_ref, const void* msg_ptr) {
    AIMRT_ASSERT(base_ptr_, "Reference is null.");
    base_ptr_->publish(base_ptr_->impl, aimrt::util::ToAimRTStringView(msg_type), ctx_ref.NativeHandle(), msg_ptr);
  }

  LoanStatus PrepareLoanedPublisher(
      std::string_view msg_type,
      aimrt_channel_loaned_publisher_base_t* output) const {
    AIMRT_ASSERT(base_ptr_, "Reference is null.");
    AIMRT_ASSERT(base_ptr_->prepare_loaned_publisher,
                 "Loaned publish is not available in this runtime.");
    return base_ptr_->prepare_loaned_publisher(
        base_ptr_->impl, aimrt::util::ToAimRTStringView(msg_type), output);
  }

  /**
   * @brief Get the topic for current publisher
   *
   * @return Topic for current publisher
   */
  std::string_view GetTopic() const {
    AIMRT_ASSERT(base_ptr_, "Reference is null.");
    return aimrt::util::ToStdStringView(base_ptr_->get_topic(base_ptr_->impl));
  }

  void MergeSubscribeContextToPublishContext(
      const ContextRef subscribe_ctx_ref, ContextRef publish_ctx_ref) const {
    AIMRT_ASSERT(base_ptr_, "Reference is null.");
    base_ptr_->merge_subscribe_context_to_publish_context(
        base_ptr_->impl,
        subscribe_ctx_ref.NativeHandle(),
        publish_ctx_ref.NativeHandle());
  }

 private:
  const aimrt_channel_publisher_base_t* base_ptr_ = nullptr;
};

class LoanedPublisherRef {
 public:
  LoanedPublisherRef() = default;
  explicit LoanedPublisherRef(LoanStatus status) : status_(status) {}
  LoanedPublisherRef(
      LoanStatus status,
      aimrt_channel_loaned_publisher_base_t base)
      : status_(status), base_(base) {}

  explicit operator bool() const noexcept {
    return LoanSucceeded(status_) && base_.impl != nullptr;
  }

  LoanStatus Status() const noexcept { return status_; }
  const void* RouteId() const noexcept { return base_.impl; }

  LoanStatus Borrow(aimrt_channel_loaned_message_base_t* output) const {
    if (output == nullptr)
      return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;
    if (!LoanSucceeded(status_)) return status_;
    if (base_.impl == nullptr || base_.borrow_loaned_message == nullptr)
      return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;
    return base_.borrow_loaned_message(base_.impl, output);
  }

  LoanStatus Publish(
      ContextRef ctx_ref,
      aimrt_channel_loaned_message_base_t* loaned_msg) const {
    if (loaned_msg == nullptr)
      return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;
    if (!LoanSucceeded(status_)) return status_;
    if (base_.impl == nullptr || base_.publish_loaned_message == nullptr)
      return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;
    return base_.publish_loaned_message(
        base_.impl, ctx_ref.NativeHandle(), loaned_msg);
  }

 private:
  LoanStatus status_ = AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;
  aimrt_channel_loaned_publisher_base_t base_{};
};

template <typename MsgType>
class LoanedPublisher {
 public:
  LoanedPublisher() = default;
  explicit LoanedPublisher(LoanedPublisherRef route) : route_(route) {}

  explicit operator bool() const noexcept { return static_cast<bool>(route_); }
  LoanStatus Status() const noexcept { return route_.Status(); }

  LoanedMessage<MsgType> BorrowLoanedMessage() const {
    aimrt_channel_loaned_message_base_t loaned_msg{};
    const auto status = route_.Borrow(&loaned_msg);
    return LoanedMessage<MsgType>(status, route_.RouteId(), loaned_msg);
  }

  LoanStatus Publish(
      ContextRef ctx_ref,
      LoanedMessage<MsgType>&& loaned_msg) const {
    if (loaned_msg.RouteId() != route_.RouteId()) {
      loaned_msg.MarkPublishResult(AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT);
      return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;
    }
    const auto status = route_.Publish(ctx_ref, loaned_msg.LoanNativeHandle());
    loaned_msg.MarkPublishResult(status);
    return status;
  }

  LoanStatus Publish(LoanedMessage<MsgType>&& loaned_msg) const {
    Context ctx;
    return Publish(ctx, std::move(loaned_msg));
  }

 private:
  LoanedPublisherRef route_;
};

class SubscriberRef {
 public:
  SubscriberRef() = default;
  explicit SubscriberRef(const aimrt_channel_subscriber_base_t* base_ptr)
      : base_ptr_(base_ptr) {}
  ~SubscriberRef() = default;

  explicit operator bool() const { return (base_ptr_ != nullptr); }

  const aimrt_channel_subscriber_base_t* NativeHandle() const {
    return base_ptr_;
  }

  /**
   * @brief Subscribe to a certain type
   *
   * @param msg_type_support
   * @param callback
   * @return Subscribe result
   */
  bool Subscribe(
      const aimrt_type_support_base_t* msg_type_support,
      SubscriberCallback&& callback) {
    AIMRT_ASSERT(base_ptr_, "Reference is null.");
    return base_ptr_->subscribe(base_ptr_->impl, msg_type_support, callback.NativeHandle());
  }

  LoanStatus SubscribeLoaned(
      const aimrt_type_support_base_t* msg_type_support,
      SubscriberLoanedCallback&& callback) const {
    AIMRT_ASSERT(base_ptr_, "Reference is null.");
    AIMRT_ASSERT(base_ptr_->subscribe_loaned, "Loaned subscribe is not available in this runtime.");
    return base_ptr_->subscribe_loaned(
        base_ptr_->impl, msg_type_support, callback.NativeHandle());
  }

  /**
   * @brief Get the topic for current subscriber
   *
   * @return Topic for current subscriber
   */
  std::string_view GetTopic() const {
    AIMRT_ASSERT(base_ptr_, "Reference is null.");
    return aimrt::util::ToStdStringView(base_ptr_->get_topic(base_ptr_->impl));
  }

 private:
  const aimrt_channel_subscriber_base_t* base_ptr_ = nullptr;
};

class ChannelHandleRef {
 public:
  ChannelHandleRef() = default;
  explicit ChannelHandleRef(const aimrt_channel_handle_base_t* base_ptr)
      : base_ptr_(base_ptr) {}
  ~ChannelHandleRef() = default;

  explicit operator bool() const { return (base_ptr_ != nullptr); }

  const aimrt_channel_handle_base_t* NativeHandle() const { return base_ptr_; }

  PublisherRef GetPublisher(std::string_view topic) {
    AIMRT_ASSERT(base_ptr_, "Reference is null.");
    return PublisherRef(
        base_ptr_->get_publisher(base_ptr_->impl, aimrt::util::ToAimRTStringView(topic)));
  }

  SubscriberRef GetSubscriber(std::string_view topic) {
    AIMRT_ASSERT(base_ptr_, "Reference is null.");
    return SubscriberRef(
        base_ptr_->get_subscriber(base_ptr_->impl, aimrt::util::ToAimRTStringView(topic)));
  }

  void MergeSubscribeContextToPublishContext(
      const ContextRef subscribe_ctx_ref, ContextRef publish_ctx_ref) const {
    AIMRT_ASSERT(base_ptr_, "Reference is null.");
    base_ptr_->merge_subscribe_context_to_publish_context(
        base_ptr_->impl,
        subscribe_ctx_ref.NativeHandle(),
        publish_ctx_ref.NativeHandle());
  }

 private:
  const aimrt_channel_handle_base_t* base_ptr_ = nullptr;
};

class PublisherProxyBase {
 public:
  explicit PublisherProxyBase(PublisherRef publisher, std::string_view msg_type_name)
      : publisher_(publisher), msg_type_name_(msg_type_name) {}
  virtual ~PublisherProxyBase() = default;

  PublisherProxyBase(const PublisherProxyBase&) = delete;
  PublisherProxyBase& operator=(const PublisherProxyBase&) = delete;

  std::shared_ptr<Context> NewContextSharedPtr(ContextRef ctx_ref = ContextRef()) const {
    auto result_ctx = default_ctx_ptr_
                          ? std::make_shared<Context>(*default_ctx_ptr_)
                          : std::make_shared<Context>();
    if (ctx_ref) {
      publisher_.MergeSubscribeContextToPublishContext(ctx_ref, result_ctx);
    }

    return result_ctx;
  }

  void SetDefaultContextSharedPtr(const std::shared_ptr<Context>& ctx_ptr) {
    default_ctx_ptr_ = ctx_ptr;
  }

  std::shared_ptr<Context> GetDefaultContextSharedPtr() const {
    return default_ctx_ptr_;
  }

  std::string_view GetTopic() const {
    return publisher_.GetTopic();
  }

 protected:
  void PublishImpl(ContextRef ctx_ref, const void* msg_ptr) {
    publisher_.Publish(msg_type_name_, ctx_ref, msg_ptr);
  }

  template <typename MsgType>
  LoanedMessage<MsgType> BorrowLoanedMessageImpl() const {
    const auto loaned_publisher = GetLoanedPublisher();
    aimrt_channel_loaned_message_base_t loaned_msg{};
    const auto status = loaned_publisher.Borrow(&loaned_msg);
    return LoanedMessage<MsgType>(
        status, loaned_publisher.RouteId(), loaned_msg);
  }

  template <typename MsgType>
  LoanStatus PublishLoanedMessageImpl(ContextRef ctx_ref, LoanedMessage<MsgType>&& loaned_msg) const {
    const auto loaned_publisher = GetLoanedPublisher();
    if (loaned_msg.RouteId() != loaned_publisher.RouteId()) {
      loaned_msg.MarkPublishResult(AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT);
      return AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT;
    }
    auto status = loaned_publisher.Publish(
        ctx_ref, loaned_msg.LoanNativeHandle());
    loaned_msg.MarkPublishResult(status);
    return status;
  }

 private:
  LoanedPublisherRef GetLoanedPublisher() const {
    if (loaned_publisher_prepared_.load(std::memory_order_acquire))
      return loaned_publisher_;

    std::lock_guard guard(loaned_publisher_prepare_mutex_);
    if (loaned_publisher_prepared_.load(std::memory_order_relaxed))
      return loaned_publisher_;

    aimrt_channel_loaned_publisher_base_t route{};
    const auto status = publisher_.PrepareLoanedPublisher(msg_type_name_, &route);
    LoanedPublisherRef result(status, route);
    if (status != AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE) {
      loaned_publisher_ = result;
      loaned_publisher_prepared_.store(true, std::memory_order_release);
    }
    return result;
  }

 protected:
  PublisherRef publisher_;
  const std::string msg_type_name_;
  std::shared_ptr<Context> default_ctx_ptr_;
  mutable LoanedPublisherRef loaned_publisher_;
  mutable std::mutex loaned_publisher_prepare_mutex_;
  mutable std::atomic_bool loaned_publisher_prepared_ = false;
};

template <typename>
class PublisherProxy;

class SubscriberProxyBase {
 public:
  explicit SubscriberProxyBase(SubscriberRef subscriber, std::string_view msg_type_name)
      : subscriber_(subscriber), msg_type_name_(msg_type_name) {}
  virtual ~SubscriberProxyBase() = default;

  SubscriberProxyBase(const SubscriberProxyBase&) = delete;
  SubscriberProxyBase& operator=(const SubscriberProxyBase&) = delete;

  std::string_view GetTopic() const {
    return subscriber_.GetTopic();
  }

 protected:
  SubscriberRef subscriber_;
  const std::string msg_type_name_;
};

template <typename>
class SubscriberProxy;

}  // namespace aimrt::channel

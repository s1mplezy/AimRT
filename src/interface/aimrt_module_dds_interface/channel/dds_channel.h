// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "aimrt_module_cpp_interface/channel/channel_handle.h"
#include "aimrt_module_cpp_interface/co/inline_scheduler.h"
#include "aimrt_module_cpp_interface/co/on.h"
#include "aimrt_module_cpp_interface/co/start_detached.h"
#include "aimrt_module_cpp_interface/co/task.h"
#include "aimrt_module_cpp_interface/co/then.h"
#include "aimrt_module_cpp_interface/context/details/type_support.h"
#include "aimrt_module_dds_interface/util/dds_type_support.h"

namespace aimrt::channel {

template <DdsMessageType MsgType>
const std::string& DdsAimrtTypeName() {
  static const std::string kName =
      "dds:" + std::string(DdsTypeSupportTraits<MsgType>::TypeName());
  return kName;
}

template <DdsMessageType MsgType>
inline bool RegisterPublishType(PublisherRef publisher) {
  return publisher.RegisterPublishType(GetDdsMessageTypeSupport<MsgType>());
}

template <DdsMessageType MsgType>
inline void Publish(PublisherRef publisher, ContextRef ctx_ref, const MsgType& msg) {
  if (ctx_ref) {
    if (ctx_ref.GetSerializationType().empty()) ctx_ref.SetSerializationType("dds_xcdr2");
    publisher.Publish(DdsAimrtTypeName<MsgType>(), ctx_ref, &msg);
    return;
  }
  Context context;
  context.SetSerializationType("dds_xcdr2");
  publisher.Publish(DdsAimrtTypeName<MsgType>(), context, &msg);
}

template <DdsMessageType MsgType>
inline void Publish(PublisherRef publisher, const MsgType& msg) {
  Publish(publisher, ContextRef(), msg);
}

template <DdsMessageType MsgType>
inline LoanedPublisher<MsgType> PrepareLoanedPublisher(PublisherRef publisher) {
  aimrt_channel_loaned_publisher_base_t route{};
  const auto status = publisher.PrepareLoanedPublisher(DdsAimrtTypeName<MsgType>(), &route);
  return LoanedPublisher<MsgType>(LoanedPublisherRef(status, route));
}

template <DdsMessageType MsgType>
struct MessagePublisherTraits<MsgType> {
  static void PublishMsg(PublisherRef publisher, ContextRef ctx_ref, const MsgType& msg) {
    Publish(publisher, ctx_ref, msg);
  }
};

template <DdsMessageType MsgType>
inline bool Subscribe(
    SubscriberRef subscriber,
    std::function<void(ContextRef, const std::shared_ptr<const MsgType>&)>&& callback) {
  return subscriber.Subscribe(
      GetDdsMessageTypeSupport<MsgType>(),
      [callback{std::move(callback)}](const aimrt_channel_context_base_t* context,
                                      const void* msg,
                                      aimrt_function_base_t* release_callback) {
        SubscriberReleaseCallback release(release_callback);
        callback(
            ContextRef(context),
            std::shared_ptr<const MsgType>(
                static_cast<const MsgType*>(msg),
                [release{std::move(release)}](const MsgType*) mutable { release(); }));
      });
}

template <DdsMessageType MsgType>
inline bool Subscribe(
    SubscriberRef subscriber,
    std::function<void(const std::shared_ptr<const MsgType>&)>&& callback) {
  return Subscribe<MsgType>(
      subscriber,
      [callback{std::move(callback)}](ContextRef, const std::shared_ptr<const MsgType>& msg) {
        callback(msg);
      });
}

template <DdsMessageType MsgType>
inline LoanStatus SubscribeLoaned(
    SubscriberRef subscriber,
    std::function<void(ContextRef, const LoanedMessageView<const MsgType>&)>&& callback) {
  return subscriber.SubscribeLoaned(
      GetDdsMessageTypeSupport<MsgType>(),
      [callback{std::move(callback)}](const aimrt_channel_context_base_t* context,
                                      const void* msg) noexcept {
        try {
          const LoanedMessageView<const MsgType> view(msg);
          callback(ContextRef(context), view);
        } catch (const std::exception& error) {
          details::ReportLoanedSubscriberCallbackException(error.what());
        } catch (...) {
          details::ReportLoanedSubscriberCallbackException(nullptr);
        }
      });
}

template <DdsMessageType MsgType>
inline bool SubscribeCo(
    SubscriberRef subscriber,
    std::function<co::Task<void>(ContextRef, const MsgType&)>&& callback) {
  return subscriber.Subscribe(
      GetDdsMessageTypeSupport<MsgType>(),
      [callback{std::move(callback)}](const aimrt_channel_context_base_t* context,
                                      const void* msg,
                                      aimrt_function_base_t* release_callback) {
        aimrt::co::StartDetached(
            aimrt::co::On(
                aimrt::co::InlineScheduler(),
                callback(ContextRef(context), *static_cast<const MsgType*>(msg))) |
            aimrt::co::Then(SubscriberReleaseCallback(release_callback)));
      });
}

template <DdsMessageType MsgType>
class PublisherProxy<MsgType> : public PublisherProxyBase {
 public:
  explicit PublisherProxy(PublisherRef publisher)
      : PublisherProxyBase(publisher, DdsAimrtTypeName<MsgType>()) {}

  static bool RegisterPublishType(PublisherRef publisher) {
    return publisher.RegisterPublishType(GetDdsMessageTypeSupport<MsgType>());
  }

  bool RegisterPublishType() {
    return publisher_.RegisterPublishType(GetDdsMessageTypeSupport<MsgType>());
  }

  void Publish(ContextRef context, const MsgType& msg) {
    if (context) {
      if (context.GetSerializationType().empty()) context.SetSerializationType("dds_xcdr2");
      PublishImpl(context, &msg);
      return;
    }
    auto context_ptr = NewContextSharedPtr();
    context_ptr->SetSerializationType("dds_xcdr2");
    PublishImpl(context_ptr, &msg);
  }

  void Publish(const MsgType& msg) {
    Publish(ContextRef(), msg);
  }

  LoanedMessage<MsgType> BorrowLoanedMessage() const {
    return BorrowLoanedMessageImpl<MsgType>();
  }

  LoanStatus Publish(ContextRef context, LoanedMessage<MsgType>&& msg) const {
    if (context) {
      if (context.GetSerializationType().empty()) context.SetSerializationType("dds_xcdr2");
      return PublishLoanedMessageImpl(context, std::move(msg));
    }
    Context local_context;
    local_context.SetSerializationType("dds_xcdr2");
    return PublishLoanedMessageImpl(local_context, std::move(msg));
  }

  LoanStatus Publish(LoanedMessage<MsgType>&& msg) const {
    return Publish(ContextRef(), std::move(msg));
  }
};

template <DdsMessageType MsgType>
class SubscriberProxy<MsgType> : public SubscriberProxyBase {
 public:
  explicit SubscriberProxy(SubscriberRef subscriber)
      : SubscriberProxyBase(subscriber, DdsAimrtTypeName<MsgType>()) {}

  bool Subscribe(
      std::function<void(ContextRef, const std::shared_ptr<const MsgType>&)>&& callback) {
    return aimrt::channel::Subscribe<MsgType>(subscriber_, std::move(callback));
  }

  bool Subscribe(
      std::function<void(const std::shared_ptr<const MsgType>&)>&& callback) {
    return aimrt::channel::Subscribe<MsgType>(subscriber_, std::move(callback));
  }

  bool SubscribeCo(
      std::function<co::Task<void>(ContextRef, const MsgType&)>&& callback) {
    return aimrt::channel::SubscribeCo<MsgType>(subscriber_, std::move(callback));
  }

  LoanStatus SubscribeLoaned(
      std::function<void(ContextRef, const LoanedMessageView<const MsgType>&)>&& callback) {
    return aimrt::channel::SubscribeLoaned<MsgType>(subscriber_, std::move(callback));
  }
};

}  // namespace aimrt::channel

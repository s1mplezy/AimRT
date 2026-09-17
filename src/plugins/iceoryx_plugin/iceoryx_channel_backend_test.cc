// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "iceoryx_plugin/iceoryx_channel_backend.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

#include "aimrt_module_cpp_interface/channel/channel_context.h"
#include "aimrt_module_cpp_interface/util/string.h"
#include "gtest/gtest.h"

#if defined(AIMRT_ICEORYX_TEST_WITH_DDS)
  #include "IceoryxLoanTypes.h"
  #include "aimrt_module_dds_interface/util/dds_type_support.h"
#endif

namespace aimrt::plugins::iceoryx_plugin {
namespace {

using namespace std::chrono_literals;

#if defined(AIMRT_ICEORYX_TEST_WITH_DDS)

using PlainMessage = aimrt_iceoryx_test::NativePlainMessage;

const aimrt_type_support_base_t* PlainMessageTypeSupport() {
  return aimrt::GetDdsMessageTypeSupport<PlainMessage>();
}

runtime::core::channel::TopicInfo MakeTopicInfo(std::string topic) {
  const aimrt::util::TypeSupportRef type_support(PlainMessageTypeSupport());
  return runtime::core::channel::TopicInfo{
      .msg_type = std::string(type_support.TypeName()),
      .topic_name = std::move(topic),
      .pkg_path = "iceoryx_channel_backend_test",
      .module_name = "iceoryx_channel_backend_test",
      .index = 1,
      .msg_type_support_ref = type_support};
}

template <typename Predicate>
bool WaitFor(Predicate&& predicate,
             std::chrono::milliseconds timeout = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
}

TEST(IceoryxDdsNativeTypeSupport, ExposesOnlyXcdr2PlainTypes) {
  const aimrt::util::TypeSupportRef plain_type_support(
      aimrt::GetDdsMessageTypeSupport<
          aimrt_iceoryx_test::NativePlainMessage>());
  const auto* native = plain_type_support.NativeLoanTypeSupportPtr();
  ASSERT_NE(native, nullptr);
  EXPECT_EQ(native->size, sizeof(aimrt_iceoryx_test::NativePlainMessage));
  EXPECT_EQ(native->alignment,
            alignof(aimrt_iceoryx_test::NativePlainMessage));

  void* storage = ::operator new(native->size,
                                 std::align_val_t(native->alignment));
  ASSERT_TRUE(native->construct(storage));
  auto* message =
      static_cast<aimrt_iceoryx_test::NativePlainMessage*>(storage);
  message->value(10);
  EXPECT_EQ(message->value(), 10);
  native->destroy(storage);
  ::operator delete(storage, std::align_val_t(native->alignment));

  const aimrt::util::TypeSupportRef non_plain_type_support(
      aimrt::GetDdsMessageTypeSupport<
          aimrt_iceoryx_test::NativeNonPlainMessage>());
  EXPECT_EQ(non_plain_type_support.NativeLoanTypeSupportPtr(), nullptr);
}

TEST(IceoryxDdsNativeTypeSupport, BackendRejectsNonPlainLoans) {
  const auto* type_support = aimrt::GetDdsMessageTypeSupport<
      aimrt_iceoryx_test::NativeNonPlainMessage>();
  const aimrt::util::TypeSupportRef type_support_ref(type_support);
  const runtime::core::channel::TopicInfo info{
      .msg_type = std::string(type_support_ref.TypeName()),
      .topic_name = "iceoryx/loan/non_plain",
      .pkg_path = "iceoryx_channel_backend_test",
      .module_name = "iceoryx_channel_backend_test",
      .index = 1,
      .msg_type_support_ref = type_support_ref};

  IceoryxManager manager;
  IceoryxChannelBackend backend(manager);
  backend.Initialize(YAML::Node{});
  const runtime::core::channel::LoanedSubscribeWrapper subscriber{
      .info = info,
      .callback = [](aimrt::channel::ContextRef, const void*) {}};
  EXPECT_EQ(backend.SubscribeLoaned(subscriber),
            AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE);

  backend.Start();
  const runtime::core::channel::PublishTypeWrapper publisher{.info = info};
  runtime::core::channel::BackendLoanedPublisher loaned_publisher;
  EXPECT_EQ(backend.PrepareLoanedPublisher(publisher, loaned_publisher),
            AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE);
  backend.Shutdown();
}

  #if defined(__linux__) && defined(AIMRT_ICEORYX_ROUDI_PATH)
class RouDiProcess {
 public:
  RouDiProcess() {
    child_ = fork();
    if (child_ == 0) {
      execl(AIMRT_ICEORYX_ROUDI_PATH, AIMRT_ICEORYX_ROUDI_PATH,
            "--log-level", "off", nullptr);
      _exit(127);
    }
    if (child_ > 0) std::this_thread::sleep_for(1s);
  }

  ~RouDiProcess() {
    if (child_ <= 0) return;
    int status = 0;
    if (waitpid(child_, &status, WNOHANG) == child_) return;
    kill(child_, SIGTERM);
    for (int attempt = 0; attempt < 100; ++attempt) {
      if (waitpid(child_, &status, WNOHANG) == child_) return;
      std::this_thread::sleep_for(10ms);
    }
    kill(child_, SIGKILL);
    waitpid(child_, &status, 0);
  }

  RouDiProcess(const RouDiProcess&) = delete;
  RouDiProcess& operator=(const RouDiProcess&) = delete;

  bool Started() const { return child_ > 0; }

 private:
  pid_t child_ = -1;
};

bool RunMixedPublishSubscribeScenario() {
  IceoryxManager manager;
  manager.Initialize(64, "aimrt_iox_loan_test");
  IceoryxChannelBackend backend(manager);
  backend.Initialize(YAML::Node{});

  const auto publish_info = MakeTopicInfo("iceoryx/loan/mixed");
  const auto ordinary_info = MakeTopicInfo("iceoryx/loan/mixed");
  const auto loaned_info = MakeTopicInfo("iceoryx/loan/mixed");
  runtime::core::channel::PublishTypeWrapper publisher{
      .info = publish_info};

  std::atomic_int ordinary_normal = 0;
  std::atomic_int ordinary_loaned = 0;
  std::atomic_int loaned_normal = 0;
  std::atomic_int loaned_loaned = 0;
  std::atomic_bool ordinary_backend = false;
  std::atomic_bool loaned_backend = false;
  std::atomic<const void*> ordinary_pointer = nullptr;
  std::atomic<const void*> loaned_pointer = nullptr;

  runtime::core::channel::SubscribeWrapper ordinary_subscriber{
      .info = ordinary_info,
      .callback = [&](runtime::core::channel::MsgWrapper& message,
                      std::function<void()>&&) {
        const auto value =
            static_cast<const PlainMessage*>(message.msg_ptr)->value();
        ordinary_backend =
            message.ctx_ref.GetMetaValue(AIMRT_CHANNEL_CONTEXT_KEY_BACKEND) ==
            "iceoryx";
        ordinary_pointer = message.msg_ptr;
        if (value == 11) ++ordinary_normal;
        if (value == 22) ++ordinary_loaned;
      }};
  runtime::core::channel::LoanedSubscribeWrapper loaned_subscriber{
      .info = loaned_info,
      .callback = [&](aimrt::channel::ContextRef context,
                      const void* message) {
        const auto value = static_cast<const PlainMessage*>(message)->value();
        loaned_backend =
            context.GetMetaValue(AIMRT_CHANNEL_CONTEXT_KEY_BACKEND) ==
            "iceoryx";
        loaned_pointer = message;
        if (value == 11) ++loaned_normal;
        if (value == 22) ++loaned_loaned;
      }};

  if (!backend.RegisterPublishType(publisher) ||
      !backend.Subscribe(ordinary_subscriber) ||
      backend.SubscribeLoaned(loaned_subscriber) !=
          AIMRT_CHANNEL_LOAN_STATUS_OK)
    return false;
  backend.Start();

  PlainMessage normal_message;
  normal_message.value(11);
  if (!WaitFor([&] {
        runtime::core::channel::MsgWrapper wrapper{
            .info = publish_info,
            .msg_ptr = &normal_message};
        backend.Publish(wrapper);
        return WaitFor(
            [&] { return ordinary_normal > 0 && loaned_normal > 0; }, 50ms);
      }))
    return false;

  runtime::core::channel::BackendLoanedPublisher loaned_publisher;
  if (backend.PrepareLoanedPublisher(publisher, loaned_publisher) !=
      AIMRT_CHANNEL_LOAN_STATUS_OK)
    return false;
  if (!WaitFor([&] {
        aimrt_channel_loaned_message_base_t loaned_message;
        if (loaned_publisher.borrow(loaned_publisher.impl, loaned_message) !=
            AIMRT_CHANNEL_LOAN_STATUS_OK)
          return false;
        static_cast<PlainMessage*>(loaned_message.msg_ptr)->value(22);
        aimrt::channel::Context context(AIMRT_CHANNEL_PUBLISHER_CONTEXT);
        if (loaned_publisher.publish(
                loaned_publisher.impl, context, loaned_message) !=
            AIMRT_CHANNEL_LOAN_STATUS_OK) {
          aimrt::channel::ReleaseLoanedMessage(loaned_message);
          return false;
        }
        return WaitFor(
            [&] { return ordinary_loaned > 0 && loaned_loaned > 0; }, 50ms);
      }))
    return false;

  std::vector<aimrt_channel_loaned_message_base_t> held_loans;
  bool exhausted = false;
  for (size_t attempt = 0; attempt < 32; ++attempt) {
    aimrt_channel_loaned_message_base_t held_loan;
    const auto status =
        loaned_publisher.borrow(loaned_publisher.impl, held_loan);
    if (status == AIMRT_CHANNEL_LOAN_STATUS_LOAN_UNAVAILABLE) {
      exhausted = true;
      break;
    }
    if (status != AIMRT_CHANNEL_LOAN_STATUS_OK) return false;
    held_loans.emplace_back(held_loan);
  }
  if (!exhausted || held_loans.empty()) return false;
  for (auto& held_loan : held_loans) {
    if (aimrt::channel::ReleaseLoanedMessage(held_loan) !=
        AIMRT_CHANNEL_LOAN_STATUS_OK)
      return false;
  }

  aimrt_channel_loaned_message_base_t recovered_loan;
  if (loaned_publisher.borrow(loaned_publisher.impl, recovered_loan) !=
          AIMRT_CHANNEL_LOAN_STATUS_OK ||
      aimrt::channel::ReleaseLoanedMessage(recovered_loan) !=
          AIMRT_CHANNEL_LOAN_STATUS_OK)
    return false;

  const bool success = ordinary_backend && loaned_backend &&
                       ordinary_pointer.load() != loaned_pointer.load();
  backend.Shutdown();
  return success;
}

TEST(IceoryxChannelLoan, MixesOrdinaryAndLoanedPublishSubscribe) {
  RouDiProcess roudi;
  ASSERT_TRUE(roudi.Started());

  const pid_t worker = fork();
  if (worker == 0)
    _exit(RunMixedPublishSubscribeScenario() ? EXIT_SUCCESS : EXIT_FAILURE);
  ASSERT_GT(worker, 0);

  int status = 0;
  ASSERT_EQ(waitpid(worker, &status, 0), worker);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);
}
  #endif

#endif  // AIMRT_ICEORYX_TEST_WITH_DDS

}  // namespace
}  // namespace aimrt::plugins::iceoryx_plugin

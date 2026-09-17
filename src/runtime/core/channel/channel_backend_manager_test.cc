// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "core/channel/channel_backend_manager.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <thread>

#include "aimrt_module_cpp_interface/channel/channel_handle.h"

namespace aimrt::runtime::core::channel {

class MockChannelBackend : public ChannelBackendBase {
 public:
  explicit MockChannelBackend(std::string name = "mock_backend")
      : name_(std::move(name)) {}

  std::string_view Name() const noexcept override { return name_; }

  void Initialize(YAML::Node options_node) noexcept override {
    is_initialized_ = true;
  }
  void Start() override { is_statrted_ = true; }

  void Shutdown() override { is_shutdowned_ = false; }

  bool RegisterPublishType(const PublishTypeWrapper& publish_type_wrapper) noexcept override {
    is_registered_publish_type_ = true;
    return true;
  }

  bool Subscribe(const SubscribeWrapper& subscribe_wrapper) noexcept override {
    is_subscribed = true;
    return true;
  }

  void Publish(MsgWrapper& msg_wrapper) noexcept override {
    is_published = true;
    ++ordinary_publish_count;
  }

  aimrt_channel_loan_status_t PrepareLoanedPublisher(
      const PublishTypeWrapper&,
      BackendLoanedPublisher& route) noexcept override {
    ++prepare_count;
    if (prepare_status != AIMRT_CHANNEL_LOAN_STATUS_OK) return prepare_status;
    route = BackendLoanedPublisher{
        .impl = this,
        .borrow = [](void* impl,
                     aimrt_channel_loaned_message_base_t& loaned_msg) noexcept {
          auto& backend = *static_cast<MockChannelBackend*>(impl);
          ++backend.borrow_count;
          if (backend.borrow_hook) backend.borrow_hook();
          if (backend.borrow_status != AIMRT_CHANNEL_LOAN_STATUS_OK)
            return backend.borrow_status;
          loaned_msg = aimrt_channel_loaned_message_base_t{
              .msg_ptr = &backend.loaned_value,
              .impl = &backend,
              .release = [](void* release_impl, void*) {
                auto& backend = *static_cast<MockChannelBackend*>(release_impl);
                ++backend.release_count;
                if (backend.release_hook) backend.release_hook();
                return backend.release_status;
              }};
          return AIMRT_CHANNEL_LOAN_STATUS_OK; },
        .publish = [](void* impl, aimrt::channel::ContextRef,
                      aimrt_channel_loaned_message_base_t& loaned_msg) noexcept {
          auto& backend = *static_cast<MockChannelBackend*>(impl);
          ++backend.loaned_publish_count;
          if (backend.publish_hook) backend.publish_hook();
          backend.observed_loaned_value =
              *static_cast<const int*>(loaned_msg.msg_ptr);
          const auto status = backend.publish_loaned_status;
          if (status == AIMRT_CHANNEL_LOAN_STATUS_OK) loaned_msg = {};
          return status; }};
    return AIMRT_CHANNEL_LOAN_STATUS_OK;
  }

  aimrt_channel_loan_status_t SubscribeLoaned(
      const LoanedSubscribeWrapper& subscribe_wrapper) noexcept override {
    ++loaned_subscribe_count;
    if (subscribe_loaned_status == AIMRT_CHANNEL_LOAN_STATUS_OK)
      loaned_subscribe_wrapper_ptr = &subscribe_wrapper;
    return subscribe_loaned_status;
  }

  void DeliverLoaned(const void* msg_ptr) {
    ASSERT_NE(loaned_subscribe_wrapper_ptr, nullptr);
    aimrt::channel::Context ctx(
        aimrt_channel_context_type_t::AIMRT_CHANNEL_SUBSCRIBER_CONTEXT);
    loaned_subscribe_wrapper_ptr->callback(ctx, msg_ptr);
  }

  std::string name_;

  bool is_initialized_ = false;
  bool is_statrted_ = false;
  bool is_shutdowned_ = false;

  bool is_registered_publish_type_ = false;
  bool is_subscribed = false;
  bool is_published = false;
  int ordinary_publish_count = 0;
  int prepare_count = 0;
  int borrow_count = 0;
  int loaned_publish_count = 0;
  int release_count = 0;
  int loaned_subscribe_count = 0;
  int loaned_value = 0;
  int observed_loaned_value = 0;
  aimrt_channel_loan_status_t borrow_status = AIMRT_CHANNEL_LOAN_STATUS_OK;
  aimrt_channel_loan_status_t prepare_status = AIMRT_CHANNEL_LOAN_STATUS_OK;
  aimrt_channel_loan_status_t publish_loaned_status = AIMRT_CHANNEL_LOAN_STATUS_OK;
  aimrt_channel_loan_status_t release_status = AIMRT_CHANNEL_LOAN_STATUS_OK;
  aimrt_channel_loan_status_t subscribe_loaned_status = AIMRT_CHANNEL_LOAN_STATUS_OK;
  std::function<void()> borrow_hook;
  std::function<void()> publish_hook;
  std::function<void()> release_hook;
  const LoanedSubscribeWrapper* loaned_subscribe_wrapper_ptr = nullptr;
};

aimrt_type_support_base_t MakeIntTypeSupport(const char* type_name) {
  static const aimrt_string_view_t kSerializationTypes[] = {
      {.str = "test", .len = 4}};
  return aimrt_type_support_base_t{
      .type_name = [](void* impl) {
        const auto* name = static_cast<const char*>(impl);
        return aimrt_string_view_t{.str = name, .len = strlen(name)}; },
      .create = [](void*) -> void* { return new int(); },
      .destroy = [](void*, void* msg) { delete static_cast<int*>(msg); },
      .copy = [](void*, const void* from, void* to) { *static_cast<int*>(to) = *static_cast<const int*>(from); },
      .move = [](void*, void* from, void* to) { *static_cast<int*>(to) = *static_cast<int*>(from); },
      .serialize = [](void*, aimrt_string_view_t, const void*,
                      const aimrt_buffer_array_allocator_t*, aimrt_buffer_array_t*) { return false; },
      .deserialize = [](void*, aimrt_string_view_t, aimrt_buffer_array_view_t, void*) { return false; },
      .serialization_types_supported_num = [](void*) { return size_t{1}; },
      .serialization_types_supported_list = [](void*) { return kSerializationTypes; },
      .custom_type_support_ptr = [](void*) -> const void* { return nullptr; },
      .impl = const_cast<char*>(type_name)};
}

aimrt_channel_loan_status_t PrepareRoute(
    ChannelBackendManager& manager,
    aimrt_channel_loaned_publisher_base_t& route,
    std::string_view topic = "topic",
    std::string_view msg_type = "ros2:test_msgs/msg/Bounded") {
  return manager.PrepareLoanedPublisher(
      PrepareLoanedPublisherProxyInfoWrapper{
          .pkg_path = "pkg",
          .module_name = "module",
          .topic_name = topic,
          .msg_type = aimrt::util::ToAimRTStringView(msg_type),
          .output = &route});
}

aimrt_channel_loan_status_t BorrowFromRoute(
    const aimrt_channel_loaned_publisher_base_t& route,
    aimrt_channel_loaned_message_base_t& loaned_msg) {
  return route.borrow_loaned_message(route.impl, &loaned_msg);
}

aimrt_channel_loan_status_t PublishThroughRoute(
    const aimrt_channel_loaned_publisher_base_t& route,
    aimrt::channel::ContextRef ctx,
    aimrt_channel_loaned_message_base_t& loaned_msg) {
  return route.publish_loaned_message(
      route.impl, ctx.NativeHandle(), &loaned_msg);
}

class ChannelBackendManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    EXPECT_EQ(channel_backend_manager_.GetState(), ChannelBackendManager::State::kPreInit);
    channel_backend_manager_.RegisterChannelBackend(mock_backend_ptr_.get());

    channel_backend_manager_.SetPubTopicsBackendsRules({
        {"(.*)", {"mock_backend"}},
    });
    channel_backend_manager_.SetSubTopicsBackendsRules({
        {"(.*)", {"mock_backend"}},
    });

    channel_backend_manager_.SetChannelRegistry(channel_registry_test_ptr_.get());
    channel_backend_manager_.SetPublishFrameworkAsyncChannelFilterManager(
        &publish_filter_manager_);
    channel_backend_manager_.SetSubscribeFrameworkAsyncChannelFilterManager(
        &subscribe_filter_manager_);
    channel_backend_manager_.Initialize();
    EXPECT_EQ(channel_backend_manager_.GetState(), ChannelBackendManager::State::kInit);
  }

  // Test Shutdown
  void TearDown() override {
    channel_backend_manager_.Shutdown();
    EXPECT_EQ(channel_backend_manager_.GetState(), ChannelBackendManager::State::kShutdown);
  }

  std::unique_ptr<MockChannelBackend> mock_backend_ptr_ = std::make_unique<MockChannelBackend>();
  std::shared_ptr<ChannelRegistry> channel_registry_test_ptr_ = std::make_shared<ChannelRegistry>();

  ChannelBackendManager channel_backend_manager_;
  FrameworkAsyncChannelFilterManager publish_filter_manager_;
  FrameworkAsyncChannelFilterManager subscribe_filter_manager_;
};

// Test RegisterChannelBackend, Start
TEST_F(ChannelBackendManagerTest, RegisterChannelBackend_Start) {
  EXPECT_EQ(mock_backend_ptr_->is_statrted_, false);
  channel_backend_manager_.Start();
  EXPECT_EQ(mock_backend_ptr_->is_statrted_, true);
}

TEST_F(ChannelBackendManagerTest, LoanedPublishBorrowPublishAndAbandonLifecycle) {
  auto type_support = MakeIntTypeSupport("ros2:test_msgs/msg/Bounded");
  ASSERT_TRUE(channel_backend_manager_.RegisterPublishType(
      RegisterPublishTypeProxyInfoWrapper{
          .pkg_path = "pkg",
          .module_name = "module",
          .topic_name = "topic",
          .msg_type_support = &type_support}));
  channel_backend_manager_.Start();

  aimrt_channel_loaned_publisher_base_t route{};
  ASSERT_EQ(PrepareRoute(channel_backend_manager_, route),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  aimrt_channel_loaned_publisher_base_t second_route{};
  ASSERT_EQ(PrepareRoute(channel_backend_manager_, second_route),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_EQ(mock_backend_ptr_->prepare_count, 1);
  EXPECT_EQ(route.impl, second_route.impl);
  aimrt_channel_loaned_message_base_t abandoned{};
  EXPECT_EQ(BorrowFromRoute(route, abandoned),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_EQ(
      channel_backend_manager_.GetLoanedMessageDiagnostics().outstanding_loans,
      1u);
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  aimrt::channel::ReleaseLoanedMessage(abandoned);
  EXPECT_EQ(mock_backend_ptr_->release_count, 1);
  auto diagnostics = channel_backend_manager_.GetLoanedMessageDiagnostics();
  EXPECT_EQ(diagnostics.outstanding_loans, 0u);
  EXPECT_GT(diagnostics.max_hold_duration_ns, 0u);

  aimrt_channel_loaned_message_base_t loaned_msg{};
  ASSERT_EQ(BorrowFromRoute(route, loaned_msg),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  *static_cast<int*>(loaned_msg.msg_ptr) = 42;
  aimrt::channel::Context ctx;
  EXPECT_EQ(PublishThroughRoute(route, ctx, loaned_msg),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_EQ(mock_backend_ptr_->observed_loaned_value, 42);
  EXPECT_EQ(mock_backend_ptr_->loaned_publish_count, 1);
  EXPECT_EQ(mock_backend_ptr_->ordinary_publish_count, 0);
  EXPECT_EQ(loaned_msg.release, nullptr);
}

TEST_F(ChannelBackendManagerTest, ShutdownRejectsOutstandingPublisherLoan) {
  auto type_support = MakeIntTypeSupport("ros2:test_msgs/msg/Bounded");
  ASSERT_TRUE(channel_backend_manager_.RegisterPublishType(
      RegisterPublishTypeProxyInfoWrapper{
          .pkg_path = "pkg",
          .module_name = "module",
          .topic_name = "topic",
          .msg_type_support = &type_support}));
  channel_backend_manager_.Start();

  aimrt_channel_loaned_publisher_base_t route{};
  ASSERT_EQ(PrepareRoute(channel_backend_manager_, route),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  aimrt_channel_loaned_message_base_t loaned_msg{};
  ASSERT_EQ(BorrowFromRoute(route, loaned_msg),
            AIMRT_CHANNEL_LOAN_STATUS_OK);

  EXPECT_DEATH(
      channel_backend_manager_.Shutdown(),
      "have not been returned or published");

  EXPECT_EQ(channel_backend_manager_.GetState(),
            ChannelBackendManager::State::kStart);
  EXPECT_EQ(mock_backend_ptr_->release_count, 0);
  aimrt::channel::ReleaseLoanedMessage(loaned_msg);
  EXPECT_EQ(mock_backend_ptr_->release_count, 1);
}

TEST_F(ChannelBackendManagerTest, ProtobufLoanIsRejectedWithoutBackendCall) {
  auto type_support = MakeIntTypeSupport("pb:test.Message");
  ASSERT_TRUE(channel_backend_manager_.RegisterPublishType(
      RegisterPublishTypeProxyInfoWrapper{
          .pkg_path = "pkg",
          .module_name = "module",
          .topic_name = "topic",
          .msg_type_support = &type_support}));
  channel_backend_manager_.Start();

  aimrt_channel_loaned_publisher_base_t route{};
  EXPECT_EQ(PrepareRoute(
                channel_backend_manager_, route, "topic", "pb:test.Message"),
            AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE);
  EXPECT_EQ(mock_backend_ptr_->borrow_count, 0);
  EXPECT_EQ(mock_backend_ptr_->ordinary_publish_count, 0);
}

TEST_F(ChannelBackendManagerTest, UnsupportedAndUnavailableLoansDoNotFallback) {
  auto type_support = MakeIntTypeSupport("ros2:test_msgs/msg/Bounded");
  ASSERT_TRUE(channel_backend_manager_.RegisterPublishType(
      RegisterPublishTypeProxyInfoWrapper{
          .pkg_path = "pkg", .module_name = "module", .topic_name = "topic", .msg_type_support = &type_support}));
  channel_backend_manager_.Start();

  aimrt_channel_loaned_publisher_base_t route{};
  ASSERT_EQ(PrepareRoute(channel_backend_manager_, route),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  aimrt_channel_loaned_message_base_t loaned_msg{};
  mock_backend_ptr_->borrow_status = AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_BACKEND;
  EXPECT_EQ(BorrowFromRoute(route, loaned_msg),
            AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_BACKEND);
  mock_backend_ptr_->borrow_status = AIMRT_CHANNEL_LOAN_STATUS_LOAN_UNAVAILABLE;
  EXPECT_EQ(BorrowFromRoute(route, loaned_msg),
            AIMRT_CHANNEL_LOAN_STATUS_LOAN_UNAVAILABLE);
  EXPECT_EQ(
      channel_backend_manager_.GetLoanedMessageDiagnostics().borrow_failures,
      2u);
  EXPECT_EQ(mock_backend_ptr_->ordinary_publish_count, 0);
}

TEST_F(ChannelBackendManagerTest, LoanedPublishFailureRetainsLoanUntilExplicitRelease) {
  auto type_support = MakeIntTypeSupport("ros2:test_msgs/msg/Bounded");
  ASSERT_TRUE(channel_backend_manager_.RegisterPublishType(
      RegisterPublishTypeProxyInfoWrapper{
          .pkg_path = "pkg", .module_name = "module", .topic_name = "topic", .msg_type_support = &type_support}));
  channel_backend_manager_.Start();
  aimrt_channel_loaned_publisher_base_t route{};
  ASSERT_EQ(PrepareRoute(channel_backend_manager_, route),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  aimrt_channel_loaned_message_base_t loaned_msg{};
  ASSERT_EQ(BorrowFromRoute(route, loaned_msg),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  mock_backend_ptr_->publish_loaned_status = AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR;
  aimrt::channel::Context ctx;
  EXPECT_EQ(PublishThroughRoute(route, ctx, loaned_msg),
            AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR);
  EXPECT_EQ(mock_backend_ptr_->release_count, 0);
  EXPECT_NE(loaned_msg.release, nullptr);
  EXPECT_EQ(
      channel_backend_manager_.GetLoanedMessageDiagnostics().outstanding_loans,
      1u);
  EXPECT_EQ(aimrt::channel::ReleaseLoanedMessage(loaned_msg),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_EQ(mock_backend_ptr_->release_count, 1);
  EXPECT_EQ(
      channel_backend_manager_.GetLoanedMessageDiagnostics().outstanding_loans,
      0u);
  EXPECT_EQ(mock_backend_ptr_->ordinary_publish_count, 0);
}

TEST_F(ChannelBackendManagerTest, ShutdownWaitsForAdmittedBorrowRollback) {
  auto type_support = MakeIntTypeSupport("ros2:test_msgs/msg/Bounded");
  ASSERT_TRUE(channel_backend_manager_.RegisterPublishType(
      {.pkg_path = "pkg", .module_name = "module", .topic_name = "topic", .msg_type_support = &type_support}));
  channel_backend_manager_.Start();
  aimrt_channel_loaned_publisher_base_t route{};
  ASSERT_EQ(PrepareRoute(channel_backend_manager_, route),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  mock_backend_ptr_->borrow_status = AIMRT_CHANNEL_LOAN_STATUS_LOAN_UNAVAILABLE;
  std::promise<void> entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  mock_backend_ptr_->borrow_hook = [&] {
    entered.set_value();
    release_future.wait();
  };
  aimrt_channel_loaned_message_base_t loan{};
  auto borrowing = std::async(std::launch::async, [&] {
    return BorrowFromRoute(route, loan);
  });
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  auto stopping = std::async(
      std::launch::async, [&] { channel_backend_manager_.Shutdown(); });
  EXPECT_EQ(stopping.wait_for(std::chrono::milliseconds(50)),
            std::future_status::timeout);
  release.set_value();
  EXPECT_EQ(borrowing.get(), AIMRT_CHANNEL_LOAN_STATUS_LOAN_UNAVAILABLE);
  EXPECT_EQ(stopping.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
}

TEST_F(ChannelBackendManagerTest, ShutdownWaitsForTerminalPublishTracking) {
  auto type_support = MakeIntTypeSupport("ros2:test_msgs/msg/Bounded");
  ASSERT_TRUE(channel_backend_manager_.RegisterPublishType(
      {.pkg_path = "pkg", .module_name = "module", .topic_name = "topic", .msg_type_support = &type_support}));
  channel_backend_manager_.Start();
  aimrt_channel_loaned_publisher_base_t route{};
  ASSERT_EQ(PrepareRoute(channel_backend_manager_, route),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  aimrt_channel_loaned_message_base_t loan{};
  ASSERT_EQ(BorrowFromRoute(route, loan), AIMRT_CHANNEL_LOAN_STATUS_OK);
  std::promise<void> entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  mock_backend_ptr_->publish_hook = [&] {
    entered.set_value();
    release_future.wait();
  };
  auto publishing = std::async(std::launch::async, [&] {
    aimrt::channel::Context context;
    return PublishThroughRoute(route, context, loan);
  });
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  auto stopping = std::async(
      std::launch::async, [&] { channel_backend_manager_.Shutdown(); });
  EXPECT_EQ(stopping.wait_for(std::chrono::milliseconds(50)),
            std::future_status::timeout);
  release.set_value();
  EXPECT_EQ(publishing.get(), AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_EQ(stopping.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
}

TEST_F(ChannelBackendManagerTest, ShutdownWaitsForTerminalExplicitReleaseTracking) {
  auto type_support = MakeIntTypeSupport("ros2:test_msgs/msg/Bounded");
  ASSERT_TRUE(channel_backend_manager_.RegisterPublishType(
      {.pkg_path = "pkg", .module_name = "module", .topic_name = "topic", .msg_type_support = &type_support}));
  channel_backend_manager_.Start();
  aimrt_channel_loaned_publisher_base_t route{};
  ASSERT_EQ(PrepareRoute(channel_backend_manager_, route),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  aimrt_channel_loaned_message_base_t loan{};
  ASSERT_EQ(BorrowFromRoute(route, loan), AIMRT_CHANNEL_LOAN_STATUS_OK);
  std::promise<void> entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  mock_backend_ptr_->release_hook = [&] {
    entered.set_value();
    release_future.wait();
  };
  auto returning = std::async(std::launch::async, [&] {
    return aimrt::channel::ReleaseLoanedMessage(loan);
  });
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  auto stopping = std::async(
      std::launch::async, [&] { channel_backend_manager_.Shutdown(); });
  EXPECT_EQ(stopping.wait_for(std::chrono::milliseconds(50)),
            std::future_status::timeout);
  release.set_value();
  EXPECT_EQ(returning.get(), AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_EQ(stopping.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
}

TEST_F(ChannelBackendManagerTest, OrdinaryPublishBehaviorRemainsUnchanged) {
  auto type_support = MakeIntTypeSupport("ros2:test_msgs/msg/Bounded");
  ASSERT_TRUE(channel_backend_manager_.RegisterPublishType(
      RegisterPublishTypeProxyInfoWrapper{
          .pkg_path = "pkg", .module_name = "module", .topic_name = "topic", .msg_type_support = &type_support}));
  channel_backend_manager_.Start();
  int value = 11;
  aimrt::channel::Context ctx;
  channel_backend_manager_.Publish(PublishProxyInfoWrapper{
      .pkg_path = "pkg", .module_name = "module", .topic_name = "topic", .msg_type = {.str = "ros2:test_msgs/msg/Bounded", .len = 26}, .ctx_ptr = ctx.NativeHandle(), .msg_ptr = &value});
  EXPECT_EQ(mock_backend_ptr_->ordinary_publish_count, 1);
  EXPECT_EQ(mock_backend_ptr_->loaned_publish_count, 0);
}

TEST_F(ChannelBackendManagerTest, LoanedSubscribeDeliversOriginalScopedPointer) {
  auto type_support = MakeIntTypeSupport("ros2:test_msgs/msg/Bounded");
  const void* observed_ptr = nullptr;
  bool observed_outstanding_loan = false;
  aimrt::channel::SubscriberLoanedCallback callback(
      [this, &observed_ptr, &observed_outstanding_loan](
          const aimrt_channel_context_base_t*, const void* msg_ptr) {
        observed_ptr = msg_ptr;
        observed_outstanding_loan =
            channel_backend_manager_.GetLoanedMessageDiagnostics()
                .outstanding_loans == 1;
      });
  EXPECT_EQ(channel_backend_manager_.SubscribeLoaned(
                SubscribeLoanedProxyInfoWrapper{
                    .pkg_path = "pkg",
                    .module_name = "module",
                    .topic_name = "topic",
                    .msg_type_support = &type_support,
                    .callback = callback.NativeHandle()}),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  int value = 7;
  mock_backend_ptr_->DeliverLoaned(&value);
  EXPECT_EQ(observed_ptr, &value);
  EXPECT_TRUE(observed_outstanding_loan);
  EXPECT_EQ(
      channel_backend_manager_.GetLoanedMessageDiagnostics().outstanding_loans,
      0u);
  EXPECT_FALSE(mock_backend_ptr_->is_subscribed);
}

TEST_F(ChannelBackendManagerTest, ShutdownDestroysLoanedSubscriberCallback) {
  auto type_support = MakeIntTypeSupport("ros2:test_msgs/msg/Bounded");
  std::weak_ptr<int> callback_lifetime;
  {
    auto lifetime = std::make_shared<int>(0);
    callback_lifetime = lifetime;
    aimrt::channel::SubscriberLoanedCallback callback(
        [lifetime](const aimrt_channel_context_base_t*, const void*) {});
    ASSERT_EQ(channel_backend_manager_.SubscribeLoaned(
                  SubscribeLoanedProxyInfoWrapper{
                      .pkg_path = "pkg",
                      .module_name = "module",
                      .topic_name = "topic",
                      .msg_type_support = &type_support,
                      .callback = callback.NativeHandle()}),
              AIMRT_CHANNEL_LOAN_STATUS_OK);
  }

  EXPECT_FALSE(callback_lifetime.expired());
  channel_backend_manager_.Shutdown();
  EXPECT_TRUE(callback_lifetime.expired());
}

TEST_F(ChannelBackendManagerTest, ProtobufLoanedSubscribeIsRejectedWithoutBackendCall) {
  auto type_support = MakeIntTypeSupport("pb:test.Message");
  aimrt::channel::SubscriberLoanedCallback callback(
      [](const aimrt_channel_context_base_t*, const void*) {});
  EXPECT_EQ(channel_backend_manager_.SubscribeLoaned(
                SubscribeLoanedProxyInfoWrapper{
                    .pkg_path = "pkg", .module_name = "module", .topic_name = "topic", .msg_type_support = &type_support, .callback = callback.NativeHandle()}),
            AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE);
  EXPECT_EQ(mock_backend_ptr_->loaned_subscribe_count, 0);
  EXPECT_FALSE(mock_backend_ptr_->is_subscribed);
}

TEST(ChannelBackendManagerLoanRouteTest, MultipleBackendsAreRejectedBeforeBorrow) {
  ChannelRegistry registry;
  FrameworkAsyncChannelFilterManager publish_filters;
  FrameworkAsyncChannelFilterManager subscribe_filters;
  ChannelBackendManager manager;
  MockChannelBackend first("first");
  MockChannelBackend second("second");
  manager.RegisterChannelBackend(&first);
  manager.RegisterChannelBackend(&second);
  manager.SetPubTopicsBackendsRules({{"(.*)", {"first", "second"}}});
  manager.SetSubTopicsBackendsRules({{"(.*)", {"first", "second"}}});
  manager.SetChannelRegistry(&registry);
  manager.SetPublishFrameworkAsyncChannelFilterManager(&publish_filters);
  manager.SetSubscribeFrameworkAsyncChannelFilterManager(&subscribe_filters);
  manager.Initialize();
  auto type_support = MakeIntTypeSupport("ros2:test_msgs/msg/Bounded");
  ASSERT_TRUE(manager.RegisterPublishType(RegisterPublishTypeProxyInfoWrapper{
      .pkg_path = "pkg", .module_name = "module", .topic_name = "topic", .msg_type_support = &type_support}));
  manager.Start();
  aimrt_channel_loaned_publisher_base_t route{};
  EXPECT_EQ(PrepareRoute(manager, route),
            AIMRT_CHANNEL_LOAN_STATUS_INVALID_BACKEND_COUNT);
  EXPECT_EQ(first.prepare_count, 0);
  EXPECT_EQ(second.prepare_count, 0);
  manager.Shutdown();
}

TEST(ChannelBackendManagerLoanRouteTest, ZeroBackendsAreRejectedBeforeBorrow) {
  ChannelRegistry registry;
  FrameworkAsyncChannelFilterManager publish_filters;
  FrameworkAsyncChannelFilterManager subscribe_filters;
  ChannelBackendManager manager;
  manager.SetPubTopicsBackendsRules({{"(.*)", {}}});
  manager.SetSubTopicsBackendsRules({{"(.*)", {}}});
  manager.SetChannelRegistry(&registry);
  manager.SetPublishFrameworkAsyncChannelFilterManager(&publish_filters);
  manager.SetSubscribeFrameworkAsyncChannelFilterManager(&subscribe_filters);
  manager.Initialize();
  auto type_support = MakeIntTypeSupport("ros2:test_msgs/msg/Bounded");
  ASSERT_TRUE(manager.RegisterPublishType(RegisterPublishTypeProxyInfoWrapper{
      .pkg_path = "pkg", .module_name = "module", .topic_name = "topic", .msg_type_support = &type_support}));
  manager.Start();
  aimrt_channel_loaned_publisher_base_t route{};
  EXPECT_EQ(PrepareRoute(manager, route),
            AIMRT_CHANNEL_LOAN_STATUS_INVALID_BACKEND_COUNT);
  manager.Shutdown();
}

TEST(ChannelBackendManagerLoanRouteTest, PublisherAndSubscriberRoutesAreIndependent) {
  ChannelRegistry registry;
  FrameworkAsyncChannelFilterManager publish_filters;
  FrameworkAsyncChannelFilterManager subscribe_filters;
  ChannelBackendManager manager;
  MockChannelBackend first("first");
  MockChannelBackend second("second");
  manager.RegisterChannelBackend(&first);
  manager.RegisterChannelBackend(&second);
  manager.SetPubTopicsBackendsRules({{"(.*)", {"first"}}});
  manager.SetSubTopicsBackendsRules({{"(.*)", {"first", "second"}}});
  manager.SetChannelRegistry(&registry);
  manager.SetPublishFrameworkAsyncChannelFilterManager(&publish_filters);
  manager.SetSubscribeFrameworkAsyncChannelFilterManager(&subscribe_filters);
  manager.Initialize();
  auto type_support = MakeIntTypeSupport("ros2:test_msgs/msg/Bounded");
  aimrt::channel::SubscriberLoanedCallback callback(
      [](const aimrt_channel_context_base_t*, const void*) {});
  EXPECT_EQ(manager.SubscribeLoaned(SubscribeLoanedProxyInfoWrapper{
                .pkg_path = "pkg", .module_name = "module", .topic_name = "topic", .msg_type_support = &type_support, .callback = callback.NativeHandle()}),
            AIMRT_CHANNEL_LOAN_STATUS_INVALID_BACKEND_COUNT);
  ASSERT_TRUE(manager.RegisterPublishType(RegisterPublishTypeProxyInfoWrapper{
      .pkg_path = "pkg", .module_name = "module", .topic_name = "topic", .msg_type_support = &type_support}));
  manager.Start();
  aimrt_channel_loaned_publisher_base_t route{};
  ASSERT_EQ(PrepareRoute(manager, route), AIMRT_CHANNEL_LOAN_STATUS_OK);
  aimrt_channel_loaned_message_base_t loaned_msg{};
  EXPECT_EQ(BorrowFromRoute(route, loaned_msg),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  aimrt::channel::ReleaseLoanedMessage(loaned_msg);
  manager.Shutdown();
}

TEST(ChannelBackendManagerLoanRouteTest, PublishFiltersAreRejectedAtPreparation) {
  ChannelRegistry registry;
  FrameworkAsyncChannelFilterManager publish_filters;
  FrameworkAsyncChannelFilterManager subscribe_filters;
  ChannelBackendManager manager;
  MockChannelBackend backend("backend");
  manager.RegisterChannelBackend(&backend);
  manager.SetPubTopicsBackendsRules({{"(.*)", {"backend"}}});
  manager.SetSubTopicsBackendsRules({{"(.*)", {"backend"}}});
  manager.SetChannelRegistry(&registry);
  manager.SetPublishFrameworkAsyncChannelFilterManager(&publish_filters);
  manager.SetSubscribeFrameworkAsyncChannelFilterManager(&subscribe_filters);
  publish_filters.RegisterFilter(
      "loan_filter",
      [](MsgWrapper& wrapper, FrameworkAsyncChannelHandle&& handle) {
        handle(wrapper);
      });
  manager.SetPublishFiltersRules({{"(.*)", {"loan_filter"}}});
  manager.Initialize();
  auto type_support = MakeIntTypeSupport("ros2:test_msgs/msg/Bounded");
  ASSERT_TRUE(manager.RegisterPublishType(RegisterPublishTypeProxyInfoWrapper{
      .pkg_path = "pkg", .module_name = "module", .topic_name = "topic", .msg_type_support = &type_support}));
  manager.Start();

  aimrt_channel_loaned_publisher_base_t route{};
  EXPECT_EQ(PrepareRoute(manager, route),
            AIMRT_CHANNEL_LOAN_STATUS_INCOMPATIBLE_BACKEND_CONFIG);
  EXPECT_EQ(backend.prepare_count, 0);
  manager.Shutdown();
}

TEST(ChannelBackendManagerLoanRouteTest, SubscribeFiltersAreRejectedBeforeBackendRegistration) {
  ChannelRegistry registry;
  FrameworkAsyncChannelFilterManager publish_filters;
  FrameworkAsyncChannelFilterManager subscribe_filters;
  ChannelBackendManager manager;
  MockChannelBackend backend("backend");
  manager.RegisterChannelBackend(&backend);
  manager.SetPubTopicsBackendsRules({{"(.*)", {"backend"}}});
  manager.SetSubTopicsBackendsRules({{"(.*)", {"backend"}}});
  manager.SetChannelRegistry(&registry);
  manager.SetPublishFrameworkAsyncChannelFilterManager(&publish_filters);
  manager.SetSubscribeFrameworkAsyncChannelFilterManager(&subscribe_filters);
  subscribe_filters.RegisterFilter(
      "loan_filter",
      [](MsgWrapper& wrapper, FrameworkAsyncChannelHandle&& handle) {
        handle(wrapper);
      });
  manager.SetSubscribeFiltersRules({{"(.*)", {"loan_filter"}}});
  manager.Initialize();

  auto type_support = MakeIntTypeSupport("ros2:test_msgs/msg/Bounded");
  aimrt::channel::SubscriberLoanedCallback callback(
      [](const aimrt_channel_context_base_t*, const void*) {});
  EXPECT_EQ(manager.SubscribeLoaned(SubscribeLoanedProxyInfoWrapper{
                .pkg_path = "pkg",
                .module_name = "module",
                .topic_name = "topic",
                .msg_type_support = &type_support,
                .callback = callback.NativeHandle()}),
            AIMRT_CHANNEL_LOAN_STATUS_INCOMPATIBLE_BACKEND_CONFIG);
  EXPECT_EQ(backend.loaned_subscribe_count, 0);
  manager.Shutdown();
}

}  // namespace aimrt::runtime::core::channel

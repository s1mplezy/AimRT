// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "aimrt_module_cpp_interface/channel/channel_handle.h"

#include <gtest/gtest.h>

#include <cstddef>

namespace aimrt::channel {
namespace {

struct LoanState {
  int value = 0;
  int release_count = 0;
  LoanStatus release_status = AIMRT_CHANNEL_LOAN_STATUS_OK;
  LoanStatus publish_status = AIMRT_CHANNEL_LOAN_STATUS_OK;
  bool publish_terminal = true;
};

aimrt_channel_loaned_message_base_t MakeLoan(LoanState& state) {
  return aimrt_channel_loaned_message_base_t{
      .msg_ptr = &state.value,
      .impl = &state,
      .release = [](void* impl, void*) {
        auto& state = *static_cast<LoanState*>(impl);
        ++state.release_count;
        return state.release_status;
      },
      .owner = nullptr};
}

LoanedPublisher<int> MakeLoanedPublisher(LoanState& state) {
  return LoanedPublisher<int>(LoanedPublisherRef(
      AIMRT_CHANNEL_LOAN_STATUS_OK,
      aimrt_channel_loaned_publisher_base_t{
          .impl = &state,
          .borrow_loaned_message =
              [](void* impl, aimrt_channel_loaned_message_base_t* output) {
                *output = MakeLoan(*static_cast<LoanState*>(impl));
                return AIMRT_CHANNEL_LOAN_STATUS_OK;
              },
          .publish_loaned_message =
              [](void* impl, const aimrt_channel_context_base_t*,
                 aimrt_channel_loaned_message_base_t* loaned_msg) {
                auto& state = *static_cast<LoanState*>(impl);
                if (state.publish_terminal) *loaned_msg = {};
                return state.publish_status;
              }}));
}

static_assert(!std::is_copy_constructible_v<LoanedMessage<int>>);
static_assert(std::is_move_constructible_v<LoanedMessage<int>>);
static_assert(!std::is_constructible_v<
              LoanedMessage<int>,
              LoanStatus,
              const void*,
              aimrt_channel_loaned_message_base_t>);
static_assert(!std::is_copy_constructible_v<LoanedMessageView<const int>>);
static_assert(!std::is_move_constructible_v<LoanedMessageView<const int>>);
static_assert(offsetof(aimrt_channel_publisher_base_t, prepare_loaned_publisher) >
              offsetof(aimrt_channel_publisher_base_t, impl));
static_assert(offsetof(aimrt_channel_subscriber_base_t, subscribe_loaned) >
              offsetof(aimrt_channel_subscriber_base_t, impl));
static_assert(AIMRT_CHANNEL_LOAN_STATUS_OK == 0);
static_assert(AIMRT_CHANNEL_LOAN_STATUS_INVALID_BACKEND_COUNT == 1);
static_assert(AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_BACKEND == 2);
static_assert(AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE == 3);
static_assert(AIMRT_CHANNEL_LOAN_STATUS_INCOMPATIBLE_BACKEND_CONFIG == 4);
static_assert(AIMRT_CHANNEL_LOAN_STATUS_RUNTIME_CANNOT_LOAN == 5);
static_assert(AIMRT_CHANNEL_LOAN_STATUS_LOAN_UNAVAILABLE == 6);
static_assert(AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR == 7);
static_assert(AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT == 8);
static_assert(AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE == 9);
static_assert(AIMRT_CHANNEL_LOAN_STATUS_UNREGISTERED_MESSAGE_TYPE == 10);

TEST(LoanedMessageTest, AbandonedLoanIsReleasedExactlyOnce) {
  LoanState state;
  {
    auto loaned_msg = MakeLoanedPublisher(state).BorrowLoanedMessage();
    ASSERT_TRUE(loaned_msg);
    *loaned_msg = 42;
    EXPECT_EQ(state.value, 42);
  }
  EXPECT_EQ(state.release_count, 1);
}

TEST(LoanedMessageTest, MoveTransfersUniqueOwnership) {
  LoanState state;
  {
    auto source = MakeLoanedPublisher(state).BorrowLoanedMessage();
    LoanedMessage<int> destination(std::move(source));
    EXPECT_FALSE(source);
    ASSERT_TRUE(destination);
    *destination = 7;
  }
  EXPECT_EQ(state.value, 7);
  EXPECT_EQ(state.release_count, 1);
}

TEST(LoanedMessageTest, PublishingConsumesWithoutReleasingAgain) {
  LoanState state;
  {
    auto publisher = MakeLoanedPublisher(state);
    auto loaned_msg = publisher.BorrowLoanedMessage();
    *loaned_msg = 9;
    EXPECT_EQ(publisher.Publish(std::move(loaned_msg)),
              AIMRT_CHANNEL_LOAN_STATUS_OK);
  }
  EXPECT_EQ(state.release_count, 0);
}

TEST(LoanedMessageTest, FailedPreparedRoutePreservesStatus) {
  LoanedPublisherRef route(AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE);
  aimrt_channel_loaned_message_base_t loaned_msg{};
  EXPECT_EQ(
      route.Borrow(&loaned_msg),
      AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE);
}

TEST(LoanedMessageTest, FailedResetRetainsOwnershipForRetry) {
  LoanState state{.release_status = AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR};
  auto loaned_msg = MakeLoanedPublisher(state).BorrowLoanedMessage();
  ASSERT_TRUE(loaned_msg);
  EXPECT_EQ(loaned_msg.Reset(), AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR);
  EXPECT_TRUE(loaned_msg);
  EXPECT_EQ(loaned_msg.Status(), AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR);
  EXPECT_EQ(state.release_count, 1);

  state.release_status = AIMRT_CHANNEL_LOAN_STATUS_OK;
  EXPECT_EQ(loaned_msg.Reset(), AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_FALSE(loaned_msg);
  EXPECT_EQ(state.release_count, 2);
}

TEST(LoanedMessageTest, FailedPublishRetainsOwnershipForRetry) {
  LoanState state{.publish_status = AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR,
                  .publish_terminal = false};
  auto publisher = MakeLoanedPublisher(state);
  auto loaned_msg = publisher.BorrowLoanedMessage();
  ASSERT_TRUE(loaned_msg);
  EXPECT_EQ(publisher.Publish(std::move(loaned_msg)),
            AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR);
  EXPECT_TRUE(loaned_msg);
  EXPECT_EQ(loaned_msg.Status(), AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR);

  state.publish_status = AIMRT_CHANNEL_LOAN_STATUS_OK;
  state.publish_terminal = true;
  EXPECT_EQ(publisher.Publish(std::move(loaned_msg)),
            AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_FALSE(loaned_msg);
  EXPECT_EQ(state.release_count, 0);
}

TEST(LoanedMessageTest, TerminalPublishFailureClearsOwnership) {
  LoanState state{.publish_status = AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR,
                  .publish_terminal = true};
  auto publisher = MakeLoanedPublisher(state);
  auto loaned_msg = publisher.BorrowLoanedMessage();
  EXPECT_EQ(publisher.Publish(std::move(loaned_msg)),
            AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR);
  EXPECT_FALSE(loaned_msg);
  EXPECT_EQ(loaned_msg.Status(), AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR);
}

TEST(LoanedMessageTest, WrongPublisherPreservesOriginalLoan) {
  LoanState first;
  LoanState second;
  auto first_publisher = MakeLoanedPublisher(first);
  auto second_publisher = MakeLoanedPublisher(second);
  auto loaned_msg = first_publisher.BorrowLoanedMessage();
  ASSERT_TRUE(loaned_msg);
  EXPECT_EQ(second_publisher.Publish(std::move(loaned_msg)),
            AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT);
  EXPECT_TRUE(loaned_msg);
  EXPECT_EQ(loaned_msg.Reset(), AIMRT_CHANNEL_LOAN_STATUS_OK);
  EXPECT_EQ(first.release_count, 1);
  EXPECT_EQ(second.release_count, 0);
}

TEST(LoanedMessageTest, EmptyResetReturnsInvalidArgument) {
  LoanedMessage<int> loaned_msg;
  EXPECT_EQ(loaned_msg.Reset(), AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT);
  EXPECT_FALSE(loaned_msg);
}

TEST(LoanedMessageTest, DestructorFailsFastWhenReleaseRemainsNonterminal) {
  EXPECT_DEATH(
      {
        LoanState state{.release_status = AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR};
        auto loaned_msg = MakeLoanedPublisher(state).BorrowLoanedMessage();
        static_cast<void>(loaned_msg);
      },
      "destruction failed before ownership was returned");
}

TEST(LoanedMessageTest, MoveAssignmentFailsFastWhenDestinationCannotRelease) {
  EXPECT_DEATH(
      {
        LoanState destination_state{
            .release_status = AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR};
        LoanState source_state;
        auto destination =
            MakeLoanedPublisher(destination_state).BorrowLoanedMessage();
        auto source = MakeLoanedPublisher(source_state).BorrowLoanedMessage();
        destination = std::move(source);
      },
      "move assignment failed before ownership was returned");
}

}  // namespace
}  // namespace aimrt::channel

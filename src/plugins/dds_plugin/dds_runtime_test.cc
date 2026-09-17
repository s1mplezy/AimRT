// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "dds_plugin/dds_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>
#include <unistd.h>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>

#include "Calculator.h"
#include "aimrt_module_dds_interface/channel/dds_channel.h"
#include "dds_plugin/dds_test_process.h"

namespace aimrt::plugins::dds_plugin {
namespace {

using namespace std::chrono_literals;
using namespace eprosima::fastdds::dds;

uint32_t TestDomain(uint32_t offset) {
  return 100U + (static_cast<uint32_t>(getpid()) + offset) % 100U;
}

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
}

TypeSupport RequestType() {
  return aimrt::GetStaticDdsTypeSupportHandle<example::AddRequest>();
}

TypeSupport ResponseType() {
  return aimrt::GetStaticDdsTypeSupportHandle<example::AddResponse>();
}

template <typename Operation>
std::string CaptureInvalidArgument(Operation&& operation) {
  try {
    operation();
  } catch (const std::invalid_argument& error) {
    return error.what();
  } catch (const std::exception& error) {
    ADD_FAILURE() << "Expected std::invalid_argument, got: " << error.what();
    return {};
  }
  ADD_FAILURE() << "Expected std::invalid_argument";
  return {};
}

class ExternalWriter {
 public:
  ExternalWriter(uint32_t domain, std::string topic_name, const TypeSupport& type,
                 const DataWriterQos& qos, DataWriterListener* listener = nullptr)
      : topic_name_(std::move(topic_name)), type_(type) {
    participant_ = DomainParticipantFactory::get_instance()->create_participant(domain, PARTICIPANT_QOS_DEFAULT);
    if (!participant_) return;
    if (type_.register_type(participant_) != RETCODE_OK) return;
    topic_ = participant_->create_topic(topic_name_, type_->get_name(), TOPIC_QOS_DEFAULT);
    if (!topic_) return;
    publisher_ = participant_->create_publisher(PUBLISHER_QOS_DEFAULT);
    if (!publisher_) return;
    writer_ = publisher_->create_datawriter(topic_, qos, listener);
  }

  ~ExternalWriter() { Reset(); }
  DataWriter* Writer() const { return writer_; }

  void Reset() {
    if (!participant_) return;
    if (writer_) {
      EXPECT_EQ(publisher_->delete_datawriter(writer_), RETCODE_OK);
      writer_ = nullptr;
    }
    if (publisher_) {
      EXPECT_EQ(participant_->delete_publisher(publisher_), RETCODE_OK);
      publisher_ = nullptr;
    }
    if (topic_) {
      EXPECT_EQ(participant_->delete_topic(topic_), RETCODE_OK);
      topic_ = nullptr;
    }
    EXPECT_EQ(DomainParticipantFactory::get_instance()->delete_participant(participant_), RETCODE_OK);
    participant_ = nullptr;
  }

 private:
  std::string topic_name_;
  TypeSupport type_;
  DomainParticipant* participant_ = nullptr;
  Topic* topic_ = nullptr;
  Publisher* publisher_ = nullptr;
  DataWriter* writer_ = nullptr;
};

TEST(DdsConfigSharedQosConflict, ReusesEqualEndpointsAndRollsBackEveryConflict) {
  DdsParticipantContext context;
  context.Initialize(DdsPluginOptions{.domain_id = TestDomain(1), .participant_name = "dds_c06"});
  DdsRuntime runtime;
  runtime.Initialize(context);

  const auto request_type = RequestType();
  auto* first_writer = runtime.Endpoints().GetOrCreateWriter(
      "dds04/c06", request_type, context.Qos().channel_writer,
      "channel publisher registration module_alpha");
  EXPECT_EQ(runtime.Endpoints().GetOrCreateWriter(
                "dds04/c06", request_type, context.Qos().channel_writer,
                "channel publisher registration module_beta"),
            first_writer);
  EXPECT_EQ(runtime.Endpoints().WriterCount(), 1U);
  EXPECT_EQ(runtime.Endpoints().TopicCount(), 1U);

  auto conflicting_writer_qos = context.Qos().channel_writer;
  conflicting_writer_qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
  const auto writer_conflict = CaptureInvalidArgument([&] {
    runtime.Endpoints().GetOrCreateWriter(
        "dds04/c06", request_type, conflicting_writer_qos,
        "channel publisher registration module_gamma");
  });
  EXPECT_NE(writer_conflict.find("existing_origin='channel publisher registration module_alpha'"),
            std::string::npos);
  EXPECT_NE(writer_conflict.find("requested_origin='channel publisher registration module_gamma'"),
            std::string::npos);
  EXPECT_NE(writer_conflict.find("conflicting_policies=[reliability]"), std::string::npos);
  EXPECT_NE(writer_conflict.find("existing_effective_qos={reliability=best_effort"),
            std::string::npos);
  EXPECT_NE(writer_conflict.find("requested_effective_qos={reliability=reliable"),
            std::string::npos);
  EXPECT_EQ(runtime.Endpoints().WriterCount(), 1U);

  auto first_reader = runtime.Endpoints().GetOrCreateReader(
      "dds04/c06", request_type, context.Qos().channel_reader,
      "channel subscription registration module_alpha");
  EXPECT_EQ(runtime.Endpoints()
                .GetOrCreateReader("dds04/c06", request_type, context.Qos().channel_reader,
                                   "channel subscription registration module_beta")
                .reader,
            first_reader.reader);
  auto conflicting_reader_qos = context.Qos().channel_reader;
  conflicting_reader_qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
  const auto reader_conflict = CaptureInvalidArgument([&] {
    runtime.Endpoints().GetOrCreateReader(
        "dds04/c06", request_type, conflicting_reader_qos,
        "channel subscription registration module_gamma");
  });
  EXPECT_NE(
      reader_conflict.find("existing_origin='channel subscription registration module_alpha'"),
      std::string::npos);
  EXPECT_NE(
      reader_conflict.find("requested_origin='channel subscription registration module_gamma'"),
      std::string::npos);
  EXPECT_NE(reader_conflict.find("conflicting_policies=[reliability]"), std::string::npos);
  EXPECT_NE(reader_conflict.find("existing_effective_qos={reliability=best_effort"),
            std::string::npos);
  EXPECT_NE(reader_conflict.find("requested_effective_qos={reliability=reliable"),
            std::string::npos);
  EXPECT_EQ(runtime.Endpoints().ReaderCount(), 1U);

  EXPECT_THROW(runtime.Endpoints().GetOrCreateReader(
                   "dds04/c06", ResponseType(), context.Qos().channel_reader,
                   "channel subscription registration conflicting_type"),
               std::invalid_argument);
  EXPECT_EQ(runtime.Endpoints().TopicCount(), 1U);

  auto invalid_qos = context.Qos().channel_writer;
  invalid_qos.history().depth = 0;
  EXPECT_THROW(runtime.Endpoints().GetOrCreateWriter(
                   "dds04/rollback", request_type, invalid_qos,
                   "channel publisher registration invalid_qos"),
               std::runtime_error);
  EXPECT_EQ(runtime.Endpoints().TopicCount(), 1U);

  runtime.Shutdown();
  context.Shutdown();
}

TEST(DdsConfigTopicCollisions, RejectsConflictingOwnersWithoutOverwriting) {
  DdsNameRegistry names;
  names.RegisterChannel("dds04/c07", "Channel topic dds:example::AddRequest");
  EXPECT_THROW(names.RegisterChannel("dds04/c07", "Channel topic dds:example::AddResponse"),
               std::invalid_argument);
  EXPECT_THROW(names.RegisterChannel("req/dds/example::Calculator/Add"), std::invalid_argument);
  EXPECT_THROW(names.RegisterChannel("rsp/dds/example::Calculator/Add"), std::invalid_argument);
  const auto topics = names.RegisterRpc("dds:/example::Calculator/Add");
  EXPECT_EQ(topics.request, "req/dds/example::Calculator/Add");
  EXPECT_EQ(topics.response, "rsp/dds/example::Calculator/Add");
  EXPECT_NO_THROW(names.RegisterRpc("dds:/example::Calculator/Add"));
}

TEST(DdsExecutorAsioAuto, ResolvesTwoToFourAndUsesOneDrainTimerOwner) {
  const auto expected = ResolveDdsExecutorThreadNum(0, std::thread::hardware_concurrency());
  ASSERT_GE(expected, 2U);
  ASSERT_LE(expected, 4U);
  DdsParticipantContext context;
  context.Initialize(DdsPluginOptions{.domain_id = TestDomain(2),
                                      .participant_name = "dds_h07",
                                      .executor = {.type = "asio_thread", .thread_num = 0}});
  DdsRuntime runtime;
  runtime.Initialize(context);
  EXPECT_EQ(runtime.ThreadNum(), expected);
  runtime.Start();

  std::mutex mutex;
  std::condition_variable condition;
  std::vector<std::thread::id> owners;
  ASSERT_TRUE(runtime.Executor()->Post([&] {
    std::lock_guard lock(mutex);
    owners.emplace_back(std::this_thread::get_id());
    condition.notify_all();
  }));
  ASSERT_TRUE(runtime.Executor()->ScheduleAfter(1ms, [&] {
    std::lock_guard lock(mutex);
    owners.emplace_back(std::this_thread::get_id());
    condition.notify_all();
  }));
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, 5s, [&] { return owners.size() == 2; }));
  }
  EXPECT_NE(owners[0], std::this_thread::get_id());
  EXPECT_NE(owners[1], std::this_thread::get_id());

  runtime.Shutdown();
  context.Shutdown();
}

TEST(DdsExecutorExplicit, PreservesCountsSerializesReadersAndAllowsReaderParallelism) {
  for (const uint32_t thread_num : {1U, 2U, 4U}) {
    auto executor = std::make_shared<DdsAsioExecutor>(thread_num);
    executor->Start();
    EXPECT_EQ(executor->ThreadNum(), thread_num);

    std::atomic_int remaining = 32;
    std::atomic_int active = 0;
    std::atomic_int max_active = 0;
    auto serial = std::make_shared<DdsReaderDrainState>(
        executor,
        [&] {
          const int now_active = ++active;
          max_active.store(std::max(max_active.load(), now_active));
          std::this_thread::sleep_for(1ms);
          const bool consumed = remaining.fetch_sub(1) > 0;
          --active;
          return consumed ? DdsReaderDrainState::DrainResult::kConsumed
                          : DdsReaderDrainState::DrainResult::kNoData;
        },
        [&] { return remaining.load() > 0; });
    serial->Start();
    for (int index = 0; index < 64; ++index) serial->NotifyData();
    ASSERT_TRUE(WaitFor([&] { return remaining.load() <= 0 && !serial->DrainScheduled(); }));
    EXPECT_EQ(max_active.load(), 1);

    if (thread_num > 1) {
      std::mutex mutex;
      std::condition_variable condition;
      int entered = 0;
      bool release = false;
      const auto blocking_drain = [&] {
        std::unique_lock lock(mutex);
        ++entered;
        condition.notify_all();
        condition.wait(lock, [&] { return release; });
        return DdsReaderDrainState::DrainResult::kNoData;
      };
      auto first = std::make_shared<DdsReaderDrainState>(
          executor, blocking_drain, [] { return false; });
      auto second = std::make_shared<DdsReaderDrainState>(
          executor, blocking_drain, [] { return false; });
      first->Start();
      second->Start();
      first->NotifyData();
      second->NotifyData();
      {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(condition.wait_for(lock, 5s, [&] { return entered == 2; }));
        release = true;
      }
      condition.notify_all();
    }
    executor->Shutdown();
  }
}

TEST(DdsExecutorExplicit, PreservesTheLastWakeupRaceWithoutConcurrentDrain) {
  auto executor = std::make_shared<DdsAsioExecutor>(2);
  executor->Start();
  std::mutex mutex;
  std::condition_variable condition;
  bool recheck_entered = false;
  bool release_recheck = false;
  std::atomic_int unread_checks = 0;
  std::atomic_int samples = 0;
  std::atomic_int consumed = 0;
  std::atomic_int active = 0;
  std::atomic_int max_active = 0;
  auto state = std::make_shared<DdsReaderDrainState>(
      executor,
      [&] {
        const int now_active = ++active;
        max_active.store(std::max(max_active.load(), now_active));
        const bool has_sample = samples.fetch_sub(1) > 0;
        if (has_sample) ++consumed;
        --active;
        return has_sample ? DdsReaderDrainState::DrainResult::kConsumed
                          : DdsReaderDrainState::DrainResult::kNoData;
      },
      [&] {
        if (++unread_checks == 1) return false;  // Start-time check.
        std::unique_lock lock(mutex);
        if (!recheck_entered) {
          recheck_entered = true;
          condition.notify_all();
          condition.wait(lock, [&] { return release_recheck; });
        }
        return samples.load() > 0;
      });
  state->Start();
  state->NotifyData();
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, 5s, [&] { return recheck_entered; }));
    samples = 1;
    state->NotifyData();
    release_recheck = true;
  }
  condition.notify_all();
  ASSERT_TRUE(WaitFor([&] { return consumed.load() == 1 && !state->DrainScheduled(); }));
  EXPECT_EQ(max_active.load(), 1);
  executor->Shutdown();
}

TEST(DdsExecutorInvalidType, DefaultsToAsioAndRejectsEveryOtherType) {
  EXPECT_EQ(DecodeDdsPluginOptions(YAML::Load("{}")).executor.type, "asio_thread");
  EXPECT_EQ(DecodeDdsPluginOptions(YAML::Load("executor: {type: asio_thread}")).executor.type,
            "asio_thread");
  EXPECT_THROW(DecodeDdsPluginOptions(YAML::Load("executor: {type: simple_thread}")),
               std::invalid_argument);
  EXPECT_THROW(DecodeDdsPluginOptions(YAML::Load("executor: {type: tbb_thread}")),
               std::invalid_argument);
}

TEST(DdsListenerMatches, TracksRealMatchUnmatchAndRematch) {
  const auto domain = TestDomain(3);
  DdsParticipantContext context;
  context.Initialize(DdsPluginOptions{.domain_id = domain, .participant_name = "dds_d01"});
  DdsRuntime runtime;
  runtime.Initialize(context);
  runtime.Endpoints().GetOrCreateReader(
      "dds04/d01", RequestType(), context.Qos().channel_reader,
      "listener match diagnostic reader");
  runtime.Start();

  ExternalWriter first(domain, "dds04/d01", RequestType(), context.Qos().channel_writer);
  ASSERT_NE(first.Writer(), nullptr);
  ASSERT_TRUE(WaitFor([&] { return runtime.Diagnostics()->Snapshot().reader_current_matches == 1; }));
  first.Reset();
  ASSERT_TRUE(WaitFor([&] { return runtime.Diagnostics()->Snapshot().reader_current_matches == 0; }));
  ExternalWriter second(domain, "dds04/d01", RequestType(), context.Qos().channel_writer);
  ASSERT_NE(second.Writer(), nullptr);
  ASSERT_TRUE(WaitFor([&] {
    const auto snapshot = runtime.Diagnostics()->Snapshot();
    return snapshot.reader_current_matches == 1 && snapshot.reader_match_events >= 3;
  }));
  second.Reset();
  runtime.Shutdown();
  context.Shutdown();
}

TEST(DdsListenerIncompatibleQos, CountsRequestedAndOfferedPolicyDiagnostics) {
  const auto domain = TestDomain(4);
  DdsParticipantContext context;
  context.Initialize(DdsPluginOptions{.domain_id = domain, .participant_name = "dds_d02"});
  DdsRuntime runtime;
  runtime.Initialize(context);
  auto reliable_reader = context.Qos().channel_reader;
  reliable_reader.reliability().kind = RELIABLE_RELIABILITY_QOS;
  runtime.Endpoints().GetOrCreateReader("dds04/d02", RequestType(), reliable_reader,
                                        "incompatible QoS diagnostic reader");
  runtime.Start();

  auto writer_listener = std::make_unique<DdsDataWriterListener>(
      DdsEndpointMetadata{.participant = "external", .topic = "dds04/d02", .type = RequestType()->get_name()},
      runtime.Diagnostics());
  ExternalWriter writer(domain, "dds04/d02", RequestType(), context.Qos().channel_writer,
                        writer_listener.get());
  ASSERT_NE(writer.Writer(), nullptr);
  ASSERT_TRUE(WaitFor([&] {
    const auto snapshot = runtime.Diagnostics()->Snapshot();
    return snapshot.requested_incompatible_qos_total > 0 &&
           snapshot.offered_incompatible_qos_total > 0 &&
           snapshot.last_incompatible_qos_policy != 0;
  }));
  writer.Reset();
  writer_listener.reset();
  runtime.Shutdown();
  context.Shutdown();
}

TEST(DdsLifecycleCycles, StopsNewWorkCancelsTimersAndDrainsBeforeDeletion) {
  test::ProcessTestScope processes(50s);
  ASSERT_TRUE(processes.Ready());
  for (uint32_t cycle = 0; cycle < 3; ++cycle) {
    auto* child = processes.Launch(
        "--gtest_filter=DdsLifecycleProcessChild.RunsOneStartupShutdownCycle");
    ASSERT_TRUE(child);
    const auto result = processes.Finish(child, 10s);
    EXPECT_TRUE(result.process_group_established);
    ASSERT_TRUE(result.child_reaped);
    ASSERT_TRUE(WIFEXITED(result.status));
    EXPECT_EQ(WEXITSTATUS(result.status), 0);
    EXPECT_FALSE(result.term_sent);
    EXPECT_FALSE(result.kill_fallback_sent);
    EXPECT_TRUE(result.no_residual_process_group);
  }

  auto* stalled_child = processes.Launch(
      "--gtest_filter=DdsLifecycleProcessChild.StallsUntilParentDeadlineCleanup");
  ASSERT_TRUE(stalled_child);
  const auto stalled_result = processes.Finish(stalled_child, 200ms);
  EXPECT_TRUE(stalled_result.process_group_established);
  EXPECT_TRUE(stalled_result.term_sent);
  EXPECT_FALSE(stalled_result.kill_fallback_sent);
  ASSERT_TRUE(stalled_result.child_reaped);
  ASSERT_TRUE(WIFSIGNALED(stalled_result.status));
  EXPECT_EQ(WTERMSIG(stalled_result.status), SIGTERM);
  EXPECT_TRUE(stalled_result.no_residual_process_group);
  EXPECT_EQ(processes.ReceivedSignal(), 0);
  EXPECT_FALSE(processes.Expired());
}

TEST(DdsLifecycleProcessChild, RunsOneStartupShutdownCycle) {
  alarm(15);
  DdsParticipantContext context;
  context.Initialize(DdsPluginOptions{
      .domain_id = TestDomain(30), .participant_name = "dds_s01_child"});
  DdsRuntime runtime;
  runtime.Initialize(context);
  runtime.Endpoints().GetOrCreateWriter(
      "dds04/s01/process", RequestType(), context.Qos().channel_writer,
      "multi-process lifecycle writer");
  runtime.Start();

  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release_promise;
  auto release = release_promise.get_future().share();
  const bool posted = runtime.Executor()->Post([&] {
    entered.set_value();
    release.wait();
  });
  if (!posted || entered_future.wait_for(5s) != std::future_status::ready) {
    release_promise.set_value();
    runtime.Shutdown();
    context.Shutdown();
    FAIL() << "executor task did not enter before the child deadline";
  }

  std::atomic_bool timer_ran = false;
  if (!runtime.Executor()->ScheduleAfter(1h, [&] { timer_ran = true; })) {
    release_promise.set_value();
    runtime.Shutdown();
    context.Shutdown();
    FAIL() << "failed to schedule the lifecycle timer";
  }

  auto shutdown = std::async(std::launch::async, [&] { runtime.Shutdown(); });
  const bool stopping = WaitFor([&] {
    return runtime.State() == DdsRuntimeState::kStopping;
  });
  EXPECT_TRUE(stopping);
  EXPECT_FALSE(runtime.Executor()->Post([] {}));
  EXPECT_EQ(shutdown.wait_for(50ms), std::future_status::timeout);
  release_promise.set_value();
  ASSERT_EQ(shutdown.wait_for(5s), std::future_status::ready);
  shutdown.get();
  EXPECT_FALSE(timer_ran.load());
  EXPECT_EQ(runtime.State(), DdsRuntimeState::kStopped);
  EXPECT_EQ(runtime.Endpoints().WriterCount(), 0U);
  context.Shutdown();
}

TEST(DdsLifecycleProcessChild, StallsUntilParentDeadlineCleanup) {
  while (true) pause();
}

TEST(DdsListenerEntityDeletion, KeepsListenersUntilPublicDeleteReturnsOk) {
  DdsParticipantContext context;
  context.Initialize(DdsPluginOptions{.domain_id = TestDomain(20), .participant_name = "dds_s03"});
  DdsRuntime runtime;
  runtime.Initialize(context);
  auto tracker = std::make_shared<DdsListenerLifetimeTracker>();
  runtime.Endpoints().SetListenerLifetimeTracker(tracker);

  std::promise<void> drain_entered;
  std::promise<void> drain_release;
  auto release = drain_release.get_future().share();
  auto reader = runtime.Endpoints().GetOrCreateReader(
      "dds04/s03", RequestType(), context.Qos().channel_reader,
      "listener lifetime reader",
      [&] {
        drain_entered.set_value();
        release.wait();
        return DdsReaderDrainState::DrainResult::kNoData;
      },
      [] { return false; });
  runtime.Endpoints().GetOrCreateWriter(
      "dds04/s03", RequestType(), context.Qos().channel_writer,
      "listener lifetime writer");
  runtime.Start();
  reader.drain_state->NotifyData();
  drain_entered.get_future().wait();

  auto shutdown = std::async(std::launch::async, [&] { runtime.Shutdown(); });
  ASSERT_TRUE(WaitFor([&] { return runtime.State() == DdsRuntimeState::kStopping; }));
  {
    std::lock_guard lock(tracker->mutex);
    EXPECT_TRUE(std::ranges::none_of(tracker->events, [](const auto& event) {
      return event.starts_with("reader_delete_begin:") ||
             event.starts_with("reader_listener_destroyed:");
    }));
  }
  drain_release.set_value();
  ASSERT_EQ(shutdown.wait_for(5s), std::future_status::ready);
  shutdown.get();

  std::vector<std::string> events;
  {
    std::lock_guard lock(tracker->mutex);
    events = tracker->events;
  }
  const auto index_of = [&events](std::string_view prefix) {
    return std::distance(events.begin(), std::ranges::find_if(events, [prefix](const auto& event) {
                           return event.starts_with(prefix);
                         }));
  };
  EXPECT_LT(index_of("reader_delete_begin:"), index_of("reader_delete_ok:"));
  EXPECT_LT(index_of("reader_delete_ok:"), index_of("reader_listener_destroyed:"));
  EXPECT_LT(index_of("writer_delete_begin:"), index_of("writer_delete_ok:"));
  EXPECT_LT(index_of("writer_delete_ok:"), index_of("writer_listener_destroyed:"));
  EXPECT_LT(index_of("reader_listener_destroyed:"), index_of("topic_delete_begin:"));
  EXPECT_LT(index_of("writer_listener_destroyed:"), index_of("topic_delete_begin:"));
  EXPECT_LT(index_of("topic_delete_ok:"), index_of("publisher_delete_begin"));
  EXPECT_LT(index_of("publisher_delete_ok"), index_of("subscriber_delete_begin"));
  EXPECT_LT(index_of("subscriber_delete_begin"), index_of("subscriber_delete_ok"));
  context.Shutdown();
  tracker->Record("participant_delete_ok");
  {
    std::lock_guard lock(tracker->mutex);
    EXPECT_EQ(tracker->events.back(), "participant_delete_ok");
  }
}

}  // namespace
}  // namespace aimrt::plugins::dds_plugin

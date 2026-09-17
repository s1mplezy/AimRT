// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "loaned_benchmark_publisher_module/loaned_benchmark_publisher_module.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include "aimrt_module_ros2_interface/channel/ros2_channel.h"
#include "example_ros2/msg/benchmark_signal.hpp"
#include "example_ros2/msg/benchmark_status.hpp"
#include "loaned_benchmark_types.h"
#include "yaml-cpp/yaml.h"

namespace aimrt::examples::cpp::ros2_chn::loaned_benchmark_publisher_module {
namespace {

constexpr size_t kControlRepeatCount = 10;
constexpr auto kControlRepeatInterval = std::chrono::milliseconds(100);

std::vector<uint32_t> GenerateLastSendCounts(uint32_t total,
                                             uint32_t concurrency) {
  std::vector<uint32_t> result(concurrency + 1, 0);
  const uint32_t base = total / concurrency;
  for (uint32_t index = 1; index < concurrency; ++index)
    result[index] = result[index - 1] + base;
  result[concurrency] = total;
  return result;
}

void PublishControlRepeated(
    aimrt::channel::PublisherRef publisher,
    const example_ros2::msg::BenchmarkSignal& signal) {
  for (size_t index = 0; index < kControlRepeatCount; ++index) {
    aimrt::channel::Publish(publisher, signal);
    std::this_thread::sleep_for(kControlRepeatInterval);
  }
}

void FillBeginSignal(example_ros2::msg::BenchmarkSignal& signal,
                     uint32_t plan_id, std::string mode,
                     uint32_t topic_number, uint32_t parallel_number,
                     uint32_t send_num, uint32_t message_size,
                     uint32_t send_frequency) {
  signal.status = example_ros2::msg::BenchmarkStatus::BEGIN;
  signal.bench_plan_id = plan_id;
  signal.mode = std::move(mode);
  signal.topic_number = topic_number;
  signal.parallel_number = parallel_number;
  signal.send_num = send_num;
  signal.message_size = message_size;
  signal.send_frequency = send_frequency;
}

}  // namespace

bool LoanedBenchmarkPublisherModule::Initialize(aimrt::CoreRef core) {
  core_ = core;
  try {
    const auto config_path = core_.GetConfigurator().GetConfigFilePath();
    AIMRT_CHECK_ERROR_THROW(!config_path.empty(),
                            "Loan benchmark publisher config is required.");
    const auto config = YAML::LoadFile(std::string(config_path));
    max_topic_number_ = config["max_topic_number"].as<uint32_t>();
    max_parallel_number_ = config["max_parallel_number"].as<uint32_t>();
    loan_message_size_ = config["loan_message_size"].as<uint32_t>();
    AIMRT_CHECK_ERROR_THROW(max_topic_number_ > 0,
                            "max_topic_number must be positive.");
    AIMRT_CHECK_ERROR_THROW(max_parallel_number_ > 0,
                            "max_parallel_number must be positive.");
    AIMRT_CHECK_ERROR_THROW(IsSupportedLoanedBenchmarkSize(loan_message_size_),
                            "Unsupported loan_message_size {}.",
                            loan_message_size_);

    for (const auto& node : config["bench_plans"]) {
      BenchPlan plan;
      const auto mode = node["perf_mode"].as<std::string>();
      if (mode == "multi-topic") {
        plan.mode = BenchPlan::PerfMode::kMultiTopic;
        plan.topic_number = node["topic_number"].as<uint32_t>();
        AIMRT_CHECK_ERROR_THROW(
            plan.topic_number > 0 && plan.topic_number <= max_topic_number_,
            "Invalid topic_number {}.", plan.topic_number);
      } else if (mode == "parallel") {
        plan.mode = BenchPlan::PerfMode::kParallel;
        plan.parallel_number = node["parallel_number"].as<uint32_t>();
        AIMRT_CHECK_ERROR_THROW(
            plan.parallel_number > 0 &&
                plan.parallel_number <= max_parallel_number_,
            "Invalid parallel_number {}.", plan.parallel_number);
      } else {
        throw aimrt::common::util::AimRTException("Unsupported perf mode: " +
                                                  mode);
      }
      plan.channel_frq = node["channel_frq"].as<uint32_t>();
      plan.msg_size = node["msg_size"].as<uint32_t>();
      plan.msg_count = node["msg_count"].as<uint32_t>();
      AIMRT_CHECK_ERROR_THROW(plan.channel_frq > 0,
                              "channel_frq must be positive.");
      AIMRT_CHECK_ERROR_THROW(plan.msg_count > 0,
                              "msg_count must be positive.");
      AIMRT_CHECK_ERROR_THROW(
          plan.msg_size == loan_message_size_,
          "Every plan msg_size {} must equal loan_message_size {}.",
          plan.msg_size, loan_message_size_);
      bench_plans_.emplace_back(plan);
    }
    AIMRT_CHECK_ERROR_THROW(!bench_plans_.empty(),
                            "At least one bench plan is required.");

    publish_control_executor_ =
        core_.GetExecutorManager().GetExecutor("publish_control_executor");
    AIMRT_CHECK_ERROR_THROW(publish_control_executor_,
                            "Get publish_control_executor failed.");
    signal_publisher_ =
        core_.GetChannelHandle().GetPublisher("benchmark_signal");
    AIMRT_CHECK_ERROR_THROW(signal_publisher_,
                            "Get benchmark_signal publisher failed.");
    AIMRT_CHECK_ERROR_THROW(
        aimrt::channel::RegisterPublishType<
            example_ros2::msg::BenchmarkSignal>(signal_publisher_),
        "Register BenchmarkSignal failed.");

    for (uint32_t index = 0;
         index < std::max(max_topic_number_, max_parallel_number_); ++index) {
      auto executor = core_.GetExecutorManager().GetExecutor(
          "publish_executor_" + std::to_string(index));
      AIMRT_CHECK_ERROR_THROW(executor, "Get publish executor {} failed.",
                              index);
      executor_vec_.emplace_back(executor);
    }

    for (uint32_t index = 0; index < max_topic_number_; ++index) {
      auto publisher = core_.GetChannelHandle().GetPublisher(
          "test_topic_" + std::to_string(index));
      AIMRT_CHECK_ERROR_THROW(publisher, "Get data publisher {} failed.",
                              index);
      const bool registered = DispatchLoanedBenchmarkType(
          loan_message_size_, [&]<typename MsgType>() {
            return aimrt::channel::RegisterPublishType<MsgType>(publisher);
          });
      AIMRT_CHECK_ERROR_THROW(registered,
                              "Register loan data type {} failed.",
                              loan_message_size_);
      publisher_vec_.emplace_back(publisher);
    }
  } catch (const std::exception& error) {
    AIMRT_ERROR("Init failed, {}", error.what());
    return false;
  }
  AIMRT_INFO("Init succeeded, explicit loan message size={}.",
             loan_message_size_);
  return true;
}

bool LoanedBenchmarkPublisherModule::Start() {
  try {
    run_flag_ = true;
    publish_control_executor_.Execute([this] { MainLoop(); });
  } catch (const std::exception& error) {
    AIMRT_ERROR("Start failed, {}", error.what());
    return false;
  }
  return true;
}

void LoanedBenchmarkPublisherModule::Shutdown() {
  try {
    if (run_flag_.exchange(false)) stop_signal_.get_future().wait();
  } catch (const std::exception& error) {
    AIMRT_ERROR("Shutdown failed, {}", error.what());
  }
}

void LoanedBenchmarkPublisherModule::MainLoop() {
  try {
    AIMRT_INFO("Start explicit-loan benchmark.");
    for (size_t index = 0; index < 10 && run_flag_; ++index) {
      example_ros2::msg::BenchmarkSignal signal;
      signal.status = example_ros2::msg::BenchmarkStatus::WARM_UP;
      aimrt::channel::Publish(signal_publisher_, signal);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    for (size_t index = 0; index < bench_plans_.size() && run_flag_; ++index) {
      StartSinglePlan(static_cast<uint32_t>(index), bench_plans_[index]);
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    AIMRT_INFO("Bench completed.");
  } catch (const std::exception& error) {
    AIMRT_ERROR("Loan benchmark aborted: {}", error.what());
  }
  stop_signal_.set_value();
}

void LoanedBenchmarkPublisherModule::StartSinglePlan(
    uint32_t plan_id, const BenchPlan& plan) {
  if (plan.mode == BenchPlan::PerfMode::kMultiTopic)
    StartMultiTopicPlan(plan_id, plan);
  else
    StartParallelPlan(plan_id, plan);
}

void LoanedBenchmarkPublisherModule::PublishRange(
    aimrt::channel::PublisherRef publisher, uint32_t msg_size,
    uint32_t begin_seq, uint32_t count, uint32_t frequency) {
  const bool dispatched = DispatchLoanedBenchmarkType(
      msg_size, [&]<typename MsgType>() {
        auto loaned_publisher =
            aimrt::channel::PrepareLoanedPublisher<MsgType>(publisher);
        AIMRT_CHECK_ERROR_THROW(
            loaned_publisher,
            "Prepare explicit loan publisher failed, status={}.",
            static_cast<int>(loaned_publisher.Status()));
        const auto period =
            std::chrono::nanoseconds(1000000000ULL / frequency);
        auto next_publish_time = std::chrono::steady_clock::now();
        for (uint32_t offset = 0; offset < count && run_flag_; ++offset) {
          auto message = loaned_publisher.BorrowLoanedMessage();
          AIMRT_CHECK_ERROR_THROW(
              message, "Borrow explicit loan failed, status={}.",
              static_cast<int>(message.Status()));
          message->data.fill(static_cast<uint8_t>('a'));
          message->seq = begin_seq + offset;
          message->timestamp = aimrt::common::util::GetCurTimestampNs();
          const auto status = loaned_publisher.Publish(std::move(message));
          AIMRT_CHECK_ERROR_THROW(
              aimrt::channel::LoanSucceeded(status),
              "Publish explicit loan failed, status={}.",
              static_cast<int>(status));
          next_publish_time += period;
          std::this_thread::sleep_until(next_publish_time);
        }
        return true;
      });
  AIMRT_CHECK_ERROR_THROW(dispatched, "Unsupported loan message size {}.",
                          msg_size);
}

void LoanedBenchmarkPublisherModule::StartMultiTopicPlan(
    uint32_t plan_id, const BenchPlan& plan) {
  example_ros2::msg::BenchmarkSignal begin;
  FillBeginSignal(begin, plan_id, "multi-topic", plan.topic_number, 1,
                  plan.msg_count, plan.msg_size, plan.channel_frq);
  PublishControlRepeated(signal_publisher_, begin);

  std::vector<std::future<void>> futures;
  for (uint32_t index = 0; index < plan.topic_number; ++index) {
    std::promise<void> completion;
    futures.emplace_back(completion.get_future());
    auto publisher = publisher_vec_[index];
    executor_vec_[index].Execute(
        [this, publisher, plan,
         completion{std::move(completion)}]() mutable {
          PublishRange(publisher, plan.msg_size, 0, plan.msg_count,
                       plan.channel_frq);
          completion.set_value();
        });
  }
  for (auto& future : futures) future.get();

  std::this_thread::sleep_for(std::chrono::seconds(1));
  example_ros2::msg::BenchmarkSignal end;
  end.status = example_ros2::msg::BenchmarkStatus::END;
  end.bench_plan_id = plan_id;
  PublishControlRepeated(signal_publisher_, end);
}

void LoanedBenchmarkPublisherModule::StartParallelPlan(
    uint32_t plan_id, const BenchPlan& plan) {
  example_ros2::msg::BenchmarkSignal begin;
  FillBeginSignal(begin, plan_id, "parallel", 1, plan.parallel_number,
                  plan.msg_count, plan.msg_size, plan.channel_frq);
  PublishControlRepeated(signal_publisher_, begin);

  const auto boundaries =
      GenerateLastSendCounts(plan.msg_count, plan.parallel_number);
  std::vector<std::future<void>> futures;
  for (uint32_t index = 0; index < plan.parallel_number; ++index) {
    std::promise<void> completion;
    futures.emplace_back(completion.get_future());
    executor_vec_[index].Execute(
        [this, plan, boundaries, index,
         completion{std::move(completion)}]() mutable {
          PublishRange(publisher_vec_[0], plan.msg_size, boundaries[index],
                       boundaries[index + 1] - boundaries[index],
                       plan.channel_frq);
          completion.set_value();
        });
  }
  for (auto& future : futures) future.get();

  std::this_thread::sleep_for(std::chrono::seconds(1));
  example_ros2::msg::BenchmarkSignal end;
  end.status = example_ros2::msg::BenchmarkStatus::END;
  end.bench_plan_id = plan_id;
  PublishControlRepeated(signal_publisher_, end);
}

}  // namespace aimrt::examples::cpp::ros2_chn::loaned_benchmark_publisher_module

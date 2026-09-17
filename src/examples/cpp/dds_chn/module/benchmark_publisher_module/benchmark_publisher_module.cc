// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "benchmark_publisher_module/benchmark_publisher_module.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <utility>

#include "Benchmark.h"
#include "aimrt_module_dds_interface/channel/dds_channel.h"
#include "yaml-cpp/yaml.h"

namespace aimrt::examples::cpp::dds_chn::benchmark_publisher_module {
namespace {

constexpr size_t kControlSignalRepeatCount = 10;
constexpr auto kControlSignalRepeatInterval = std::chrono::milliseconds(100);

std::vector<uint8_t> GeneratePayload(uint32_t size) {
  return std::vector<uint8_t>(size, static_cast<uint8_t>('a'));
}

std::vector<uint32_t> GenerateLastSendCounts(uint32_t total, uint32_t concurrency) {
  std::vector<uint32_t> last_send_counts(concurrency + 1, 0);
  const uint32_t base_count = total / concurrency;
  for (uint32_t index = 1; index < concurrency; ++index) {
    last_send_counts[index] = last_send_counts[index - 1] + base_count;
  }
  last_send_counts[concurrency] = total;
  return last_send_counts;
}

void FillBeginSignal(aimrt_examples::dds::BenchmarkSignal& signal,
                     uint32_t plan_id,
                     std::string mode,
                     uint32_t topic_number,
                     uint32_t parallel_number,
                     uint32_t send_num,
                     uint32_t message_size,
                     uint32_t send_frequency) {
  signal.status(aimrt_examples::dds::BenchmarkStatus::BEGIN);
  signal.bench_plan_id(plan_id);
  signal.mode(std::move(mode));
  signal.topic_number(topic_number);
  signal.parallel_number(parallel_number);
  signal.send_num(send_num);
  signal.message_size(message_size);
  signal.send_frequency(send_frequency);
}

void PublishControlSignalRepeated(
    aimrt::channel::PublisherRef publisher,
    const aimrt_examples::dds::BenchmarkSignal& signal) {
  for (size_t index = 0; index < kControlSignalRepeatCount; ++index) {
    aimrt::channel::Publish(publisher, signal);
    std::this_thread::sleep_for(kControlSignalRepeatInterval);
  }
}

}  // namespace

bool BenchmarkPublisherModule::Initialize(aimrt::CoreRef core) {
  core_ = core;
  try {
    const auto config_path = core_.GetConfigurator().GetConfigFilePath();
    AIMRT_CHECK_ERROR_THROW(!config_path.empty(), "Benchmark publisher config file is required.");

    const auto config = YAML::LoadFile(std::string(config_path));
    max_topic_number_ = config["max_topic_number"].as<uint32_t>();
    max_parallel_number_ = config["max_parallel_number"].as<uint32_t>();
    AIMRT_CHECK_ERROR_THROW(max_topic_number_ > 0, "max_topic_number must be positive.");
    AIMRT_CHECK_ERROR_THROW(max_parallel_number_ > 0, "max_parallel_number must be positive.");

    if (config["bench_plans"] && config["bench_plans"].IsSequence()) {
      for (const auto& plan_node : config["bench_plans"]) {
        BenchPlan plan;
        const auto perf_mode = plan_node["perf_mode"].as<std::string>();
        if (perf_mode == "multi-topic") {
          plan.mode = BenchPlan::PerfMode::kMultiTopic;
          plan.topic_number = plan_node["topic_number"].as<uint32_t>();
          AIMRT_CHECK_ERROR_THROW(plan.topic_number > 0 && plan.topic_number <= max_topic_number_,
                                  "Invalid bench plan topic number({}), max is {}.",
                                  plan.topic_number, max_topic_number_);
        } else if (perf_mode == "parallel") {
          plan.mode = BenchPlan::PerfMode::kParallel;
          plan.parallel_number = plan_node["parallel_number"].as<uint32_t>();
          AIMRT_CHECK_ERROR_THROW(plan.parallel_number > 0 && plan.parallel_number <= max_parallel_number_,
                                  "Invalid bench plan parallel number({}), max is {}.",
                                  plan.parallel_number, max_parallel_number_);
        } else {
          throw aimrt::common::util::AimRTException("Unsupported perf mode: " + perf_mode);
        }

        plan.channel_frq = plan_node["channel_frq"].as<uint32_t>();
        plan.msg_size = plan_node["msg_size"].as<uint32_t>();
        plan.msg_count = plan_node["msg_count"].as<uint32_t>();
        AIMRT_CHECK_ERROR_THROW(plan.channel_frq > 0, "channel_frq must be positive.");
        AIMRT_CHECK_ERROR_THROW(plan.msg_size <= 16777216U,
                                "msg_size({}) exceeds BenchmarkMessage's 16 MiB IDL bound.", plan.msg_size);
        AIMRT_CHECK_ERROR_THROW(plan.msg_count > 0, "msg_count must be positive.");
        bench_plans_.emplace_back(plan);
      }
    }

    publish_control_executor_ = core_.GetExecutorManager().GetExecutor("publish_control_executor");
    AIMRT_CHECK_ERROR_THROW(publish_control_executor_, "Get executor 'publish_control_executor' failed.");

    signal_publisher_ = core_.GetChannelHandle().GetPublisher("benchmark_signal");
    AIMRT_CHECK_ERROR_THROW(signal_publisher_, "Get publisher for topic 'benchmark_signal' failed.");
    AIMRT_CHECK_ERROR_THROW(
        aimrt::channel::RegisterPublishType<aimrt_examples::dds::BenchmarkSignal>(signal_publisher_),
        "Register DDS BenchmarkSignal failed.");

    for (uint32_t index = 0; index < std::max(max_parallel_number_, max_topic_number_); ++index) {
      const auto executor_name = "publish_executor_" + std::to_string(index);
      auto executor = core_.GetExecutorManager().GetExecutor(executor_name);
      AIMRT_CHECK_ERROR_THROW(executor, "Get executor '{}' failed.", executor_name);
      executor_vec_.emplace_back(executor);
    }

    for (uint32_t index = 0; index < max_topic_number_; ++index) {
      const auto topic_name = "test_topic_" + std::to_string(index);
      auto publisher = core_.GetChannelHandle().GetPublisher(topic_name);
      AIMRT_CHECK_ERROR_THROW(publisher, "Get publisher for topic '{}' failed.", topic_name);
      AIMRT_CHECK_ERROR_THROW(
          aimrt::channel::RegisterPublishType<aimrt_examples::dds::BenchmarkMessage>(publisher),
          "Register DDS BenchmarkMessage failed for topic '{}'.", topic_name);
      publisher_vec_.emplace_back(publisher);
    }
  } catch (const std::exception& error) {
    AIMRT_ERROR("Init failed, {}", error.what());
    return false;
  }

  AIMRT_INFO("Init succeeded.");
  return true;
}

bool BenchmarkPublisherModule::Start() {
  try {
    run_flag_ = true;
    publish_control_executor_.Execute([this] { MainLoop(); });
  } catch (const std::exception& error) {
    AIMRT_ERROR("Start failed, {}", error.what());
    return false;
  }
  AIMRT_INFO("Start succeeded.");
  return true;
}

void BenchmarkPublisherModule::Shutdown() {
  try {
    if (run_flag_.exchange(false)) stop_signal_.get_future().wait();
  } catch (const std::exception& error) {
    AIMRT_ERROR("Shutdown failed, {}", error.what());
  }
}

void BenchmarkPublisherModule::MainLoop() {
  try {
    AIMRT_INFO("Start Bench.");
    for (size_t index = 0; index < 10 && run_flag_; ++index) {
      aimrt_examples::dds::BenchmarkSignal signal;
      signal.status(aimrt_examples::dds::BenchmarkStatus::WARM_UP);
      aimrt::channel::Publish(signal_publisher_, signal);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    for (size_t index = 0; index < bench_plans_.size() && run_flag_; ++index) {
      StartSinglePlan(static_cast<uint32_t>(index), bench_plans_[index]);
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    AIMRT_INFO("Bench completed.");
  } catch (const std::exception& error) {
    AIMRT_ERROR("Exit MainLoop with exception, {}", error.what());
  }
  stop_signal_.set_value();
}

void BenchmarkPublisherModule::StartSinglePlan(uint32_t plan_id, const BenchPlan& plan) {
  if (plan.mode == BenchPlan::PerfMode::kMultiTopic) {
    StartMultiTopicPlan(plan_id, plan);
  } else {
    StartParallelPlan(plan_id, plan);
  }
}

void BenchmarkPublisherModule::StartMultiTopicPlan(uint32_t plan_id, const BenchPlan& plan) {
  aimrt_examples::dds::BenchmarkSignal begin_signal;
  FillBeginSignal(begin_signal, plan_id, "multi-topic", plan.topic_number, 1,
                  plan.msg_count, plan.msg_size, plan.channel_frq);
  AIMRT_INFO("Publish benchmark start signal: plan_id={}, mode=multi-topic, topic_number={}, frequency={} hz, msg_size={} bytes, msg_count={}",
             plan_id, plan.topic_number, plan.channel_frq, plan.msg_size, plan.msg_count);
  PublishControlSignalRepeated(signal_publisher_, begin_signal);

  std::vector<std::future<void>> futures;
  for (uint32_t index = 0; index < plan.topic_number; ++index) {
    auto executor = executor_vec_[index];
    auto publisher = publisher_vec_[index];
    std::promise<void> task_signal;
    futures.emplace_back(task_signal.get_future());
    executor.Execute([this, publisher, plan, task_signal{std::move(task_signal)}]() mutable {
      aimrt_examples::dds::BenchmarkMessage message;
      message.data(GeneratePayload(plan.msg_size));
      const auto sleep_ns = std::chrono::nanoseconds(1000000000ULL / plan.channel_frq);
      auto next_publish_time = std::chrono::steady_clock::now();
      for (uint32_t seq = 0; seq < plan.msg_count && run_flag_; ++seq) {
        message.seq(seq);
        message.timestamp(aimrt::common::util::GetCurTimestampNs());
        aimrt::channel::Publish(publisher, message);
        next_publish_time += sleep_ns;
        std::this_thread::sleep_until(next_publish_time);
      }
      task_signal.set_value();
    });
  }
  for (auto& future : futures) future.wait();

  std::this_thread::sleep_for(std::chrono::seconds(1));
  aimrt_examples::dds::BenchmarkSignal end_signal;
  end_signal.status(aimrt_examples::dds::BenchmarkStatus::END);
  end_signal.bench_plan_id(plan_id);
  AIMRT_INFO("Publish benchmark end signal: plan_id={}", plan_id);
  PublishControlSignalRepeated(signal_publisher_, end_signal);
}

void BenchmarkPublisherModule::StartParallelPlan(uint32_t plan_id, const BenchPlan& plan) {
  aimrt_examples::dds::BenchmarkSignal begin_signal;
  FillBeginSignal(begin_signal, plan_id, "parallel", 1, plan.parallel_number,
                  plan.msg_count, plan.msg_size, plan.channel_frq);
  AIMRT_INFO("Publish benchmark start signal: plan_id={}, mode=parallel, parallel_number={}, frequency={} hz, msg_size={} bytes, msg_count={}",
             plan_id, plan.parallel_number, plan.channel_frq, plan.msg_size, plan.msg_count);
  PublishControlSignalRepeated(signal_publisher_, begin_signal);

  std::vector<std::future<void>> futures;
  const auto last_send_counts = GenerateLastSendCounts(plan.msg_count, plan.parallel_number);
  for (uint32_t index = 0; index < plan.parallel_number; ++index) {
    auto executor = executor_vec_[index];
    auto publisher = publisher_vec_[0];
    std::promise<void> task_signal;
    futures.emplace_back(task_signal.get_future());
    executor.Execute([this, publisher, plan, last_send_counts, index,
                      task_signal{std::move(task_signal)}]() mutable {
      aimrt_examples::dds::BenchmarkMessage message;
      message.data(GeneratePayload(plan.msg_size));
      const auto sleep_ns = std::chrono::nanoseconds(1000000000ULL / plan.channel_frq);
      auto next_publish_time = std::chrono::steady_clock::now();
      const uint32_t messages_to_send = last_send_counts[index + 1] - last_send_counts[index];
      for (uint32_t count = 0; count < messages_to_send && run_flag_; ++count) {
        message.seq(last_send_counts[index] + count);
        message.timestamp(aimrt::common::util::GetCurTimestampNs());
        aimrt::channel::Publish(publisher, message);
        next_publish_time += sleep_ns;
        std::this_thread::sleep_until(next_publish_time);
      }
      task_signal.set_value();
    });
  }
  for (auto& future : futures) future.wait();

  std::this_thread::sleep_for(std::chrono::seconds(1));
  aimrt_examples::dds::BenchmarkSignal end_signal;
  end_signal.status(aimrt_examples::dds::BenchmarkStatus::END);
  end_signal.bench_plan_id(plan_id);
  AIMRT_INFO("Publish benchmark end signal: plan_id={}", plan_id);
  PublishControlSignalRepeated(signal_publisher_, end_signal);
}

}  // namespace aimrt::examples::cpp::dds_chn::benchmark_publisher_module

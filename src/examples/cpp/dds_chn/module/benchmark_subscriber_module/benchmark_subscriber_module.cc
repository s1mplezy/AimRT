// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "benchmark_subscriber_module/benchmark_subscriber_module.h"

#include <algorithm>
#include <functional>
#include <string>

#include "aimrt_module_dds_interface/channel/dds_channel.h"
#include "yaml-cpp/yaml.h"

namespace aimrt::examples::cpp::dds_chn::benchmark_subscriber_module {

bool BenchmarkSubscriberModule::Initialize(aimrt::CoreRef core) {
  core_ = core;
  try {
    const auto config_path = core_.GetConfigurator().GetConfigFilePath();
    AIMRT_CHECK_ERROR_THROW(!config_path.empty(), "Benchmark subscriber config file is required.");
    const auto config = YAML::LoadFile(std::string(config_path));
    max_topic_number_ = config["max_topic_number"].as<uint32_t>();
    AIMRT_CHECK_ERROR_THROW(max_topic_number_ > 0, "max_topic_number must be positive.");

    signal_subscriber_ = core_.GetChannelHandle().GetSubscriber("benchmark_signal");
    AIMRT_CHECK_ERROR_THROW(signal_subscriber_, "Get subscriber for topic 'benchmark_signal' failed.");
    AIMRT_CHECK_ERROR_THROW(
        aimrt::channel::Subscribe<aimrt_examples::dds::BenchmarkSignal>(
            signal_subscriber_,
            [this](const std::shared_ptr<const aimrt_examples::dds::BenchmarkSignal>& data) {
              BenchmarkSignalHandle(data);
            }),
        "Subscribe DDS BenchmarkSignal failed.");

    for (uint32_t index = 0; index < max_topic_number_; ++index) {
      const auto topic_name = "test_topic_" + std::to_string(index);
      topic_records_.emplace_back(TopicRecord{.topic_name = topic_name});
      auto subscriber = core_.GetChannelHandle().GetSubscriber(topic_name);
      AIMRT_CHECK_ERROR_THROW(subscriber, "Get subscriber for topic '{}' failed.", topic_name);
      AIMRT_CHECK_ERROR_THROW(
          aimrt::channel::Subscribe<aimrt_examples::dds::BenchmarkMessage>(
              subscriber,
              [this, index](const std::shared_ptr<const aimrt_examples::dds::BenchmarkMessage>& data) {
                BenchmarkMessageHandle(index, data);
              }),
          "Subscribe DDS BenchmarkMessage failed for topic '{}'.", topic_name);
      subscribers_.emplace_back(subscriber);
    }
  } catch (const std::exception& error) {
    AIMRT_ERROR("Init failed, {}", error.what());
    return false;
  }

  AIMRT_INFO("Init succeeded.");
  return true;
}

void BenchmarkSubscriberModule::BenchmarkSignalHandle(
    const std::shared_ptr<const aimrt_examples::dds::BenchmarkSignal>& data) {
  AIMRT_INFO("Receive benchmark signal: status={}, plan_id={}",
             static_cast<uint32_t>(data->status()), data->bench_plan_id());
  try {
    std::optional<BenchmarkReport> completed_report;
    if (data->status() == aimrt_examples::dds::BenchmarkStatus::BEGIN) {
      std::lock_guard lock(state_mutex_);
      if (run_state_ == State::kRunning && data->bench_plan_id() == current_plan_id_) return;
      if (has_completed_plan_ && data->bench_plan_id() <= last_completed_plan_id_) return;
      AIMRT_CHECK_ERROR_THROW(run_state_ == State::kReadyToRun,
                              "BEGIN for plan {} received while plan {} is running.",
                              data->bench_plan_id(), current_plan_id_);

      const uint32_t topic_number = data->topic_number();
      const uint32_t expected_send_num = data->send_num();
      AIMRT_CHECK_ERROR_THROW(
          topic_number > 0 && topic_number <= max_topic_number_,
          "Invalid topic number({}), max is {}.", topic_number, max_topic_number_);
      AIMRT_CHECK_ERROR_THROW(expected_send_num > 0, "send_num must be positive.");
      AIMRT_CHECK_ERROR_THROW(data->message_size() <= 16777216U,
                              "message_size({}) exceeds BenchmarkMessage's 16 MiB IDL bound.",
                              data->message_size());

      for (uint32_t index = 0; index < topic_number; ++index) {
        topic_records_[index].message_records.clear();
        topic_records_[index].message_records.resize(expected_send_num);
      }

      current_mode_ = data->mode().c_str();
      current_plan_id_ = data->bench_plan_id();
      current_topic_number_ = topic_number;
      current_parallel_number_ = data->parallel_number();
      current_expected_send_num_ = expected_send_num;
      current_message_size_ = data->message_size();
      current_send_frequency_ = data->send_frequency();
      run_state_ = State::kRunning;
    } else if (data->status() == aimrt_examples::dds::BenchmarkStatus::END) {
      {
        std::lock_guard lock(state_mutex_);
        if (has_completed_plan_ && data->bench_plan_id() <= last_completed_plan_id_) return;
        AIMRT_CHECK_ERROR_THROW(run_state_ == State::kRunning,
                                "END for plan {} received with no active plan.", data->bench_plan_id());
        AIMRT_CHECK_ERROR_THROW(data->bench_plan_id() == current_plan_id_,
                                "END plan id {} does not match active plan {}.",
                                data->bench_plan_id(), current_plan_id_);
        completed_report.emplace(BuildReportLocked());
        last_completed_plan_id_ = current_plan_id_;
        has_completed_plan_ = true;
        run_state_ = State::kReadyToRun;
      }
      LogReport(*completed_report);
    }
  } catch (const std::exception& error) {
    AIMRT_ERROR("Exception, {}", error.what());
  }
}

void BenchmarkSubscriberModule::BenchmarkMessageHandle(
    uint32_t topic_index,
    const std::shared_ptr<const aimrt_examples::dds::BenchmarkMessage>& data) {
  const auto receive_timestamp = aimrt::common::util::GetCurTimestampNs();
  std::lock_guard lock(state_mutex_);
  auto& topic_record = topic_records_[topic_index];
  if (run_state_ != State::kRunning) [[unlikely]] {
    AIMRT_WARN("Topic '{}' is not running a benchmark.", topic_record.topic_name);
    return;
  }
  if (data->data().size() != current_message_size_) [[unlikely]] {
    AIMRT_WARN("Topic '{}' got data size {}, expected {}.",
               topic_record.topic_name, data->data().size(), current_message_size_);
    return;
  }

  const uint32_t seq = data->seq();
  if (seq >= topic_record.message_records.size()) [[unlikely]] {
    AIMRT_WARN("Invalid seq {}, topic '{}'.", seq, topic_record.topic_name);
    return;
  }
  auto& record = topic_record.message_records[seq];
  record.received = true;
  record.send_timestamp = data->timestamp();
  record.receive_timestamp = receive_timestamp;
}

BenchmarkSubscriberModule::BenchmarkReport BenchmarkSubscriberModule::BuildReportLocked() const {
  BenchmarkReport report{
      .mode = current_mode_,
      .plan_id = current_plan_id_,
      .send_frequency = current_send_frequency_,
      .topic_number = current_topic_number_,
      .parallel_number = current_parallel_number_,
      .message_size = current_message_size_,
      .expected_send_num = current_expected_send_num_,
      .send_count = static_cast<size_t>(current_expected_send_num_) * current_topic_number_};
  std::vector<uint64_t> latencies;
  latencies.reserve(report.send_count);
  uint64_t latency_sum = 0;

  for (uint32_t index = 0; index < current_topic_number_; ++index) {
    for (const auto& record : topic_records_[index].message_records) {
      if (!record.received) continue;
      uint64_t latency = 0;
      if (record.receive_timestamp < record.send_timestamp) [[unlikely]] {
        AIMRT_WARN("Invalid timestamp, recv timestamp: {}, send timestamp: {}",
                   record.receive_timestamp, record.send_timestamp);
      } else {
        latency = record.receive_timestamp - record.send_timestamp;
      }
      latency_sum += latency;
      latencies.emplace_back(latency);
    }
  }

  report.receive_count = latencies.size();
  report.loss_rate = static_cast<double>(report.send_count - report.receive_count) /
                     report.send_count * 100.0;
  if (!latencies.empty()) {
    std::sort(latencies.begin(), latencies.end());
    report.min_latency = latencies.front();
    report.max_latency = latencies.back();
    report.avg_latency = latency_sum / report.receive_count;
    report.p90_latency = latencies[static_cast<size_t>(report.receive_count * 0.9)];
    report.p99_latency = latencies[static_cast<size_t>(report.receive_count * 0.99)];
    report.p999_latency = latencies[static_cast<size_t>(report.receive_count * 0.999)];
  }
  return report;
}

void BenchmarkSubscriberModule::LogReport(const BenchmarkReport& report) const {
  AIMRT_INFO("Benchmark plan {} completed, evaluate...", report.plan_id);
  AIMRT_INFO(R"str(Benchmark plan {} completed, report:
mode: {}
frequency: {} hz
topic number: {}
parallel number: {}
msg size: {} bytes
msg count per topic: {}
send count : {}
recv count: {}
loss rate: {} %
min latency: {} us
max latency: {} us
avg latency: {} us
p90 latency: {} us
p99 latency: {} us
p999 latency: {} us
)str",
             report.plan_id, report.mode, report.send_frequency, report.topic_number,
             report.parallel_number, report.message_size, report.expected_send_num,
             report.send_count, report.receive_count, report.loss_rate,
             report.min_latency / 1000.0, report.max_latency / 1000.0,
             report.avg_latency / 1000.0, report.p90_latency / 1000.0,
             report.p99_latency / 1000.0, report.p999_latency / 1000.0);
}

}  // namespace aimrt::examples::cpp::dds_chn::benchmark_subscriber_module

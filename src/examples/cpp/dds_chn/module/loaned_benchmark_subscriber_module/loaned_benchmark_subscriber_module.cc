// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "loaned_benchmark_subscriber_module/loaned_benchmark_subscriber_module.h"

#include <algorithm>
#include <string>

#include "aimrt_module_dds_interface/channel/dds_channel.h"
#include "loaned_benchmark_types.h"
#include "yaml-cpp/yaml.h"

namespace aimrt::examples::cpp::dds_chn::loaned_benchmark_subscriber_module {

bool LoanedBenchmarkSubscriberModule::Initialize(aimrt::CoreRef core) {
  core_ = core;
  try {
    const auto config_path = core_.GetConfigurator().GetConfigFilePath();
    AIMRT_CHECK_ERROR_THROW(!config_path.empty(),
                            "Loan benchmark subscriber config is required.");
    const auto config = YAML::LoadFile(std::string(config_path));
    max_topic_number_ = config["max_topic_number"].as<uint32_t>();
    loan_message_size_ = config["loan_message_size"].as<uint32_t>();
    AIMRT_CHECK_ERROR_THROW(max_topic_number_ > 0,
                            "max_topic_number must be positive.");
    AIMRT_CHECK_ERROR_THROW(IsSupportedLoanedBenchmarkSize(loan_message_size_),
                            "Unsupported loan_message_size {}.",
                            loan_message_size_);
    signal_subscriber_ =
        core_.GetChannelHandle().GetSubscriber("benchmark_signal");
    AIMRT_CHECK_ERROR_THROW(signal_subscriber_,
                            "Get benchmark_signal subscriber failed.");
    AIMRT_CHECK_ERROR_THROW(
        aimrt::channel::Subscribe<aimrt_examples::dds::BenchmarkSignal>(
            signal_subscriber_,
            [this](const std::shared_ptr<
                   const aimrt_examples::dds::BenchmarkSignal>& signal) {
              SignalHandle(signal);
            }),
        "Subscribe BenchmarkSignal failed.");

    for (uint32_t index = 0; index < max_topic_number_; ++index) {
      const auto topic_name = "test_topic_" + std::to_string(index);
      topic_records_.emplace_back(TopicRecord{.topic_name = topic_name});
      auto subscriber = core_.GetChannelHandle().GetSubscriber(topic_name);
      AIMRT_CHECK_ERROR_THROW(subscriber, "Get data subscriber {} failed.",
                              index);
      const bool subscribed = DispatchLoanedBenchmarkType(
          loan_message_size_, [&]<typename MsgType>() {
            const auto status = aimrt::channel::SubscribeLoaned<MsgType>(
                subscriber,
                [this, index](
                    aimrt::channel::ContextRef context,
                    const aimrt::channel::LoanedMessageView<const MsgType>&
                        message) {
                  MessageHandle(
                      index, message->seq(), message->timestamp(),
                      message->data().size(),
                      context.GetMetaValue(AIMRT_CHANNEL_CONTEXT_KEY_BACKEND));
                });
            if (!aimrt::channel::LoanSucceeded(status)) {
              AIMRT_ERROR("Subscribe explicit loan failed, status={}.",
                          static_cast<int>(status));
              return false;
            }
            return true;
          });
      AIMRT_CHECK_ERROR_THROW(subscribed,
                              "Subscribe loan data type {} failed.",
                              loan_message_size_);
      subscribers_.emplace_back(subscriber);
    }
  } catch (const std::exception& error) {
    AIMRT_ERROR("Init failed, {}", error.what());
    return false;
  }
  AIMRT_INFO("Init succeeded, explicit loan message size={}.",
             loan_message_size_);
  return true;
}

void LoanedBenchmarkSubscriberModule::SignalHandle(
    const std::shared_ptr<const aimrt_examples::dds::BenchmarkSignal>& signal) {
  try {
    std::optional<BenchmarkReport> report;
    if (signal->status() == aimrt_examples::dds::BenchmarkStatus::BEGIN) {
      std::lock_guard lock(state_mutex_);
      if (state_ == State::kRunning && signal->bench_plan_id() == plan_id_)
        return;
      if (has_completed_plan_ &&
          signal->bench_plan_id() <= last_completed_plan_id_)
        return;
      AIMRT_CHECK_ERROR_THROW(state_ == State::kReady,
                              "BEGIN received while another plan is running.");
      AIMRT_CHECK_ERROR_THROW(
          signal->message_size() == loan_message_size_,
          "Signal message_size {} does not match configured loan type {}.",
          signal->message_size(), loan_message_size_);
      AIMRT_CHECK_ERROR_THROW(
          signal->topic_number() > 0 &&
              signal->topic_number() <= max_topic_number_,
          "Invalid topic_number {}.", signal->topic_number());
      AIMRT_CHECK_ERROR_THROW(signal->send_num() > 0,
                              "send_num must be positive.");
      for (uint32_t index = 0; index < signal->topic_number(); ++index) {
        auto& messages = topic_records_[index].messages;
        messages.clear();
        messages.resize(signal->send_num());
      }
      mode_ = signal->mode().c_str();
      backend_.clear();
      plan_id_ = signal->bench_plan_id();
      topic_number_ = signal->topic_number();
      parallel_number_ = signal->parallel_number();
      expected_send_num_ = signal->send_num();
      message_size_ = signal->message_size();
      send_frequency_ = signal->send_frequency();
      state_ = State::kRunning;
    } else if (signal->status() ==
               aimrt_examples::dds::BenchmarkStatus::END) {
      {
        std::lock_guard lock(state_mutex_);
        if (has_completed_plan_ &&
            signal->bench_plan_id() <= last_completed_plan_id_)
          return;
        AIMRT_CHECK_ERROR_THROW(state_ == State::kRunning,
                                "END received without an active plan.");
        AIMRT_CHECK_ERROR_THROW(signal->bench_plan_id() == plan_id_,
                                "END plan id does not match active plan.");
        AIMRT_CHECK_ERROR_THROW(!backend_.empty(),
                                "No explicit-loan data callback was observed.");
        report.emplace(BuildReportLocked());
        last_completed_plan_id_ = plan_id_;
        has_completed_plan_ = true;
        state_ = State::kReady;
      }
      LogReport(*report);
    }
  } catch (const std::exception& error) {
    AIMRT_ERROR("Signal handling failed, {}", error.what());
  }
}

void LoanedBenchmarkSubscriberModule::MessageHandle(
    uint32_t topic_index, uint32_t seq, uint64_t send_timestamp,
    size_t payload_size, std::string_view backend) {
  const auto receive_timestamp = aimrt::common::util::GetCurTimestampNs();
  std::lock_guard lock(state_mutex_);
  if (state_ != State::kRunning) return;
  if (payload_size != message_size_) {
    AIMRT_ERROR("Loan payload size {} does not match expected {}.",
                payload_size, message_size_);
    return;
  }
  auto& topic = topic_records_[topic_index];
  if (seq >= topic.messages.size()) {
    AIMRT_ERROR("Invalid loan message seq {} on topic {}.", seq,
                topic.topic_name);
    return;
  }
  if (backend_.empty()) backend_ = backend;
  if (backend_ != backend) {
    AIMRT_ERROR("Loan backend changed from {} to {}.", backend_, backend);
    return;
  }
  auto& record = topic.messages[seq];
  record.received = true;
  record.send_timestamp = send_timestamp;
  record.receive_timestamp = receive_timestamp;
}

LoanedBenchmarkSubscriberModule::BenchmarkReport
LoanedBenchmarkSubscriberModule::BuildReportLocked() const {
  BenchmarkReport report{
      .mode = mode_,
      .backend = backend_,
      .plan_id = plan_id_,
      .send_frequency = send_frequency_,
      .topic_number = topic_number_,
      .parallel_number = parallel_number_,
      .message_size = message_size_,
      .expected_send_num = expected_send_num_,
      .send_count = static_cast<size_t>(expected_send_num_) * topic_number_};
  std::vector<uint64_t> latencies;
  latencies.reserve(report.send_count);
  uint64_t sum = 0;
  for (uint32_t index = 0; index < topic_number_; ++index) {
    for (const auto& record : topic_records_[index].messages) {
      if (!record.received) continue;
      const uint64_t latency =
          record.receive_timestamp >= record.send_timestamp
              ? record.receive_timestamp - record.send_timestamp
              : 0;
      latencies.emplace_back(latency);
      sum += latency;
    }
  }
  report.receive_count = latencies.size();
  report.loss_rate = static_cast<double>(report.send_count -
                                         report.receive_count) /
                     report.send_count * 100.0;
  if (!latencies.empty()) {
    std::sort(latencies.begin(), latencies.end());
    report.min_latency = latencies.front();
    report.max_latency = latencies.back();
    report.avg_latency = sum / report.receive_count;
    report.p90_latency =
        latencies[static_cast<size_t>(report.receive_count * 0.9)];
    report.p99_latency =
        latencies[static_cast<size_t>(report.receive_count * 0.99)];
    report.p999_latency =
        latencies[static_cast<size_t>(report.receive_count * 0.999)];
  }
  return report;
}

void LoanedBenchmarkSubscriberModule::LogReport(
    const BenchmarkReport& report) const {
  AIMRT_INFO("Explicit loan path verified, backend={}, msg_size={}.",
             report.backend, report.message_size);
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
             report.plan_id, report.mode, report.send_frequency,
             report.topic_number, report.parallel_number,
             report.message_size, report.expected_send_num,
             report.send_count, report.receive_count, report.loss_rate,
             report.min_latency / 1000.0, report.max_latency / 1000.0,
             report.avg_latency / 1000.0, report.p90_latency / 1000.0,
             report.p99_latency / 1000.0, report.p999_latency / 1000.0);
}

}  // namespace aimrt::examples::cpp::dds_chn::loaned_benchmark_subscriber_module

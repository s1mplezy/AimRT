// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aimrt_module_cpp_interface/module_base.h"
#include "example_ros2/msg/benchmark_signal.hpp"

namespace aimrt::examples::cpp::ros2_chn::loaned_benchmark_subscriber_module {

class LoanedBenchmarkSubscriberModule : public aimrt::ModuleBase {
 public:
  ModuleInfo Info() const override {
    return ModuleInfo{.name = "LoanedBenchmarkSubscriberModule"};
  }
  bool Initialize(aimrt::CoreRef core) override;
  bool Start() override { return true; }
  void Shutdown() override {}

 private:
  enum class State : uint32_t { kReady,
                                kRunning };
  struct TopicRecord {
    std::string topic_name;
    struct MessageRecord {
      bool received = false;
      uint64_t send_timestamp = 0;
      uint64_t receive_timestamp = 0;
    };
    std::vector<MessageRecord> messages;
  };
  struct BenchmarkReport {
    std::string mode;
    std::string backend;
    uint32_t plan_id = 0;
    uint32_t send_frequency = 0;
    uint32_t topic_number = 0;
    uint32_t parallel_number = 0;
    uint32_t message_size = 0;
    uint32_t expected_send_num = 0;
    size_t send_count = 0;
    size_t receive_count = 0;
    double loss_rate = 0.0;
    uint64_t min_latency = 0;
    uint64_t max_latency = 0;
    uint64_t avg_latency = 0;
    uint64_t p90_latency = 0;
    uint64_t p99_latency = 0;
    uint64_t p999_latency = 0;
  };

  auto GetLogger() const { return core_.GetLogger(); }
  void SignalHandle(
      const std::shared_ptr<const example_ros2::msg::BenchmarkSignal>& signal);
  void MessageHandle(uint32_t topic_index, uint32_t seq,
                     uint64_t send_timestamp, size_t payload_size,
                     std::string_view backend);
  BenchmarkReport BuildReportLocked() const;
  void LogReport(const BenchmarkReport& report) const;

  aimrt::CoreRef core_;
  aimrt::channel::SubscriberRef signal_subscriber_;
  std::vector<aimrt::channel::SubscriberRef> subscribers_;
  uint32_t max_topic_number_ = 0;
  uint32_t loan_message_size_ = 0;
  mutable std::mutex state_mutex_;
  State state_ = State::kReady;
  bool has_completed_plan_ = false;
  uint32_t last_completed_plan_id_ = 0;
  std::string mode_;
  std::string backend_;
  uint32_t plan_id_ = 0;
  uint32_t topic_number_ = 0;
  uint32_t parallel_number_ = 0;
  uint32_t expected_send_num_ = 0;
  uint32_t message_size_ = 0;
  uint32_t send_frequency_ = 0;
  std::vector<TopicRecord> topic_records_;
};

}  // namespace aimrt::examples::cpp::ros2_chn::loaned_benchmark_subscriber_module

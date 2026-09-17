// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include <atomic>
#include <cstdint>
#include <future>
#include <vector>

#include "aimrt_module_cpp_interface/module_base.h"

namespace aimrt::examples::cpp::ros2_chn::loaned_benchmark_publisher_module {

class LoanedBenchmarkPublisherModule : public aimrt::ModuleBase {
 public:
  ModuleInfo Info() const override {
    return ModuleInfo{.name = "LoanedBenchmarkPublisherModule"};
  }
  bool Initialize(aimrt::CoreRef core) override;
  bool Start() override;
  void Shutdown() override;

 private:
  struct BenchPlan {
    enum class PerfMode : uint8_t { kMultiTopic,
                                    kParallel };
    PerfMode mode = PerfMode::kMultiTopic;
    uint32_t channel_frq = 0;
    uint32_t msg_size = 0;
    uint32_t topic_number = 1;
    uint32_t parallel_number = 1;
    uint32_t msg_count = 0;
  };

  auto GetLogger() { return core_.GetLogger(); }
  void MainLoop();
  void StartSinglePlan(uint32_t plan_id, const BenchPlan& plan);
  void StartMultiTopicPlan(uint32_t plan_id, const BenchPlan& plan);
  void StartParallelPlan(uint32_t plan_id, const BenchPlan& plan);
  void PublishRange(aimrt::channel::PublisherRef publisher,
                    uint32_t msg_size, uint32_t begin_seq,
                    uint32_t count, uint32_t frequency);

  aimrt::CoreRef core_;
  std::atomic_bool run_flag_ = false;
  std::promise<void> stop_signal_;
  aimrt::executor::ExecutorRef publish_control_executor_;
  aimrt::channel::PublisherRef signal_publisher_;
  std::vector<aimrt::executor::ExecutorRef> executor_vec_;
  std::vector<aimrt::channel::PublisherRef> publisher_vec_;
  uint32_t max_topic_number_ = 0;
  uint32_t max_parallel_number_ = 0;
  uint32_t loan_message_size_ = 0;
  std::vector<BenchPlan> bench_plans_;
};

}  // namespace aimrt::examples::cpp::ros2_chn::loaned_benchmark_publisher_module

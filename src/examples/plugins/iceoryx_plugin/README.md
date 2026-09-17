# iceoryx plugin examples

## protobuf channel

一个基于 protobuf 协议与 iceoryx 后端的 channel 示例，演示内容包括：
- 如何在配置文件中加载**iceoryx_plugin**；
- 如何使用 iceoryx 类型的 channel 后端；


核心代码：
- [event.proto](../../../protocols/pb/example/event.proto)
- [normal_publisher_module.cc](../../cpp/pb_chn/module/normal_publisher_module/normal_publisher_module.cc)
- [normal_subscriber_module.cc](../../cpp/pb_chn/module/normal_subscriber_module/normal_subscriber_module.cc)


配置文件：
- [examples_plugins_iceoryx_plugin_pb_chn_pub_cfg.yaml](./install/linux/bin/cfg/examples_plugins_iceoryx_plugin_pb_chn_pub_cfg.yaml)
- [examples_plugins_iceoryx_plugin_pb_chn_sub_cfg.yaml](./install/linux/bin/cfg/examples_plugins_iceoryx_plugin_pb_chn_sub_cfg.yaml)

运行方式（linux）：
- 开启 `AIMRT_BUILD_EXAMPLES`、`AIMRT_BUILD_ICEORYX_PLUGIN` 选项编译 AimRT；
- 编译成功后，在终端运行 build 目录下 iox-roudi 可执行文件以启动 iceoryx 的守护进程；
- 开启新的终端运行 build 目录下`start_examples_plugins_iceoryx_plugin_pb_chn_sub.sh`脚本启动订阅端（sub 进程）；
- 再开启一个新的终端窗口运行`start_examples_plugins_iceoryx_plugin_pb_chn_pub.sh`脚本启动发布端（pub 进程）；
- 分别在开启的三个终端键入`ctrl-c`停止对应进程；


说明：
- 此示例创建了以下两个模块：
  - `NormalPublisherModule`：会基于 `work_thread_pool` 执行器，以配置的频率、向配置的 topic 中发布 `ExampleEventMsg` 类型的消息；
  - `NormalSubscriberModule`：会订阅配置的 topic 下的 `ExampleEventMsg` 类型的消息；
- 此示例将 `NormalPublisherModule` 和 `NormalSubscriberModule` 分别集成到 `pb_chn_pub_pkg` 和 `pb_chn_sub_pkg` 两个 Pkg 中，并在两个配置文件中分别加载对应的 Pkg 到 pub 和 sub 进程中；
- 此示例加载了**iceoryx_plugin**，并使用 iceoryx 类型的 channel 后端进行通信；

## DDS Loan channel benchmark

该示例复用 `src/examples/cpp/dds_chn` 中的显式 Loan benchmark module，以 DDS
IDL 生成的固定布局类型作为消息类型，并使用 Iceoryx backend 传递 backend-owned
native memory。数据消息必须通过显式 Loan 发布和订阅；Loan 不可用时 benchmark
会直接失败，不会回退到普通 `Publish`/`Subscribe`。

编译时开启 `AIMRT_BUILD_EXAMPLES`、`AIMRT_BUILD_ICEORYX_PLUGIN` 和
`AIMRT_BUILD_WITH_DDS`。在 build 目录打开三个终端，分别运行：

```bash
./iox-roudi -c ./cfg/roudi_loan_config.toml
./start_examples_plugins_iceoryx_plugin_dds_loan_chn_benchmark_sub.sh
./start_examples_plugins_iceoryx_plugin_dds_loan_chn_benchmark_pub.sh
```

启动 subscriber 和 publisher 时可通过环境变量选择参数，默认测试 1 KiB。例如
测试 64 KiB 时，两个终端都要设置相同的 `MSG_SIZE`：

```bash
MSG_SIZE=65536 \
  ./start_examples_plugins_iceoryx_plugin_dds_loan_chn_benchmark_sub.sh

MSG_SIZE=65536 MSG_COUNT=10000 \
  ./start_examples_plugins_iceoryx_plugin_dds_loan_chn_benchmark_pub.sh
```

`MSG_SIZE` 支持 `1024`、`65536`、`1048576` 和 `16777216`；pub/sub 必须设置为
相同值。还可设置 `PERF_MODE`、`CHANNEL_FRQ`、`FREQ`、`MSG_COUNT`、
`PARALLEL_NUMBER` 和 `TOPIC_NUMBER`。subscriber 输出以下日志表示显式 Loan
路径已生效：

```text
Explicit loan path verified, backend=iceoryx, msg_size=<bytes>.
```

# ros2 plugin examples

## protobuf rpc

一个基于 protobuf 协议、协程型接口与 ros2 后端的 rpc 示例，演示内容包括：
- 如何在配置文件中加载**ros2_plugin**；
- 如何使用 ros2 类型的 rpc 后端；
- 如何与原生 ros2 humble 节点进行 protobuf 协议的 rpc 相互调用；


核心代码：
- [rpc.proto](../../../protocols/pb/example/rpc.proto)
- aimrt code:
  - [normal_rpc_co_client_module.cc](../../cpp/pb_rpc/module/normal_rpc_co_client_module/normal_rpc_co_client_module.cc)
  - [normal_rpc_co_server_module.cc](../../cpp/pb_rpc/module/normal_rpc_co_server_module/normal_rpc_co_server_module.cc)
  - [service.cc](../../cpp/pb_rpc/module/normal_rpc_co_server_module/service.cc)
- native ros2 humble code:
  - [native_ros2_pb_rpc_client/main.cc](./assistant/native_ros2_pb_rpc_client/main.cc)
  - [native_ros2_pb_rpc_server/main.cc](./assistant/native_ros2_pb_rpc_server/main.cc)


配置文件：
- [examples_plugins_ros2_plugin_pb_rpc_server_cfg.yaml](./install/linux/bin/cfg/examples_plugins_ros2_plugin_pb_rpc_server_cfg.yaml)
- [examples_plugins_ros2_plugin_pb_rpc_client_cfg.yaml](./install/linux/bin/cfg/examples_plugins_ros2_plugin_pb_rpc_client_cfg.yaml)


运行方式（linux）：
- 开启 `AIMRT_BUILD_EXAMPLES`、`AIMRT_BUILD_ROS2_PLUGIN` 选项编译 AimRT；
- aimrt client 调用 aimrt server：
  - 在终端运行 build 目录下`start_examples_plugins_ros2_plugin_pb_rpc_server.sh`脚本启动服务端（srv 进程）；
  - 开启新的终端运行 build 目录下`start_examples_plugins_ros2_plugin_pb_rpc_client.sh`脚本启动客户端（cli 进程）；
  - 分别在两个终端键入`ctrl-c`停止对应进程；
- aimrt client 调用 native ros2 server：
  - 在 build 目录下运行`start_examples_plugins_ros2_plugin_native_pb_rpc_server.sh`脚本以启动原生 ros2 服务端（srv 进程）；
  - 开启新的终端运行 build 目录下`start_examples_plugins_ros2_plugin_pb_rpc_client.sh`脚本启动客户端（cli 进程）；
  - 分别在两个终端键入`ctrl-c`停止对应进程；
- native ros2 client 调用 aimrt server：
  - 在终端运行 build 目录下`start_examples_plugins_ros2_plugin_pb_rpc_server.sh`脚本启动服务端（srv 进程）；
  - 开启新的终端，在 build 目录下运行`start_examples_plugins_ros2_plugin_native_pb_rpc_client.sh`脚本以启动原生 ros2 客户端（cli 进程）；
  - 分别在两个终端键入`ctrl-c`停止对应进程；



说明：
- 此示例创建了以下两个 aimrt 模块：
  - `NormalRpcCoClientModule`：会基于 `work_thread_pool` 执行器，以配置的频率，通过协程 Client 接口，向 `ExampleService` 发起 RPC 请求；
  - `NormalRpcCoServerModule`：会注册 `ExampleService` 服务端，通过协程 Server 接口，提供 echo 功能；
- 此示例在 Rpc Client 端和 Server 端分别注册了两个 Filter 用于打印请求日志和计算耗时；
- 此示例将 `NormalRpcCoClientModule` 和 `NormalRpcCoServerModule` 分别集成到 `pb_rpc_client_pkg` 和 `pb_rpc_server_pkg` 两个 Pkg 中，并在两个配置文件中分别加载对应的 Pkg 到 srv 和 cli 进程中；
- 此示例加载了**ros2_plugin**，并使用 ros2 类型的 rpc 后端进行通信，并在客户端配置了 `timeout_handle` 执行器作为超时执行器；
- 此示例还创建了两个原生 ros2 节点，用于展示如何使用原生 ros2 程序与 aimrt 程序通过 ros2 后端 进行 rpc 通信；


## ros2 rpc

一个基于 ros2 srv 协议、协程型接口与 ros2 后端的 rpc 示例，演示内容包括：
- 如何在配置文件中加载**ros2_plugin**；
- 如何使用 ros2 类型的 rpc 后端；
- 如何与原生 ros2 humble 节点进行 ros2 srv 协议的 rpc 相互调用；


核心代码：
- [example_ros2/srv/RosTestRpc.srv](../../../protocols/example_ros2/srv/RosTestRpc.srv)
- aimrt code:
  - [normal_rpc_co_client_module.cc](../../cpp/ros2_rpc/module/normal_rpc_co_client_module/normal_rpc_co_client_module.cc)
  - [normal_rpc_co_server_module.cc](../../cpp/ros2_rpc/module/normal_rpc_co_server_module/normal_rpc_co_server_module.cc)
  - [service.cc](../../cpp/ros2_rpc/module/normal_rpc_co_server_module/service.cc)
- native ros2 humble code:
  - [native_ros2_rpc_client/main.cc](./assistant/native_ros2_rpc_client/main.cc)
  - [native_ros2_rpc_server/main.cc](./assistant/native_ros2_rpc_server/main.cc)


配置文件：
- [examples_plugins_ros2_plugin_ros2_rpc_server_cfg.yaml](./install/linux/bin/cfg/examples_plugins_ros2_plugin_ros2_rpc_server_cfg.yaml)
- [examples_plugins_ros2_plugin_ros2_rpc_client_cfg.yaml](./install/linux/bin/cfg/examples_plugins_ros2_plugin_ros2_rpc_client_cfg.yaml)


运行方式（linux）：
- 开启 `AIMRT_BUILD_EXAMPLES`、`AIMRT_BUILD_ROS2_PLUGIN` 选项编译 AimRT；
- aimrt client 调用 aimrt server：
  - 在终端运行 build 目录下`start_examples_plugins_ros2_plugin_ros2_rpc_server.sh`脚本启动服务端（srv 进程）；
  - 开启新的终端运行 build 目录下`start_examples_plugins_ros2_plugin_ros2_rpc_client.sh`脚本启动客户端（cli 进程）；
  - 分别在两个终端键入`ctrl-c`停止对应进程；
- aimrt client 调用 native ros2 server：
  - 在 build 目录下运行`start_examples_plugins_ros2_plugin_native_ros2_rpc_server.sh`脚本以启动原生 ros2 服务端（srv 进程）；
  - 开启新的终端运行 build 目录下`start_examples_plugins_ros2_plugin_ros2_rpc_client.sh`脚本启动客户端（cli 进程）；
  - 分别在两个终端键入`ctrl-c`停止对应进程；
- native ros2 client 调用 aimrt server：
  - 在终端运行 build 目录下`start_examples_plugins_ros2_plugin_ros2_rpc_server.sh`脚本启动服务端（srv 进程）；
  - 开启新的终端，在 build 目录下运行`start_examples_plugins_ros2_plugin_native_ros2_rpc_client.sh`脚本以启动原生 ros2 客户端（cli 进程）；
  - 分别在两个终端键入`ctrl-c`停止对应进程；



说明：
- 本示例与 **protobuf rpc** 示例基本一致，除了业务层使用的是 ros2 srv 形式的协议；



## ros2 rpc with qos

一个基于 ros2 srv 协议、协程型接口与 ros2 后端的 rpc 示例，演示内容包括：
- 如何在配置文件中加载**ros2_plugin**；
- 如何使用 ros2 类型的 rpc 后端；
- 如何配置 qos；


核心代码：
- [example_ros2/srv/RosTestRpc.srv](../../../protocols/example_ros2/srv/RosTestRpc.srv)
- [normal_rpc_co_client_module.cc](../../cpp/ros2_rpc/module/normal_rpc_co_client_module/normal_rpc_co_client_module.cc)
- [normal_rpc_co_server_module.cc](../../cpp/ros2_rpc/module/normal_rpc_co_server_module/normal_rpc_co_server_module.cc)
- [service.cc](../../cpp/ros2_rpc/module/normal_rpc_co_server_module/service.cc)


配置文件：
- [examples_plugins_ros2_plugin_ros2_rpc_server_with_qos_cfg.yaml](./install/linux/bin/cfg/examples_plugins_ros2_plugin_ros2_rpc_server_with_qos_cfg.yaml)
- [examples_plugins_ros2_plugin_ros2_rpc_client_with_qos_cfg.yaml](./install/linux/bin/cfg/examples_plugins_ros2_plugin_ros2_rpc_client_with_qos_cfg.yaml)


运行方式（linux）：
- 开启 `AIMRT_BUILD_EXAMPLES`、`AIMRT_BUILD_ROS2_PLUGIN` 选项编译 AimRT；
- 在终端运行 build 目录下`start_examples_plugins_ros2_plugin_ros2_rpc_server_with_qos.sh`脚本启动服务端（srv 进程）；
- 开启新的终端运行 build 目录下`start_examples_plugins_ros2_plugin_ros2_rpc_client_with_qos.sh`脚本启动客户端（cli 进程）；
- 分别在两个终端键入`ctrl-c`停止对应进程；


说明：
- 本示例与 **ros2 rpc** 示例基本一致，只是在启动时配置了通信 QOS；


## protobuf channel

一个基于 protobuf 协议与 ros2 后端的 channel 示例，演示内容包括：
- 如何在配置文件中加载**ros2_plugin**；
- 如何使用 ros2 类型的 channel 后端；
- 如何与原生 ros2 humble 节点进行 protobuf 协议的 topic 发布与订阅；



核心代码：
- [event.proto](../../../protocols/pb/example/event.proto)
- aimrt code:
  - [normal_publisher_module.cc](../../cpp/pb_chn/module/normal_publisher_module/normal_publisher_module.cc)
  - [normal_subscriber_module.cc](../../cpp/pb_chn/module/normal_subscriber_module/normal_subscriber_module.cc)
- native ros2 humble code:
  - [native_ros2_pb_chn_publisher/main.cc](./assistant/native_ros2_pb_chn_publisher/main.cc)
  - [native_ros2_pb_chn_subscriber/main.cc](./assistant/native_ros2_pb_chn_subscriber/main.cc)



配置文件：
- [examples_plugins_ros2_plugin_pb_chn_pub_cfg.yaml](./install/linux/bin/cfg/examples_plugins_ros2_plugin_pb_chn_pub_cfg.yaml)
- [examples_plugins_ros2_plugin_pb_chn_sub_cfg.yaml](./install/linux/bin/cfg/examples_plugins_ros2_plugin_pb_chn_sub_cfg.yaml)



运行方式（linux）：
- 开启 `AIMRT_BUILD_EXAMPLES`、`AIMRT_BUILD_ROS2_PLUGIN` 选项编译 AimRT；
- aimrt publisher 向 aimrt subscriber 发布数据：
  - 在终端运行 build 目录下`start_examples_plugins_ros2_plugin_pb_chn_sub.sh`脚本启动订阅端（sub 进程）；
  - 再开启一个新的终端窗口运行`start_examples_plugins_ros2_plugin_pb_chn_pub.sh`脚本启动发布端（pub 进程）；
  - 分别在两个终端键入`ctrl-c`停止对应进程；
- aimrt publisher 向 native ros2 subscriber 发布数据：
  - 在 build 目录下运行`start_examples_plugins_ros2_plugin_native_pb_chn_sub.sh`脚本以启动原生 ros2 订阅端（sub 进程）；
  - 再开启一个新的终端窗口运行`start_examples_plugins_ros2_plugin_pb_chn_pub.sh`脚本启动发布端（pub 进程）；
  - 分别在两个终端键入`ctrl-c`停止对应进程；
- native ros2 publisher 向 aimrt subscriber 发布数据：
  - 在终端运行 build 目录下`start_examples_plugins_ros2_plugin_pb_chn_sub.sh`脚本启动订阅端（sub 进程）；
  - 开启新的终端，在 build 目录下运行`start_examples_plugins_ros2_plugin_native_pb_chn_pub.sh`脚本以启动原生 ros2 发布端（pub 进程）；
  - 分别在两个终端键入`ctrl-c`停止对应进程；



说明：
- 此示例创建了以下两个模块：
  - `NormalPublisherModule`：会基于 `work_thread_pool` 执行器，以配置的频率、向配置的 topic 中发布 `ExampleEventMsg` 类型的消息；
  - `NormalSubscriberModule`：会订阅配置的 topic 下的 `ExampleEventMsg` 类型的消息；
- 此示例将 `NormalPublisherModule` 和 `NormalSubscriberModule` 分别集成到 `pb_chn_pub_pkg` 和 `pb_chn_sub_pkg` 两个 Pkg 中，并在两个配置文件中分别加载对应的 Pkg 到 pub 和 sub 进程中；
- 此示例加载了**ros2_plugin**，并使用 ros2 类型的 channel 后端进行通信；
- 此示例还创建了两个原生 ros2 节点，用于展示如何使用原生 ros2 程序与 aimrt 程序通过 ros2 后端进行 channel 通信；



## ros2 channel


一个基于 ros2 msg 协议与 ros2 后端的 channel 示例，演示内容包括：
- 如何在配置文件中加载**ros2_plugin**；
- 如何使用 ros2 类型的 channel 后端；
- 如何与原生 ros2 humble 节点进行 ros2 msg 协议的 topic 发布与订阅；


核心代码：
- [example_ros2/msg/RosTestMsg.msg](../../../protocols/ros2/example_ros2/msg/RosTestMsg.msg)
- aimrt code:
  - [normal_publisher_module.cc](../../cpp/ros2_chn/module/normal_publisher_module/normal_publisher_module.cc)
  - [normal_subscriber_module.cc](../../cpp/ros2_chn/module/normal_subscriber_module/normal_subscriber_module.cc)
- native ros2 humble code:
  - [native_ros2_chn_publisher/main.cc](./assistant/native_ros2_chn_publisher/main.cc)
  - [native_ros2_chn_subscriber/main.cc](./assistant/native_ros2_chn_subscriber/main.cc)


配置文件：
- [examples_plugins_ros2_plugin_ros2_chn_pub_cfg.yaml](./install/linux/bin/cfg/examples_plugins_ros2_plugin_ros2_chn_pub_cfg.yaml)
- [examples_plugins_ros2_plugin_ros2_chn_sub_cfg.yaml](./install/linux/bin/cfg/examples_plugins_ros2_plugin_ros2_chn_sub_cfg.yaml)



运行方式（linux）：
- 开启 `AIMRT_BUILD_EXAMPLES`、`AIMRT_BUILD_ROS2_PLUGIN` 选项编译 AimRT；
- aimrt publisher 向 aimrt subscriber 发布数据：
  - 在终端运行 build 目录下`start_examples_plugins_ros2_plugin_ros2_chn_sub.sh`脚本启动订阅端（sub 进程）；
  - 再开启一个新的终端窗口运行`start_examples_plugins_ros2_plugin_ros2_chn_pub.sh`脚本启动发布端（pub 进程）；
  - 分别在两个终端键入`ctrl-c`停止对应进程；
- aimrt publisher 向 native ros2 subscriber 发布数据：
  - 在 build 目录下运行`start_examples_plugins_ros2_plugin_native_ros2_chn_sub.sh`脚本以启动原生 ros2 订阅端（sub 进程）；
  - 再开启一个新的终端窗口运行`start_examples_plugins_ros2_plugin_ros2_chn_pub.sh`脚本启动发布端（pub 进程）；
  - 分别在两个终端键入`ctrl-c`停止对应进程；
- native ros2 publisher 向 aimrt subscriber 发布数据：
  - 在终端运行 build 目录下`start_examples_plugins_ros2_plugin_ros2_chn_sub.sh`脚本启动订阅端（sub 进程）；
  - 开启新的终端，在 build 目录下运行`start_examples_plugins_ros2_plugin_native_ros2_chn_pub.sh`脚本以启动原生 ros2 发布端（pub 进程）；
  - 分别在两个终端键入`ctrl-c`停止对应进程；




说明：
- 本示例与 **protobuf channel** 示例基本一致，除了业务层使用的是 ros2 msg 形式的协议；


## ros2 channel loaned message

一个使用 ROS2 loaned message 的 channel 示例，演示内容包括：
- 如何使用 `PublisherProxy::BorrowLoanedMessage()` 获取 backend-owned 消息并发布；
- 如何使用 callback-scoped `LoanedMessageView` 读取 subscriber 侧的 loaned 消息；
- 如何处理 borrow/subscribe/publish 返回的 `LoanStatus`；
- loaned channel 必须为每个方向配置唯一的 `ros2` backend；


核心代码：
- [RosLoanedMsg.msg](../../../protocols/ros2/example_ros2/msg/RosLoanedMsg.msg)
- [loaned_publisher_module.cc](../../cpp/ros2_chn/module/loaned_publisher_module/loaned_publisher_module.cc)
- [loaned_subscriber_module.cc](../../cpp/ros2_chn/module/loaned_subscriber_module/loaned_subscriber_module.cc)


配置文件：
- [examples_plugins_ros2_plugin_ros2_chn_loaned_cfg.yaml](./install/linux/bin/cfg/examples_plugins_ros2_plugin_ros2_chn_loaned_cfg.yaml)
- [examples_plugins_ros2_plugin_ros2_chn_loaned_pub_cfg.yaml](./install/linux/bin/cfg/examples_plugins_ros2_plugin_ros2_chn_loaned_pub_cfg.yaml)
- [examples_plugins_ros2_plugin_ros2_chn_loaned_sub_cfg.yaml](./install/linux/bin/cfg/examples_plugins_ros2_plugin_ros2_chn_loaned_sub_cfg.yaml)


运行方式（linux）：
- 开启 `AIMRT_BUILD_EXAMPLES`、`AIMRT_BUILD_ROS2_PLUGIN` 选项编译 AimRT；
- 确认当前 ROS2 环境安装了 `rmw_fastrtps_cpp`；
- 同进程演示：运行 build 目录下的 `start_examples_plugins_ros2_plugin_ros2_chn_loaned.sh`；
- 分进程演示：在两个终端分别运行 `start_examples_plugins_ros2_plugin_ros2_chn_loaned_sub.sh` 和 `start_examples_plugins_ros2_plugin_ros2_chn_loaned_pub.sh`，建议先启动 subscriber；
- 脚本会在进程启动前设置 `RMW_IMPLEMENTATION=rmw_fastrtps_cpp` 和 `ROS_DISABLE_LOANED_MESSAGES=0`；
- 键入 `ctrl-c` 停止进程。


说明：
- `RosLoanedMsg` 只包含固定大小的基础类型数组，满足 ROS2 plain/loanable message 的要求；
- publisher 必须检查 borrow 和 publish 的状态，失败时不能解引用无效 loan；
- subscriber 的 `LoanedMessageView` 只在同步 callback 返回前有效，不能保存到 callback 外部；
- 如果 backend 数量不唯一、RMW 不支持 loan、消息类型不满足要求或配置使用 serialized，接口会返回错误，不会自动退化成普通 pub/sub。


## ros2 channel with qos

一个基于 ros2 msg 协议与 ros2 后端的 channel 示例，演示内容包括：
- 如何在配置文件中加载**ros2_plugin**；
- 如何使用 ros2 类型的 channel 后端；
- 如何配置 qos；


核心代码：
- [example_ros2/msg/RosTestMsg.msg](../../../protocols/ros2/example_ros2/msg/RosTestMsg.msg)
- [normal_publisher_module.cc](../../cpp/ros2_chn/module/normal_publisher_module/normal_publisher_module.cc)
- [normal_subscriber_module.cc](../../cpp/ros2_chn/module/normal_subscriber_module/normal_subscriber_module.cc)


配置文件：
- [examples_plugins_ros2_plugin_ros2_chn_pub_with_qos_cfg.yaml](./install/linux/bin/cfg/examples_plugins_ros2_plugin_ros2_chn_pub_with_qos_cfg.yaml)
- [examples_plugins_ros2_plugin_ros2_chn_sub_with_qos_cfg.yaml](./install/linux/bin/cfg/examples_plugins_ros2_plugin_ros2_chn_sub_with_qos_cfg.yaml)


运行方式（linux）：
- 开启 `AIMRT_BUILD_EXAMPLES`、`AIMRT_BUILD_ROS2_PLUGIN` 选项编译 AimRT；
- 在终端运行 build 目录下`start_examples_plugins_ros2_plugin_ros2_chn_sub_with_qos.sh`脚本启动订阅端（sub 进程）；
- 再开启一个新的终端窗口运行`start_examples_plugins_ros2_plugin_ros2_chn_pub_with_qos.sh`脚本启动发布端（pub 进程）；
- 分别在两个终端键入`ctrl-c`停止对应进程；


说明：
- 本示例与 **ros2 channel** 示例基本一致，只是在启动时配置了通信 QOS；

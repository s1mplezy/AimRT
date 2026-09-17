# DDS 插件

## 概述

`dds_plugin` 基于 Fast DDS 提供 `dds` Channel 与 RPC backend。原生 IDL
类型直接使用生成的 DDS 业务类型传输；启用对应 AimRT 可选接口后，
Protobuf 与 ROS 2 类型使用有界 `aimrt::dds::SerializedMessage` wrapper。

参考代码：

- {{ '[DDS 插件]({}/src/plugins/dds_plugin)'.format(code_site_root_path_url) }}
- {{ '[DDS Channel 示例]({}/src/examples/cpp/dds_chn)'.format(code_site_root_path_url) }}
- {{ '[DDS RPC 示例]({}/src/examples/cpp/dds_rpc)'.format(code_site_root_path_url) }}
- {{ '[DDS 启动配置]({}/src/examples/plugins/dds_plugin)'.format(code_site_root_path_url) }}

首版面向 Linux 与 Fast DDS 3.6.x。每个 AimRT 进程只创建一个 DDS
participant、publisher 和 subscriber，并使用插件自有 Asio executor。

## 构建要求

DDS capability 与运行时插件默认分别通过 `AIMRT_BUILD_WITH_DDS=ON` 和
`AIMRT_BUILD_DDS_PLUGIN=ON` 启用。前者负责 Fast DDS、公共接口与代码生成，
后者只增加运行时 backend 并依赖前者。CMake 必须找到 Fast DDS 3.6.x 与 Java，
且 CMake 会从 `PATH` 自动查找 Fast DDS-Gen 4.3.0 的 `fastddsgen`。若有多个版本，
可通过 `FASTDDSGEN_EXECUTABLE` 显式指定其绝对路径：

```bash
cmake -B build \
  -DAIMRT_BUILD_WITH_DDS=ON \
  -DAIMRT_BUILD_DDS_PLUGIN=ON
cmake --build build --target aimrt_plugins_dds_plugin
```

`AIMRT_DDS_ENABLE_XTYPES` 默认为 `ON`。不需要 TypeObject 生成与 discovery
时可设为 `OFF`；该配置下原生 Channel 与 RPC 仍可使用。

使用 `aimrt_add_dds_idl_codegen` 在 build tree 内从 IDL 生成 Fast DDS 类型和
AimRT RPC 胶水代码：

```cmake
target_link_libraries(my_target PRIVATE aimrt::interface::aimrt_module_dds_interface aimrt::deps::fastdds)
aimrt_add_dds_idl_codegen(
  TARGET_NAME my_dds_codegen
  IDL_FILES Example.idl
  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
  ATTACH_TO_TARGET my_target)
```

## 插件配置

| 配置项 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `domain_id` | 无符号整数 | `0` | 当前进程使用的 DDS domain。 |
| `participant_name` | 字符串 | 可执行文件 basename | DDS participant 名称。 |
| `fastdds_xml` | 字符串 | 空 | Fast DDS XML profile 文件。 |
| `executor.type` | 字符串 | `asio_thread` | 只接受 `asio_thread`。 |
| `executor.thread_num` | 无符号整数 | `0` | worker 数；`0` 自动选择且实际至少为 2。 |

```yaml
aimrt:
  plugin:
    plugins:
      - name: dds_plugin
        path: ./libaimrt_dds_plugin.so
        options:
          domain_id: 21
          participant_name: navigation
          executor:
            type: asio_thread
            thread_num: 4
  channel:
    backends:
      - type: dds
    pub_topics_options:
      - topic_name: "(.*)"
        enable_backends: [dds]
    sub_topics_options:
      - topic_name: "(.*)"
        enable_backends: [dds]
  rpc:
    backends:
      - type: dds
    clients_options:
      - func_name: "(.*)"
        enable_backends: [dds]
    servers_options:
      - func_name: "(.*)"
        enable_backends: [dds]
```

一个进程只能拥有一个 DDS domain。第二个 context 若使用不同 domain 或
participant name，初始化会被拒绝。

## 类型、Topic 与 QoS

原生 IDL 的 AimRT 类型名为 `dds:<IDL 全限定名>`，例如
`dds:example::Pose`。Channel 直接使用注册的 AimRT topic 名。RPC function
映射为 `req/<rpc-type>/<service>/<method>` 与
`rsp/<rpc-type>/<service>/<method>`；DDS RPC 不支持 remap。

没有 XML default profile 时，Channel 使用 best-effort、volatile、keep-last
depth 20；RPC 使用 reliable、volatile、keep-all。RPC request writer 的
`max_blocking_time` 固定为 0，XML 若改变该值会导致初始化失败。数据表示固定为 XCDR2。

配置 `fastdds_xml` 后，插件对每种 entity 最多选择一个
`is_default_profile="true"` profile。选中的 XML 是完整 profile，不与插件默认值
做字段级合并；XML 不能覆盖 XCDR2。文件缺失、语法错误、重复 default、QoS
不兼容或 profile 选择不明确都会使初始化失败。

原生 plain XCDR2 类型支持 publisher/subscriber loan；Protobuf 与 ROS 2
wrapper 不支持 loan。原生 IDL Channel 不在线上传输 AimRT Context metadata，
订阅端只重建本地可知的 backend、serialization 和 endpoint 字段。wrapper
Channel metadata 有固定边界，wrapper RPC metadata 必须为空。

RPC 通过 DDS `SampleIdentity` 关联响应。server 退出后不会自动重试请求，
远端 AimRT Context、timeout 与 Status 不进入 wire。多个 server 同时匹配时，
首个合法响应完成 client 调用，但业务可能在多个 server 上执行。

## XTypes 与诊断

启用 XTypes 时，生成的原生 IDL `TypeIdentifier`/`TypeObject` 在 DDS 类型注册
阶段注册，远端 participant 可通过 discovery 与 TypeLookup 获取。wrapper 只
暴露 wrapper schema，不会把 Protobuf 或 ROS 2 原业务 schema 声明为可反射。

AimRT 初始化报告包含 participant/domain、XML 来源、各 entity 的有效 QoS
来源、executor 线程数、Channel/RPC QoS、runtime reader/writer 数以及每个 RPC
client 的 content-filter 所有权。运行期日志与计数覆盖匹配、QoS 不兼容、样本
丢失/拒绝、deadline/liveliness、write/take 失败、迟到/重复 RPC response、
pending 上限与 shutdown completion。

回调应保持轻量，耗时任务应转交业务 executor。Channel 投递、RPC handler、
RPC completion、retry timer 与 shutdown drain 共用插件自有 executor。

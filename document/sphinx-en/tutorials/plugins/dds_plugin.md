# DDS Plugin

## Overview

`dds_plugin` provides `dds` Channel and RPC backends on Fast DDS. Native IDL
types are sent as their generated DDS business types. Protobuf and ROS 2 types,
when those optional AimRT interfaces are enabled, use the bounded
`aimrt::dds::SerializedMessage` wrapper.

Reference code:

- {{ '[DDS plugin]({}/src/plugins/dds_plugin)'.format(code_site_root_path_url) }}
- {{ '[DDS Channel example]({}/src/examples/cpp/dds_chn)'.format(code_site_root_path_url) }}
- {{ '[DDS RPC example]({}/src/examples/cpp/dds_rpc)'.format(code_site_root_path_url) }}
- {{ '[DDS launch configuration]({}/src/examples/plugins/dds_plugin)'.format(code_site_root_path_url) }}

The first release targets Linux and Fast DDS 3.6.x. It creates one DDS
participant, publisher, and subscriber per AimRT process and uses a
plugin-owned Asio executor.

## Build requirements

DDS capability and the runtime plugin are enabled by default with
`AIMRT_BUILD_WITH_DDS=ON` and `AIMRT_BUILD_DDS_PLUGIN=ON`. The capability
switch owns Fast DDS, public interfaces, and code generation; the plugin switch
adds the runtime backend and requires the capability switch. CMake must find
Fast DDS 3.6.x and Java. CMake automatically finds Fast DDS-Gen 4.3.0's
`fastddsgen` on `PATH`; if multiple versions are available, set
`FASTDDSGEN_EXECUTABLE` to the desired absolute path:

```bash
cmake -B build \
  -DAIMRT_BUILD_WITH_DDS=ON \
  -DAIMRT_BUILD_DDS_PLUGIN=ON
cmake --build build --target aimrt_plugins_dds_plugin
```

`AIMRT_DDS_ENABLE_XTYPES` defaults to `ON`. Set it to `OFF` when TypeObject
generation and discovery are not needed. Native Channel and RPC remain
available in that build.

Use `aimrt_add_dds_idl_codegen` to generate the Fast DDS types and AimRT RPC
glue from IDL in the build tree:

```cmake
target_link_libraries(my_target PRIVATE aimrt::interface::aimrt_module_dds_interface aimrt::deps::fastdds)
aimrt_add_dds_idl_codegen(
  TARGET_NAME my_dds_codegen
  IDL_FILES Example.idl
  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
  ATTACH_TO_TARGET my_target)
```

## Plugin configuration

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `domain_id` | unsigned integer | `0` | DDS domain used by the process. |
| `participant_name` | string | executable basename | DDS participant name. |
| `fastdds_xml` | string | empty | Fast DDS XML profiles file. |
| `executor.type` | string | `asio_thread` | Only `asio_thread` is accepted. |
| `executor.thread_num` | unsigned integer | `0` | Worker count; `0` selects an automatic value of at least two. |

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

Only one DDS domain may be owned by a process. Loading two contexts with
different domains or participant names is rejected.

## Types, topics, and QoS

Native IDL AimRT type names use `dds:<fully-qualified IDL name>`, for example
`dds:example::Pose`. Channel topics use the registered AimRT topic name
directly. RPC functions map to `req/<rpc-type>/<service>/<method>` and
`rsp/<rpc-type>/<service>/<method>`; DDS RPC remapping is not supported.

Without XML defaults, Channel uses best-effort, volatile, keep-last depth 20.
RPC uses reliable, volatile, keep-all. RPC request writers always use a zero
`max_blocking_time`; an XML profile that changes this value is rejected.
Data representation is always XCDR2.

When `fastdds_xml` is set, the plugin selects at most one
`is_default_profile="true"` profile for each entity kind. A selected XML
profile is authoritative as a complete profile; fields are not merged with
plugin defaults. XML cannot override XCDR2. Initialization fails on missing,
malformed, duplicate-default, incompatible, or ambiguous profiles.

Native plain XCDR2 types can use publisher and subscriber loan APIs. Protobuf
and ROS 2 wrappers do not support loan. Native IDL Channel context metadata is
not transmitted on the DDS wire; subscribers reconstruct only locally known
backend, serialization, and endpoint fields. Wrapper Channel metadata is
bounded, while wrapper RPC metadata is required to be empty.

RPC responses are correlated with DDS `SampleIdentity`. Calls are not retried
automatically when a server exits, and remote AimRT context, timeout, and
status are not carried on the wire. If multiple servers match, the first valid
response completes the client call; business execution can occur on more than
one server.

## XTypes and diagnostics

With XTypes enabled, generated native IDL `TypeIdentifier` and `TypeObject`
information is registered during DDS type registration and can be obtained by
remote participants through discovery and TypeLookup. Wrapper types expose
only the wrapper schema, not the original Protobuf or ROS 2 schema.

The AimRT initialization report contains the participant/domain, XML source,
effective entity QoS sources, executor thread count, Channel and RPC QoS,
runtime reader/writer counts, and per-client RPC content-filter ownership.
Runtime logs and counters cover matches, incompatible QoS, sample loss or
rejection, deadline/liveliness violations, write/take failures, late or
duplicate RPC responses, pending limits, and shutdown completions.

Keep callbacks short or move heavy work to an application executor. Channel
delivery, RPC handlers, RPC completions, retry timers, and shutdown draining
share the plugin-owned executor.

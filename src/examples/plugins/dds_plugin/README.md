# DDS plugin examples

These launch files select the `dds` Channel and RPC backends and reuse the
business modules and packages from `src/examples/cpp/dds_chn` and
`src/examples/cpp/dds_rpc`. Protocol definitions are owned by
`src/protocols/dds/example`; this directory intentionally contains no IDL or
business source code.

Configure with `AIMRT_BUILD_EXAMPLES=ON`, `AIMRT_BUILD_WITH_DDS=ON`, and
`AIMRT_BUILD_DDS_PLUGIN=ON`, then build `all`. Run each pair from the build
directory in separate terminals:

```bash
./start_examples_plugins_dds_plugin_dds_chn_sub.sh
./start_examples_plugins_dds_plugin_dds_chn_pub.sh

./start_examples_plugins_dds_plugin_dds_rpc_server.sh
./start_examples_plugins_dds_plugin_dds_rpc_client.sh
```

Use Ctrl-C to stop each process. The Channel subscriber prints
`DDS_CHANNEL_RECEIVED`; the RPC client prints `DDS_RPC_RESPONSE` after a
successful native-IDL exchange.

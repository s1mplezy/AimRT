# DDS RPC example

This example uses the generated `aimrt_examples::dds::Calculator` API from the
public DDS example IDL. The sync client and server are reusable AimRT modules
packaged into separate shared libraries. DDS plugin configuration and launch
scripts are in `src/examples/plugins/dds_plugin`.

Build with `AIMRT_BUILD_EXAMPLES=ON`, `AIMRT_BUILD_WITH_DDS=ON`, and
`AIMRT_BUILD_DDS_PLUGIN=ON`. From the build directory, start the server and
then the client:

```bash
./start_examples_plugins_dds_plugin_dds_rpc_server.sh
./start_examples_plugins_dds_plugin_dds_rpc_client.sh
```

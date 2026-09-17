# DDS Channel example

This example publishes and subscribes to the native IDL type
`aimrt_examples::dds::ChannelMessage`. The reusable publisher/subscriber modules
and their separate packages live in this directory. Runtime DDS backend
configuration and launch scripts are in `src/examples/plugins/dds_plugin`.

Build with `AIMRT_BUILD_EXAMPLES=ON`, `AIMRT_BUILD_WITH_DDS=ON`, and
`AIMRT_BUILD_DDS_PLUGIN=ON`. From the build directory, start the subscriber and
then the publisher:

```bash
./start_examples_plugins_dds_plugin_dds_chn_sub.sh
./start_examples_plugins_dds_plugin_dds_chn_pub.sh
```

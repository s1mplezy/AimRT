// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "aimrt_pkg_c_interface/pkg_macro.h"
#include "normal_rpc_sync_client_module/normal_rpc_sync_client_module.h"

static std::tuple<std::string_view, std::function<aimrt::ModuleBase*()>> aimrt_module_register_array[]{
    {"DdsNormalRpcSyncClientModule", []() -> aimrt::ModuleBase* {
       return new aimrt::examples::cpp::dds_rpc::normal_rpc_sync_client_module::NormalRpcSyncClientModule();
     }}};

AIMRT_PKG_MAIN(aimrt_module_register_array)

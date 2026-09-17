// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#include "Calculator.h"

#include <string_view>

int main() {
  const auto* request = aimrt::GetDdsMessageTypeSupport<example::AddRequest>();
  const auto* response = aimrt::GetDdsMessageTypeSupport<example::AddResponse>();
  if (aimrt::util::ToStdStringView(request->type_name(request->impl)) != "dds:example::AddRequest") return 1;
  if (aimrt::util::ToStdStringView(response->type_name(response->impl)) != "dds:example::AddResponse") return 2;
  example::CalculatorSyncService service;
  if (service.RpcType() != "dds") return 3;
  return service.ServiceName() == "example::Calculator" ? 0 : 4;
}

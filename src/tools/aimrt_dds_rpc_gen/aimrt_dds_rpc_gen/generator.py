"""Generate AimRT C++ DDS RPC glue from a validated IDL AST."""

from __future__ import annotations

from pathlib import Path

from .model import InterfaceDecl, Operation, StructDecl, TranslationUnit, TypeRef


def _qualified(parts: tuple[str, ...] | list[str]) -> str:
    return "::" + "::".join(parts)


def _resolve_cpp_type(type_ref: TypeRef, scope: tuple[str, ...], unit: TranslationUnit) -> str:
    spelling = type_ref.spelling
    if spelling.startswith("::"):
        return spelling
    struct_names = {declaration.fqn for declaration in unit.structs}
    for prefix_length in range(len(scope), -1, -1):
        candidate = "::".join((*scope[:prefix_length], spelling))
        if candidate in struct_names:
            return "::" + candidate
    raise AssertionError(f"validated IDL type no longer resolves: {spelling}")


def _open_namespace(lines: list[str], scope: tuple[str, ...]) -> None:
    for name in scope:
        lines.append(f"namespace {name} {{")
    if scope:
        lines.append("")


def _close_namespace(lines: list[str], scope: tuple[str, ...]) -> None:
    if scope:
        lines.append("")
    for name in reversed(scope):
        lines.append(f"}} // namespace {name}")


def _method_declaration(operation: Operation, interface: InterfaceDecl, unit: TranslationUnit, style: str) -> list[str]:
    request = _resolve_cpp_type(operation.parameters[0].type_ref, interface.scope, unit)
    response = _resolve_cpp_type(operation.response_type, interface.scope, unit)
    args = ["      aimrt::rpc::ContextRef ctx_ref,", f"      const {request}& req, ", f"      {response}& rsp"]
    if style == "sync":
        return [f"  virtual aimrt::rpc::Status {operation.name}(",
                *args,
                "  ) {",
                "    return aimrt::rpc::Status(AIMRT_RPC_STATUS_SVR_NOT_IMPLEMENTED);",
                "  }"]
    if style == "async":
        return [f"  virtual void {operation.name}(",
                *args[:-1],
                args[-1] + ",",
                "      std::function<void(aimrt::rpc::Status)>&& callback) {",
                "    callback(aimrt::rpc::Status(AIMRT_RPC_STATUS_SVR_NOT_IMPLEMENTED));",
                "  }"]
    return [f"  virtual aimrt::co::Task<aimrt::rpc::Status> {operation.name}(",
            *args,
            "  ) {",
            "    co_return aimrt::rpc::Status(AIMRT_RPC_STATUS_SVR_NOT_IMPLEMENTED);",
            "  }"]


def _proxy_method_declarations(
        operation: Operation,
        interface: InterfaceDecl,
        unit: TranslationUnit,
        style: str) -> list[str]:
    request = _resolve_cpp_type(operation.parameters[0].type_ref, interface.scope, unit)
    response = _resolve_cpp_type(operation.response_type, interface.scope, unit)
    if style == "sync":
        return [
            f"  aimrt::rpc::Status {operation.name}(aimrt: : rpc: : ContextRef ctx_ref, const {request}& req, {response}& rsp)
        ",
            f"  aimrt: :rpc::Status {operation.name}(const {request}& req, {response}& rsp) {{",
            f"    return {operation.name}(aimrt: :rpc::ContextRef(), req, rsp);",
            "  }",
        ]
    if style == "async":
        return [
            f"  void {operation.name}(aimrt: :rpc::ContextRef ctx_ref, const {request}& req, {response}& rsp,",
            "      std::function<void(aimrt::rpc::Status)>&& callback);",
            f"  void {operation.name}(const {request}& req, {response}& rsp, ",
            "      std::function<void(aimrt::rpc::Status)>&& callback) {",
            f"    {operation.name}(aimrt::rpc::ContextRef(), req, rsp, std::move(callback))
        ",
            "  }",
        ]
    if style == "future":
        return [
            f"  std::future<aimrt::rpc::Status> {operation.name}(aimrt::rpc::ContextRef ctx_ref, const {request}& req, {response}& rsp)
        ",
            f"  std: :future<aimrt::rpc::Status> {operation.name}(const {request}& req, {response}& rsp) {{",
            f"    return {operation.name}(aimrt: :rpc::ContextRef(), req, rsp);",
            "  }",
        ]
    return [
        f"  aimrt::co::Task<aimrt::rpc::Status> {operation.name}(aimrt::rpc::ContextRef ctx_ref, const {request}& req, {response}& rsp)
    ",
        f"  aimrt: :co::Task<aimrt::rpc::Status> {operation.name}(const {request}& req, {response}& rsp) {{",
        f"    co_return co_await {operation.name}(aimrt: :rpc::ContextRef(), req, rsp);",
        "  }",
    ]


def _type_support_specializations(lines: list[str], structs: list[StructDecl]) -> None:
    if not structs:
        return
    lines.extend(["namespace aimrt {", ""])
    for declaration in structs:
        cpp_type = _qualified((*declaration.scope, declaration.name))
        pubsub_type = _qualified((*declaration.scope, f"{declaration.name}PubSubType"))
        lines.extend(
            [
                "template <>",
                f"struct DdsTypeSupportTraits<{cpp_type}> {{",
                f"  using PubSubType = {pubsub_type}
        ",
                f'  static constexpr std: :string_view TypeName() {{ return "{declaration.fqn}"; }}',
                "};",
                "",
            ]
        )
    if lines[-1] == "":
        lines.pop()
    lines.extend(["", "}  // namespace aimrt", "", ""])


def generate_header(
    unit: TranslationUnit,
    idl_stem: str,
    binding_structs: list[StructDecl],
    included_stems: list[str],
) -> str:
    lines = [
        "/**",
        f" * @file {idl_stem}.h",
        " * @brief Generated by aimrt_dds_rpc_gen. Do not edit.",
        " */",
        "#pragma once",
        "",
        "#include <future>",
        "#include <functional>",
        "#include <string_view>",
        "#include <utility>",
        "",
        '#include "aimrt_module_cpp_interface/co/task.h"',
        '#include "aimrt_module_cpp_interface/rpc/rpc_handle.h"',
        '#include "aimrt_module_cpp_interface/rpc/rpc_status.h"',
        '#include "aimrt_module_dds_interface/util/dds_type_support.h"',
        f'#include "detail/{idl_stem}/{idl_stem}.hpp"',
        f'#include "detail/{idl_stem}/{idl_stem}PubSubTypes.hpp"',
        "",
    ]
    for included_stem in included_stems:
        lines.append(f'#include "{included_stem}.h"')
    if included_stems:
        lines.append("")
    _type_support_specializations(lines, binding_structs)
    for interface in unit.interfaces:
        _open_namespace(lines, interface.scope)
        for class_name, base, style in (
            (f"{interface.name}SyncService", "aimrt::rpc::ServiceBase", "sync"),
            (f"{interface.name}AsyncService", "aimrt::rpc::ServiceBase", "async"),
            (f"{interface.name}CoService", "aimrt::rpc::CoServiceBase", "co"),
        ):
            lines.extend([f"class {class_name}: public {base} {{", " public:",
                         f"  {class_name}()
            ", f"  ~{class_name}() override = default;", ""])
            for operation in interface.operations:
                lines.extend(_method_declaration(operation, interface, unit, style))
                lines.append("")
            if lines[-1] == "":
                lines.pop()
            lines.extend(["};", ""])
        lines.extend(
            [
                f"bool Register{
                    interface.name}ClientFunc(aimrt::rpc::RpcHandleRef rpc_handle_ref, std::string_view service_name)
        ",
                f"bool Register{
                    interface.name}ClientFunc(aimrt: :rpc::RpcHandleRef rpc_handle_ref);",
                "",
            ])
        for suffix, base, style in (
            ("SyncProxy", "aimrt::rpc::SyncProxyBase", "sync"),
            ("AsyncProxy", "aimrt::rpc::AsyncProxyBase", "async"),
            ("FutureProxy", "aimrt::rpc::FutureProxyBase", "future"),
            ("CoProxy", "aimrt::rpc::CoProxyBase", "co"),
        ):
            class_name = f"{interface.name}{suffix}"
            lines.extend(
                [
                    f"class {class_name}: public {base} {{",
                    " public:",
                    f"  explicit {class_name}(aimrt::rpc::RpcHandleRef rpc_handle_ref)
            ",
                    f"  {class_name}(aimrt: :rpc::RpcHandleRef rpc_handle_ref, std::string_view service_name);",
                    f"  ~{class_name}() = default; ",
                    "",
                    "  static bool RegisterClientFunc(aimrt::rpc::RpcHandleRef rpc_handle_ref) {",
                    f"    return Register{interface.name}ClientFunc(rpc_handle_ref); ",
                    "  }",
                    "  static bool RegisterClientFunc(aimrt::rpc::RpcHandleRef rpc_handle_ref, std::string_view service_name) {",
                    f"    return Register{interface.name}ClientFunc(rpc_handle_ref, service_name); ",
                    "  }",
                    "",
                ]
            )
            for operation in interface.operations:
                lines.extend(_proxy_method_declarations(operation, interface, unit, style))
                lines.append("")
            if lines[-1] == "":
                lines.pop()
            lines.extend(["};", ""])
        if lines[-1] == "":
            lines.pop()
        _close_namespace(lines, interface.scope)
        lines.extend(["", ""])
    return "\n".join(lines).rstrip() + "\n"


def _key(interface: InterfaceDecl) -> str:
    # Length-prefix every IDL component so legal scopes such as a_b::C and
    # a::b::C can never collapse to the same file-scope C++ identifier.
    return "_".join(f"{len(component)}_{component}" for component in (*interface.scope, interface.name))


def _service_constructor(lines: list[str], interface: InterfaceDecl, unit: TranslationUnit, style: str) -> None:
    class_name = f"{
        interface.name}{
        'SyncService' if style == 'sync' else 'AsyncService' if style == 'async' else 'CoService'}"
    base = "ServiceBase" if style != "co" else "CoServiceBase"
    key = _key(interface)
    lines.append(f"{class_name}: :{class_name}() : aimrt::rpc::{base}(kRpcType, kServiceName_{key}) {{")
    for operation in interface.operations:
        request = _resolve_cpp_type(operation.parameters[0].type_ref, interface.scope, unit)
        response = _resolve_cpp_type(operation.response_type, interface.scope, unit)
        lines.append("  aimrt::rpc::ServiceFunc service_callback(")
        lines.append(
            "      [this](const aimrt_rpc_context_base_t* ctx, const void* req, void* rsp, aimrt_function_base_t* result_callback_ptr) {")
        if style == "sync":
            lines.extend(
                [
                    "        aimrt::rpc::ServiceCallback result_callback(result_callback_ptr);",
                    f"        auto status = {
                        operation.name}(aimrt::rpc::ContextRef(ctx), *static_cast<const {request}*>(req), *static_cast<{response}*>(rsp))
            ",
                    "        result_callback(status.Code());",
                ])
        elif style == "async":
            lines.extend(
                [
                    "        auto result_callback = std::make_shared<aimrt::rpc::ServiceCallback>(result_callback_ptr);",
                    f"        {
                        operation.name}(aimrt: :rpc::ContextRef(ctx), *static_cast<const {request}*>(req), *static_cast<{response}*>(rsp),",
                    "            [result_callback{std::move(result_callback)}](aimrt::rpc::Status status) { (*result_callback)(status.Code()); });",
                ])
        else:
            lines.extend(
                [
                    "        auto handle = std::make_unique<const aimrt::rpc::CoRpcHandle>(",
                    f"            [this](aimrt::rpc::ContextRef context, const void* request, void* response) {{ return {operation.name}(context, *static_cast<const {request}*>(request), *static_cast<{response}*>(response))
            }});",
                    "        aimrt::rpc::ServiceCallback result_callback(result_callback_ptr);",
                    "        auto* handle_ptr = handle.get();",
                    "        aimrt::co::StartDetached(aimrt::co::On(aimrt::co::InlineScheduler(), filter_mgr_.InvokeRpc(*handle_ptr, aimrt::rpc::ContextRef(ctx), req, rsp)) |",
                    "            aimrt::co::Then([handle{std::move(handle)}, result_callback{std::move(result_callback)}](aimrt::rpc::Status status) mutable { result_callback(status.Code()); }));",
                ]
            )
        lines.extend(
            [
                "      });",
                f"  RegisterServiceFunc(\"{operation.name}\", nullptr, aimrt: :GetDdsMessageTypeSupport<{request}>(),",
                f"                      aimrt::GetDdsMessageTypeSupport<{response}>(), std::move(service_callback))
        ",
            ]
        )
    lines.extend(["}", ""])


def generate_source(unit: TranslationUnit, idl_stem: str) -> str:
    lines = [
        "/**",
        f" * @file {idl_stem}.cc",
        " * @brief Generated by aimrt_dds_rpc_gen. Do not edit.",
        " */",
        f'#include "{idl_stem}.h"',
        "",
        "#include <memory>",
        "",
        '#include "aimrt_module_cpp_interface/co/inline_scheduler.h"',
        '#include "aimrt_module_cpp_interface/co/on.h"',
        '#include "aimrt_module_cpp_interface/co/start_detached.h"',
        '#include "aimrt_module_cpp_interface/co/then.h"',
        '#include "aimrt_module_dds_interface/util/dds_type_support.h"',
        "",
        "static constexpr std::string_view kRpcType = \"dds\";",
        "",
    ]
    for interface in unit.interfaces:
        key = _key(interface)
        service_name = "::".join((*interface.scope, interface.name))
        lines.append(f"static constexpr std::string_view kServiceName_{key} = \"{service_name}\"
        ")
    lines.append("")
    for interface in unit.interfaces:
        _open_namespace(lines, interface.scope)
        for style in ("sync", "async", "co"):
            _service_constructor(lines, interface, unit, style)
        key = _key(interface)
        lines.append(
            f"bool Register{
                interface.name}ClientFunc(aimrt: :rpc::RpcHandleRef rpc_handle_ref, std::string_view service_name) {{")
        lines.append("  bool result = true;")
        for operation in interface.operations:
            request = _resolve_cpp_type(operation.parameters[0].type_ref, interface.scope, unit)
            response = _resolve_cpp_type(operation.response_type, interface.scope, unit)
            lines.extend(
                [
                    "  result = rpc_handle_ref.RegisterClientFunc(",
                    f"      kRpcType, service_name, \"{
                        operation.name}\", nullptr, aimrt: :GetDdsMessageTypeSupport<{request}>(),",
                    f"      aimrt::GetDdsMessageTypeSupport<{response}>()) && result
            ",
                ])
        lines.extend(["  return result;",
                      "}",
                      "",
                      f"bool Register{interface.name}ClientFunc(aimrt: :rpc::RpcHandleRef rpc_handle_ref) {{",
                      f"  return Register{interface.name}ClientFunc(rpc_handle_ref, kServiceName_{key})
        ",
                      "}",
                      ""])
        for suffix, base, style in (
            ("SyncProxy", "SyncProxyBase", "sync"),
            ("AsyncProxy", "AsyncProxyBase", "async"),
            ("FutureProxy", "FutureProxyBase", "future"),
            ("CoProxy", "CoProxyBase", "co"),
        ):
            class_name = f"{interface.name}{suffix}"
            lines.extend(
                [
                    f"{class_name}: :{class_name}(aimrt::rpc::RpcHandleRef rpc_handle_ref)",
                    f"    : aimrt: :rpc::{base}(rpc_handle_ref, kRpcType, kServiceName_{key}) {{}}",
                    f"{class_name}: :{class_name}(aimrt::rpc::RpcHandleRef rpc_handle_ref, std::string_view service_name)",
                    f"    : aimrt: :rpc::{base}(rpc_handle_ref, kRpcType, service_name) {{}}",
                    "",
                ]
            )
            for operation in interface.operations:
                request = _resolve_cpp_type(operation.parameters[0].type_ref, interface.scope, unit)
                response = _resolve_cpp_type(operation.response_type, interface.scope, unit)
                if style == "sync":
                    lines.extend(
                        [
                            f"aimrt::rpc::Status {class_name}: :{
                                operation.name}(aimrt: :rpc::ContextRef ctx_ref, const {request}& req, {response}& rsp) {{",
                            f"  return Invoke(aimrt: :rpc::GetFullFuncName(rpc_type_, service_name_, \"{
                                operation.name}\"), ctx_ref, req, rsp)
                    ",
                            "}"])
                elif style == "async":
                    lines.extend(
                        [
                            f"void {class_name}: :{
                                operation.name}(aimrt: :rpc::ContextRef ctx_ref, const {request}& req, {response}& rsp,",
                            "    std::function<void(aimrt::rpc::Status)>&& callback) {",
                            f"  Invoke(aimrt: :rpc::GetFullFuncName(rpc_type_, service_name_, \"{
                                operation.name}\"), ctx_ref, req, rsp, std::move(callback))
                    ",
                            "}"])
                elif style == "future":
                    lines.extend(
                        [
                            f"std::future<aimrt::rpc::Status> {class_name}: :{
                                operation.name}(aimrt: :rpc::ContextRef ctx_ref, const {request}& req, {response}& rsp) {{",
                            f"  return Invoke(aimrt: :rpc::GetFullFuncName(rpc_type_, service_name_, \"{
                                operation.name}\"), ctx_ref, req, rsp)
                    ",
                            "}"])
                else:
                    lines.extend(
                        [
                            f"aimrt::co::Task<aimrt::rpc::Status> {class_name}: :{
                                operation.name}(aimrt: :rpc::ContextRef ctx_ref, const {request}& req, {response}& rsp) {{",
                            f"  co_return co_await Invoke(aimrt: :rpc::GetFullFuncName(rpc_type_, service_name_, \"{
                                operation.name}\"), ctx_ref, req, rsp)
                    ",
                            "}"])
                lines.append("")
        if lines[-1] == "":
            lines.pop()
        _close_namespace(lines, interface.scope)
        lines.extend(["", ""])
    return "\n".join(lines).rstrip() + "\n"


def generate(unit: TranslationUnit, idl_path: str) -> tuple[str, str, str]:
    root = Path(idl_path).resolve()
    stem = root.stem
    binding_structs = [declaration for declaration in unit.structs if Path(declaration.location.path) == root]
    included_stems = [Path(path).stem for path in unit.source_paths if Path(path) != root]
    root_unit = TranslationUnit(
        source_paths=unit.source_paths,
        structs=unit.structs,
        interfaces=[declaration for declaration in unit.interfaces if Path(declaration.location.path) == root],
        exceptions=unit.exceptions,
    )
    return stem, generate_header(root_unit, stem, binding_structs, included_stems), generate_source(root_unit, stem)

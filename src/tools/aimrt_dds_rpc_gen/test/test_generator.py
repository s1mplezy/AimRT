from __future__ import annotations

import contextlib
import io
from pathlib import Path
import tempfile
import time
import unittest

from aimrt_dds_rpc_gen.cli import main
from aimrt_dds_rpc_gen.diagnostics import IdlDiagnostic
from aimrt_dds_rpc_gen.loader import load_idl


VALID_IDL = """
module example {
  struct AddRequest { long lhs; long rhs; };
  struct AddResponse { long sum; };
  interface Calculator {
    AddResponse Add(in AddRequest request);
  };
};
"""


class GeneratorTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write(self, name: str, content: str) -> Path:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def run_cli(self, path: Path, *extra: str) -> tuple[int, str]:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            result = main(["--idl", str(path.resolve()), "--output-dir", str(self.root / "out"), *extra])
        return result, stderr.getvalue()

    def test_positive_generation_uses_existing_rpc_shapes_and_null_function_support(self) -> None:
        path = self.write("Calculator.idl", VALID_IDL)
        result, stderr = self.run_cli(path)
        self.assertEqual(0, result, stderr)
        header = (self.root / "out/Calculator.h").read_text(encoding="utf-8")
        source = (self.root / "out/Calculator.cc").read_text(encoding="utf-8")
        self.assertIn('#include "detail/Calculator/Calculator.hpp"', header)
        self.assertIn("class CalculatorSyncService : public aimrt::rpc::ServiceBase", header)
        self.assertIn("class CalculatorAsyncProxy : public aimrt::rpc::AsyncProxyBase", header)
        self.assertIn("class CalculatorFutureProxy : public aimrt::rpc::FutureProxyBase", header)
        self.assertIn("class CalculatorCoProxy : public aimrt::rpc::CoProxyBase", header)
        self.assertIn('kRpcType = "dds"', source)
        self.assertIn('kServiceName_7_example_10_Calculator = "example::Calculator"', source)
        self.assertIn('RegisterServiceFunc("Add", nullptr', source)
        self.assertIn("aimrt::GetDdsMessageTypeSupport<::example::AddRequest>()", source)
        self.assertIn("aimrt::GetDdsMessageTypeSupport<::example::AddResponse>()", source)

    def test_includes_are_parsed_and_recorded_in_depfile(self) -> None:
        self.write("types.idl", "module example { struct Req { long x; }; struct Rsp { long y; }; };\n")
        root = self.write(
            "Service.idl",
            '#include "types.idl"\nmodule example { interface Service { Rsp Call(in Req request); }; };\n',
        )
        depfile = self.root / "out/Service.aimrt_rpc.d"
        result, stderr = self.run_cli(root, "--include-dir", str(self.root), "--depfile", str(depfile))
        self.assertEqual(0, result, stderr)
        dependencies = depfile.read_text(encoding="utf-8")
        self.assertIn(str(root.resolve()), dependencies)
        self.assertIn(str((self.root / "types.idl").resolve()), dependencies)
        for name in ("types.h", "types.cc"):
            self.assertTrue((self.root / "out" / name).is_file(), name)

    def test_included_interfaces_are_not_regenerated(self) -> None:
        self.write(
            "shared.idl",
            "module example { struct SharedReq { long x; }; struct SharedRsp { long y; }; "
            "interface Shared { SharedRsp Call(in SharedReq request); }; };\n",
        )
        root = self.write(
            "Root.idl",
            '#include "shared.idl"\nmodule example { interface Root { SharedRsp Call(in SharedReq request); }; };\n',
        )
        result, stderr = self.run_cli(root, "--include-dir", str(self.root))
        self.assertEqual(0, result, stderr)
        header = (self.root / "out/Root.h").read_text(encoding="utf-8")
        self.assertIn("class RootSyncService", header)
        self.assertNotIn("class SharedSyncService", header)

    def test_relative_qualified_types_use_nearest_lexical_scope(self) -> None:
        root = self.write(
            "Scoped.idl",
            "module common { struct Req { long x; }; struct Rsp { long y; }; }; "
            "module outer { module common { struct Req { long x; }; struct Rsp { long y; }; }; "
            "interface Service { common::Rsp Call(in common::Req request); }; };\n",
        )
        result, stderr = self.run_cli(root)
        self.assertEqual(0, result, stderr)
        source = (self.root / "out/Scoped.cc").read_text(encoding="utf-8")
        self.assertIn("GetDdsMessageTypeSupport<::outer::common::Req>()", source)
        self.assertIn("GetDdsMessageTypeSupport<::outer::common::Rsp>()", source)

    def test_scoped_interface_keys_are_injective(self) -> None:
        root = self.write(
            "ScopedInterfaces.idl",
            "module types { struct Req { long x; }; struct Rsp { long y; }; }; "
            "module a_b { interface C { types::Rsp Call(in types::Req request); }; }; "
            "module a { module b { interface C { types::Rsp Call(in types::Req request); }; }; };\n",
        )
        result, stderr = self.run_cli(root)
        self.assertEqual(0, result, stderr)
        source = (self.root / "out/ScopedInterfaces.cc").read_text(encoding="utf-8")
        self.assertIn("kServiceName_3_a_b_1_C", source)
        self.assertIn("kServiceName_1_a_1_b_1_C", source)

    def test_unchanged_generation_does_not_rewrite_outputs(self) -> None:
        path = self.write("Calculator.idl", VALID_IDL)
        result, stderr = self.run_cli(path)
        self.assertEqual(0, result, stderr)
        output = self.root / "out/Calculator.cc"
        first_mtime = output.stat().st_mtime_ns
        time.sleep(0.01)
        result, stderr = self.run_cli(path)
        self.assertEqual(0, result, stderr)
        self.assertEqual(first_mtime, output.stat().st_mtime_ns)

    def test_relative_idl_path_is_rejected(self) -> None:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            result = main(["--idl", "relative.idl", "--output-dir", str(self.root / "out")])
        self.assertEqual(4, result)
        self.assertIn("AIMRT_DDS_IDL_E011_INCLUDE", stderr.getvalue())


class RejectionMatrixTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def diagnostic(self, body: str) -> IdlDiagnostic:
        path = self.root / "invalid.idl"
        path.write_text(
            "module m { struct Req { long x; }; struct Rsp { long y; }; " + body + " };\n",
            encoding="utf-8",
        )
        with self.assertRaises(IdlDiagnostic) as caught:
            load_idl(str(path))
        self.assertEqual(str(path), caught.exception.location.path)
        self.assertGreater(caught.exception.location.line, 0)
        self.assertGreater(caught.exception.location.column, 0)
        return caught.exception

    def test_zero_parameters(self) -> None:
        self.assertIn("E002", self.diagnostic("interface S { Rsp Call(); };").code)

    def test_multiple_parameters(self) -> None:
        self.assertIn("E003", self.diagnostic("interface S { Rsp Call(in Req a, in Req b); };").code)

    def test_out_and_inout_parameters(self) -> None:
        self.assertIn("E004", self.diagnostic("interface S { Rsp Call(out Req a); };").code)
        self.assertIn("E004", self.diagnostic("interface S { Rsp Call(inout Req a); };").code)

    def test_void_response(self) -> None:
        self.assertIn("E005", self.diagnostic("interface S { void Call(in Req a); };").code)

    def test_interface_inheritance(self) -> None:
        self.assertIn("E006", self.diagnostic(
            "interface Base { Rsp A(in Req a); }; interface S : Base { Rsp B(in Req b); };").code)

    def test_overload(self) -> None:
        self.assertIn("E007", self.diagnostic("interface S { Rsp Call(in Req a); Rsp Call(in Req b); };").code)

    def test_raises_and_exception(self) -> None:
        self.assertIn("E008", self.diagnostic("interface S { Rsp Call(in Req a) raises (Oops); };").code)
        self.assertIn("E008", self.diagnostic(
            "exception Oops { long code; }; interface S { Rsp Call(in Req a); };").code)

    def test_unknown_or_non_struct_types(self) -> None:
        self.assertIn("E009", self.diagnostic("interface S { Missing Call(in Req a); };").code)
        self.assertIn("E009", self.diagnostic("interface S { Rsp Call(in Missing a); };").code)
        self.assertIn("E009", self.diagnostic("interface S { long Call(in Req a); };").code)
        self.assertIn("E009", self.diagnostic("interface S { Rsp Call(in long a); };").code)

    def test_duplicate_declaration(self) -> None:
        self.assertIn("E010", self.diagnostic("struct Req { long y; }; interface S { Rsp Call(in Req a); };").code)

    def test_syntax_error(self) -> None:
        self.assertIn("E001", self.diagnostic("interface S { Rsp Call(in Req a) };").code)

    def test_malformed_struct_members_report_stable_source_locations(self) -> None:
        cases = (
            "struct Broken { long; }; interface S { Rsp Call(in Req a); };",
            "struct Broken { long value }; interface S { Rsp Call(in Req a); };",
            "struct Broken { long value(); }; interface S { Rsp Call(in Req a); };",
            "struct Broken { long values[]; }; interface S { Rsp Call(in Req a); };",
        )
        for body in cases:
            with self.subTest(body=body):
                diagnostic = self.diagnostic(body)
                self.assertIn("E001", diagnostic.code)
                self.assertEqual(1, diagnostic.location.line)
                self.assertGreater(diagnostic.location.column, 10)

    def test_cli_failure_leaves_no_partial_outputs(self) -> None:
        path = self.root / "invalid.idl"
        path.write_text(
            "module m { struct Req { long x; }; interface S { void Call(in Req a); }; };\n",
            encoding="utf-8")
        output = self.root / "out"
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            result = main(["--idl", str(path.resolve()), "--output-dir", str(output)])
        self.assertNotEqual(0, result)
        self.assertIn("AIMRT_DDS_IDL_E005_VOID_RESPONSE", stderr.getvalue())
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()

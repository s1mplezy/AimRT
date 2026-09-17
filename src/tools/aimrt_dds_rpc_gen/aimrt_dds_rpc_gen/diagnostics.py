"""Stable diagnostics for the restricted AimRT DDS RPC IDL contract."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class SourceLocation:
    path: str
    line: int
    column: int

    def format(self) -> str:
        return f"{self.path}:{self.line}:{self.column}"


class IdlDiagnostic(Exception):
    """A user-facing lexer, parser, include, or RPC semantic diagnostic."""

    def __init__(self, code: str, message: str, location: SourceLocation):
        super().__init__(message)
        self.code = code
        self.message = message
        self.location = location

    def __str__(self) -> str:
        return f"{self.location.format()}: {self.code}: {self.message}"


E_SYNTAX = "AIMRT_DDS_IDL_E001_SYNTAX"
E_ZERO_PARAMS = "AIMRT_DDS_IDL_E002_ZERO_PARAMETERS"
E_MULTI_PARAMS = "AIMRT_DDS_IDL_E003_MULTIPLE_PARAMETERS"
E_DIRECTION = "AIMRT_DDS_IDL_E004_PARAMETER_DIRECTION"
E_VOID = "AIMRT_DDS_IDL_E005_VOID_RESPONSE"
E_INHERITANCE = "AIMRT_DDS_IDL_E006_INTERFACE_INHERITANCE"
E_OVERLOAD = "AIMRT_DDS_IDL_E007_OPERATION_OVERLOAD"
E_RAISES = "AIMRT_DDS_IDL_E008_RAISES_OR_EXCEPTION"
E_UNKNOWN_TYPE = "AIMRT_DDS_IDL_E009_UNKNOWN_OR_NON_STRUCT_TYPE"
E_DUPLICATE = "AIMRT_DDS_IDL_E010_DUPLICATE_DECLARATION"
E_INCLUDE = "AIMRT_DDS_IDL_E011_INCLUDE"
E_UNSUPPORTED = "AIMRT_DDS_IDL_E012_UNSUPPORTED_DECLARATION"

"""AimRT DDS RPC IDL parser and C++ generator."""

from .diagnostics import IdlDiagnostic
from .loader import load_idl

__all__ = ["IdlDiagnostic", "load_idl"]

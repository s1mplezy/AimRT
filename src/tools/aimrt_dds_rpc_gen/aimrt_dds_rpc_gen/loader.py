"""Recursive include loading, symbol validation, and RPC-subset enforcement."""

from __future__ import annotations

from pathlib import Path

from .diagnostics import (
    E_DIRECTION,
    E_DUPLICATE,
    E_INCLUDE,
    E_INHERITANCE,
    E_MULTI_PARAMS,
    E_OVERLOAD,
    E_RAISES,
    E_UNKNOWN_TYPE,
    E_VOID,
    E_ZERO_PARAMS,
    IdlDiagnostic,
    SourceLocation,
)
from .lexer import Lexer
from .model import InterfaceDecl, StructDecl, TranslationUnit, TypeRef
from .parser import Parser


def _resolve_include(name: str, including: Path, include_dirs: list[Path], location: SourceLocation) -> Path:
    candidates = [including.parent / name, *(directory / name for directory in include_dirs)]
    matches = [candidate.resolve() for candidate in candidates if candidate.is_file()]
    unique = list(dict.fromkeys(matches))
    if not unique:
        raise IdlDiagnostic(E_INCLUDE, f"include not found: {name}", location)
    if len(unique) > 1:
        raise IdlDiagnostic(E_INCLUDE, f"ambiguous include {name}: {', '.join(map(str, unique))}", location)
    return unique[0]


def _merge(target: TranslationUnit, source: TranslationUnit) -> None:
    target.source_paths.extend(path for path in source.source_paths if path not in target.source_paths)
    target.structs.extend(source.structs)
    target.interfaces.extend(source.interfaces)
    target.exceptions.extend(source.exceptions)


def _load_recursive(path: Path, include_dirs: list[Path], visiting: set[Path], loaded: set[Path]) -> TranslationUnit:
    path = path.resolve()
    if path in loaded:
        return TranslationUnit()
    if path in visiting:
        raise IdlDiagnostic(E_INCLUDE, f"cyclic include involving {path}", SourceLocation(str(path), 1, 1))
    visiting.add(path)
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise IdlDiagnostic(E_INCLUDE, f"cannot read IDL: {error}", SourceLocation(str(path), 1, 1)) from error
    lexer = Lexer(text, str(path))
    tokens = lexer.tokenize()
    unit = TranslationUnit()
    for include_name, location in lexer.includes:
        included = _resolve_include(include_name, path, include_dirs, location)
        _merge(unit, _load_recursive(included, include_dirs, visiting, loaded))
    _merge(unit, Parser(tokens).parse(str(path)))
    visiting.remove(path)
    loaded.add(path)
    return unit


def _resolve_struct(type_ref: TypeRef, scope: tuple[str, ...], structs: dict[str, StructDecl]) -> StructDecl | None:
    spelling = type_ref.spelling
    if spelling.startswith("::"):
        return structs.get(spelling[2:])
    for prefix_length in range(len(scope), -1, -1):
        candidate = "::".join((*scope[:prefix_length], spelling))
        declaration = structs.get(candidate)
        if declaration:
            return declaration
    return None


def _validate(unit: TranslationUnit) -> None:
    if unit.exceptions:
        raise IdlDiagnostic(E_RAISES, "exception declarations are not supported by DDS RPC v1", unit.exceptions[0])
    structs: dict[str, StructDecl] = {}
    declarations: dict[str, SourceLocation] = {}
    for declaration in [*unit.structs, *unit.interfaces]:
        fqn = declaration.fqn
        if fqn in declarations:
            raise IdlDiagnostic(E_DUPLICATE, f"duplicate declaration: {fqn}", declaration.location)
        declarations[fqn] = declaration.location
        if isinstance(declaration, StructDecl):
            structs[fqn] = declaration
    for interface in unit.interfaces:
        if interface.inherited:
            raise IdlDiagnostic(E_INHERITANCE, f"interface inheritance is not supported: {interface.fqn}", interface.inherited[0].location)
        method_names: set[str] = set()
        for operation in interface.operations:
            if operation.name in method_names:
                raise IdlDiagnostic(E_OVERLOAD, f"operation overload is not supported: {interface.fqn}::{operation.name}", operation.location)
            method_names.add(operation.name)
            if operation.raises:
                raise IdlDiagnostic(E_RAISES, f"raises clauses are not supported: {interface.fqn}::{operation.name}", operation.location)
            if operation.response_type.spelling == "void":
                raise IdlDiagnostic(E_VOID, f"void response is not supported: {interface.fqn}::{operation.name}", operation.response_type.location)
            if not operation.parameters:
                raise IdlDiagnostic(E_ZERO_PARAMS, f"operation requires exactly one parameter: {interface.fqn}::{operation.name}", operation.location)
            if len(operation.parameters) > 1:
                raise IdlDiagnostic(E_MULTI_PARAMS, f"operation requires exactly one parameter: {interface.fqn}::{operation.name}", operation.parameters[1].location)
            parameter = operation.parameters[0]
            if parameter.direction != "in":
                raise IdlDiagnostic(E_DIRECTION, f"only an 'in' parameter is supported: {interface.fqn}::{operation.name}", parameter.location)
            if _resolve_struct(operation.response_type, interface.scope, structs) is None:
                raise IdlDiagnostic(E_UNKNOWN_TYPE, f"response type must resolve uniquely to a struct: {operation.response_type.spelling}", operation.response_type.location)
            if _resolve_struct(parameter.type_ref, interface.scope, structs) is None:
                raise IdlDiagnostic(E_UNKNOWN_TYPE, f"request type must resolve uniquely to a struct: {parameter.type_ref.spelling}", parameter.type_ref.location)


def load_idl(path: str, include_dirs: list[str] | None = None) -> TranslationUnit:
    root = Path(path)
    if not root.is_file():
        raise IdlDiagnostic(E_INCLUDE, f"IDL file does not exist: {root}", SourceLocation(str(root), 1, 1))
    directories = [Path(directory).resolve() for directory in (include_dirs or [])]
    unit = _load_recursive(root, directories, set(), set())
    _validate(unit)
    return unit

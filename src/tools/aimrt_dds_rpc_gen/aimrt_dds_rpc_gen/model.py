"""AST nodes needed by AimRT's restricted DDS RPC generator."""

from __future__ import annotations

from dataclasses import dataclass, field

from .diagnostics import SourceLocation


@dataclass(frozen=True)
class TypeRef:
    spelling: str
    location: SourceLocation


@dataclass(frozen=True)
class Parameter:
    direction: str
    type_ref: TypeRef
    name: str
    location: SourceLocation


@dataclass
class Operation:
    name: str
    response_type: TypeRef
    parameters: list[Parameter]
    location: SourceLocation
    raises: bool = False
    annotations: list[str] = field(default_factory=list)


@dataclass(frozen=True)
class Declarator:
    name: str
    array_dimensions: tuple[str, ...]
    location: SourceLocation


@dataclass(frozen=True)
class StructMember:
    type_ref: TypeRef
    declarators: tuple[Declarator, ...]
    location: SourceLocation


@dataclass(frozen=True)
class StructDecl:
    scope: tuple[str, ...]
    name: str
    location: SourceLocation
    members: tuple[StructMember, ...] = ()

    @property
    def fqn(self) -> str:
        return "::".join((*self.scope, self.name))


@dataclass
class InterfaceDecl:
    scope: tuple[str, ...]
    name: str
    operations: list[Operation]
    location: SourceLocation
    inherited: list[TypeRef] = field(default_factory=list)
    annotations: list[str] = field(default_factory=list)

    @property
    def fqn(self) -> str:
        return "::".join((*self.scope, self.name))


@dataclass
class TranslationUnit:
    source_paths: list[str] = field(default_factory=list)
    structs: list[StructDecl] = field(default_factory=list)
    interfaces: list[InterfaceDecl] = field(default_factory=list)
    exceptions: list[SourceLocation] = field(default_factory=list)

"""Recursive-descent parser for the AimRT DDS RPC IDL subset."""

from __future__ import annotations

from .diagnostics import E_SYNTAX, E_UNSUPPORTED, IdlDiagnostic
from .lexer import Token
from .model import Declarator, InterfaceDecl, Operation, Parameter, StructDecl, StructMember, TranslationUnit, TypeRef


_PRIMITIVE_WORDS = {
    "boolean",
    "char",
    "double",
    "float",
    "int8",
    "int16",
    "int32",
    "int64",
    "long",
    "octet",
    "short",
    "uint8",
    "uint16",
    "uint32",
    "uint64",
    "unsigned",
    "void",
    "wchar",
}
_GENERIC_TYPES = {"fixed", "map", "sequence", "string", "wstring"}


class Parser:
    def __init__(self, tokens: list[Token]):
        self._tokens = tokens
        self._index = 0
        self.unit = TranslationUnit()

    def _peek(self, offset: int = 0) -> Token:
        return self._tokens[min(self._index + offset, len(self._tokens) - 1)]

    def _advance(self) -> Token:
        token = self._peek()
        if token.kind != "EOF":
            self._index += 1
        return token

    def _match(self, value: str) -> Token | None:
        if self._peek().value == value:
            return self._advance()
        return None

    def _expect(self, value: str) -> Token:
        token = self._peek()
        if token.value != value:
            raise IdlDiagnostic(E_SYNTAX, f"expected {value!r}, found {token.value!r}", token.location)
        return self._advance()

    def _expect_ident(self, context: str) -> Token:
        token = self._peek()
        if token.kind != "IDENT":
            raise IdlDiagnostic(E_SYNTAX, f"expected identifier for {context}, found {token.value!r}", token.location)
        return self._advance()

    def _annotations(self) -> list[str]:
        result: list[str] = []
        while self._match("@"):
            name = self._parse_scoped_name()
            result.append(name.spelling)
            if self._match("("):
                self._skip_balanced("(", ")")
        return result

    def _skip_balanced(self, opening: str, closing: str) -> None:
        depth = 1
        while depth:
            token = self._advance()
            if token.kind == "EOF":
                raise IdlDiagnostic(E_SYNTAX, f"unterminated {opening}{closing} group", token.location)
            if token.value == opening:
                depth += 1
            elif token.value == closing:
                depth -= 1

    def _parse_scoped_name(self) -> TypeRef:
        leading = bool(self._match("::"))
        first = self._expect_ident("scoped name")
        parts = [first.value]
        while self._match("::"):
            parts.append(self._expect_ident("scoped name component").value)
        spelling = ("::" if leading else "") + "::".join(parts)
        return TypeRef(spelling, first.location)

    def _parse_type(self) -> TypeRef:
        token = self._peek()
        if token.kind != "IDENT" and token.value != "::":
            raise IdlDiagnostic(E_SYNTAX, f"expected type, found {token.value!r}", token.location)
        if token.value in _GENERIC_TYPES:
            return self._parse_generic_type()
        if token.value in _PRIMITIVE_WORDS:
            start = self._advance()
            words = [start.value]
            if start.value in {"long", "unsigned"}:
                while self._peek().value in {"long", "short", "int"}:
                    words.append(self._advance().value)
            return TypeRef(" ".join(words), start.location)
        return self._parse_scoped_name()

    def _parse_positive_bound(self, context: str) -> str:
        token = self._peek()
        if token.kind == "NUMBER" and token.value.isdecimal() and int(token.value) > 0:
            return self._advance().value
        if token.kind == "IDENT" or token.value == "::":
            return self._parse_scoped_name().spelling
        raise IdlDiagnostic(E_SYNTAX, f"expected positive integer bound for {context}", token.location)

    def _parse_generic_type(self) -> TypeRef:
        start = self._advance()
        kind = start.value
        if kind in {"string", "wstring"}:
            if not self._match("<"):
                return TypeRef(kind, start.location)
            bound = self._parse_positive_bound(kind)
            self._expect(">")
            return TypeRef(f"{kind}<{bound}>", start.location)

        self._expect("<")
        if kind == "sequence":
            if self._peek().value in {",", ">"}:
                raise IdlDiagnostic(E_SYNTAX, "sequence requires an element type", self._peek().location)
            element = self._parse_type()
            spelling = f"sequence<{element.spelling}"
            if self._match(","):
                spelling += f", {self._parse_positive_bound('sequence')}"
            self._expect(">")
            return TypeRef(spelling + ">", start.location)

        if kind == "map":
            if self._peek().value in {",", ">"}:
                raise IdlDiagnostic(E_SYNTAX, "map requires key and value types", self._peek().location)
            key = self._parse_type()
            self._expect(",")
            if self._peek().value in {",", ">"}:
                raise IdlDiagnostic(E_SYNTAX, "map requires a value type", self._peek().location)
            value = self._parse_type()
            spelling = f"map<{key.spelling}, {value.spelling}"
            if self._match(","):
                spelling += f", {self._parse_positive_bound('map')}"
            self._expect(">")
            return TypeRef(spelling + ">", start.location)

        digits = self._parse_positive_bound("fixed digits")
        self._expect(",")
        scale = self._peek()
        if scale.kind != "NUMBER" or not scale.value.isdecimal():
            raise IdlDiagnostic(E_SYNTAX, "expected non-negative scale for fixed", scale.location)
        self._advance()
        self._expect(">")
        return TypeRef(f"fixed<{digits}, {scale.value}>", start.location)

    def _skip_declaration_to_semicolon(self) -> None:
        groups = {"(": ")", "[": "]", "{": "}", "<": ">"}
        stack: list[str] = []
        while True:
            token = self._advance()
            if token.kind == "EOF":
                raise IdlDiagnostic(E_SYNTAX, "unterminated declaration", token.location)
            if token.value in groups:
                stack.append(groups[token.value])
            elif stack and token.value == stack[-1]:
                stack.pop()
            elif token.value == ";" and not stack:
                return

    def parse(self, source_path: str) -> TranslationUnit:
        self.unit.source_paths.append(source_path)
        self._definitions(())
        self._expect("")
        return self.unit

    def _definitions(self, scope: tuple[str, ...], until: str | None = None) -> None:
        while self._peek().kind != "EOF" and (until is None or self._peek().value != until):
            annotations = self._annotations()
            token = self._peek()
            if token.value == ";":
                self._advance()
            elif token.value == "module":
                self._module(scope)
            elif token.value == "struct":
                self._struct(scope)
            elif token.value == "interface":
                self._interface(scope, annotations)
            elif token.value == "exception":
                self.unit.exceptions.append(token.location)
                self._skip_declaration_to_semicolon()
            elif token.value in {"const", "enum", "typedef", "union", "bitmask", "bitset", "annotation"}:
                self._skip_declaration_to_semicolon()
            else:
                raise IdlDiagnostic(E_UNSUPPORTED, f"unsupported top-level declaration {token.value!r}", token.location)
        if until is not None:
            self._expect(until)

    def _module(self, scope: tuple[str, ...]) -> None:
        self._expect("module")
        name = self._expect_ident("module name")
        self._expect("{")
        self._definitions((*scope, name.value), "}")
        self._expect(";")

    def _struct(self, scope: tuple[str, ...]) -> None:
        self._expect("struct")
        name = self._expect_ident("struct name")
        if self._match(":"):
            self._parse_scoped_name()
        self._expect("{")
        members: list[StructMember] = []
        while self._peek().value != "}":
            if self._peek().kind == "EOF":
                raise IdlDiagnostic(E_SYNTAX, "unterminated struct", name.location)
            members.append(self._struct_member())
        self._expect("}")
        self._expect(";")
        self.unit.structs.append(StructDecl(scope, name.value, name.location, tuple(members)))

    def _struct_member(self) -> StructMember:
        self._annotations()
        type_ref = self._parse_type()
        declarators = [self._declarator()]
        while self._match(","):
            declarators.append(self._declarator())
        self._expect(";")
        return StructMember(type_ref, tuple(declarators), type_ref.location)

    def _declarator(self) -> Declarator:
        name = self._expect_ident("struct member")
        dimensions: list[str] = []
        while self._match("["):
            start = self._peek()
            pieces: list[str] = []
            while self._peek().value != "]":
                token = self._advance()
                if token.kind == "EOF" or token.value in {";", "{", "}"}:
                    raise IdlDiagnostic(E_SYNTAX, "unterminated array declarator", start.location)
                pieces.append(token.value)
            self._expect("]")
            if not pieces:
                raise IdlDiagnostic(E_SYNTAX, "array dimension must not be empty", start.location)
            dimensions.append("".join(pieces))
        return Declarator(name.value, tuple(dimensions), name.location)

    def _interface(self, scope: tuple[str, ...], annotations: list[str]) -> None:
        self._expect("interface")
        name = self._expect_ident("interface name")
        inherited: list[TypeRef] = []
        if self._match(":"):
            inherited.append(self._parse_scoped_name())
            while self._match(","):
                inherited.append(self._parse_scoped_name())
        self._expect("{")
        operations: list[Operation] = []
        while self._peek().value != "}":
            if self._peek().kind == "EOF":
                raise IdlDiagnostic(E_SYNTAX, "unterminated interface", name.location)
            operations.append(self._operation())
        self._expect("}")
        self._expect(";")
        self.unit.interfaces.append(InterfaceDecl(scope, name.value, operations, name.location, inherited, annotations))

    def _operation(self) -> Operation:
        annotations = self._annotations()
        response = self._parse_type()
        name = self._expect_ident("operation name")
        self._expect("(")
        parameters: list[Parameter] = []
        if self._peek().value != ")":
            parameters.append(self._parameter())
            while self._match(","):
                parameters.append(self._parameter())
        self._expect(")")
        raises = False
        if self._match("raises"):
            raises = True
            self._expect("(")
            self._skip_balanced("(", ")")
        if self._match("context"):
            raise IdlDiagnostic(E_UNSUPPORTED, "operation context clauses are not supported", name.location)
        self._expect(";")
        return Operation(name.value, response, parameters, name.location, raises, annotations)

    def _parameter(self) -> Parameter:
        self._annotations()
        direction = self._expect_ident("parameter direction")
        type_ref = self._parse_type()
        name = self._expect_ident("parameter name")
        return Parameter(direction.value, type_ref, name.value, direction.location)

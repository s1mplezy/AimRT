"""Character lexer for the supported OMG IDL surface.

This deliberately tokenizes comments, preprocessing directives, annotations, scoped
names, literals, and punctuation before parsing. It does not infer IDL structure from
lines or regular-expression captures.
"""

from __future__ import annotations

from dataclasses import dataclass

from .diagnostics import E_SYNTAX, IdlDiagnostic, SourceLocation


@dataclass(frozen=True)
class Token:
    kind: str
    value: str
    location: SourceLocation


class Lexer:
    def __init__(self, text: str, path: str):
        self._text = text
        self._path = path
        self._index = 0
        self._line = 1
        self._column = 1
        self.includes: list[tuple[str, SourceLocation]] = []

    def _location(self) -> SourceLocation:
        return SourceLocation(self._path, self._line, self._column)

    def _peek(self, offset: int = 0) -> str:
        index = self._index + offset
        return self._text[index] if index < len(self._text) else ""

    def _advance(self) -> str:
        char = self._peek()
        if not char:
            return ""
        self._index += 1
        if char == "\n":
            self._line += 1
            self._column = 1
        else:
            self._column += 1
        return char

    def _skip_line_comment(self) -> None:
        while self._peek() and self._peek() != "\n":
            self._advance()

    def _skip_block_comment(self) -> None:
        start = self._location()
        self._advance()
        self._advance()
        while self._peek():
            if self._peek() == "*" and self._peek(1) == "/":
                self._advance()
                self._advance()
                return
            self._advance()
        raise IdlDiagnostic(E_SYNTAX, "unterminated block comment", start)

    def _read_string(self) -> Token:
        location = self._location()
        quote = self._advance()
        value: list[str] = []
        while self._peek():
            char = self._advance()
            if char == quote:
                return Token("STRING", "".join(value), location)
            if char == "\\":
                escaped = self._advance()
                if not escaped:
                    break
                value.extend(("\\", escaped))
            elif char == "\n":
                raise IdlDiagnostic(E_SYNTAX, "newline in string literal", location)
            else:
                value.append(char)
        raise IdlDiagnostic(E_SYNTAX, "unterminated string literal", location)

    def _read_identifier(self) -> Token:
        location = self._location()
        chars: list[str] = []
        while self._peek().isalnum() or self._peek() == "_":
            chars.append(self._advance())
        return Token("IDENT", "".join(chars), location)

    def _read_number(self) -> Token:
        location = self._location()
        chars: list[str] = []
        while self._peek().isalnum() or self._peek() in "._+-":
            chars.append(self._advance())
        return Token("NUMBER", "".join(chars), location)

    def _read_directive(self) -> None:
        location = self._location()
        self._advance()
        while self._peek() in " \t":
            self._advance()
        name_token = self._read_identifier() if (self._peek().isalpha() or self._peek() == "_") else None
        if name_token and name_token.value == "include":
            while self._peek() in " \t":
                self._advance()
            if self._peek() == '"':
                include = self._read_string()
                self.includes.append((include.value, location))
            elif self._peek() == "<":
                self._advance()
                chars: list[str] = []
                while self._peek() and self._peek() not in ">\n":
                    chars.append(self._advance())
                if self._peek() != ">":
                    raise IdlDiagnostic(E_SYNTAX, "unterminated include path", location)
                self._advance()
                self.includes.append(("".join(chars), location))
            else:
                raise IdlDiagnostic(E_SYNTAX, "expected include path", location)
        self._skip_line_comment()

    def tokenize(self) -> list[Token]:
        tokens: list[Token] = []
        symbols = "{}()[];,<>:=@+-*/|&~"
        while self._peek():
            char = self._peek()
            if char.isspace():
                self._advance()
                continue
            if char == "/" and self._peek(1) == "/":
                self._skip_line_comment()
                continue
            if char == "/" and self._peek(1) == "*":
                self._skip_block_comment()
                continue
            if char == "#":
                self._read_directive()
                continue
            if char.isalpha() or char == "_":
                tokens.append(self._read_identifier())
                continue
            if char.isdigit():
                tokens.append(self._read_number())
                continue
            if char in ('"', "'"):
                tokens.append(self._read_string())
                continue
            location = self._location()
            if char == ":" and self._peek(1) == ":":
                self._advance()
                self._advance()
                tokens.append(Token("SYMBOL", "::", location))
                continue
            if char in symbols:
                tokens.append(Token("SYMBOL", self._advance(), location))
                continue
            raise IdlDiagnostic(E_SYNTAX, f"unexpected character {char!r}", location)
        tokens.append(Token("EOF", "", self._location()))
        return tokens

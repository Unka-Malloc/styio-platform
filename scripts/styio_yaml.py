#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
from typing import Any


class YamlConfigError(ValueError):
    pass


def strip_yaml_comment(line: str) -> str:
    quote: str | None = None
    escaped = False
    for index, char in enumerate(line):
        if escaped:
            escaped = False
            continue
        if quote == '"' and char == "\\":
            escaped = True
            continue
        if char in {"'", '"'}:
            if quote is None:
                quote = char
            elif quote == char:
                quote = None
            continue
        if char == "#" and quote is None and (index == 0 or line[index - 1].isspace()):
            return line[:index].rstrip()
    return line.rstrip()


def parse_yaml_scalar(value: str) -> Any:
    value = value.strip()
    lower = value.lower()
    if lower == "true":
        return True
    if lower == "false":
        return False
    if lower in {"null", "~"}:
        return None
    if value.startswith('"') and value.endswith('"'):
        return bytes(value[1:-1], "utf-8").decode("unicode_escape")
    if value.startswith("'") and value.endswith("'"):
        return value[1:-1].replace("''", "'")
    try:
        if value and not value.startswith("0") and not value.startswith("+"):
            return int(value)
        if value == "0":
            return 0
    except ValueError:
        pass
    return value


def load_yaml_mapping(path: Path) -> dict[str, Any]:
    root: dict[str, Any] = {}
    stack: list[tuple[int, dict[str, Any]]] = [(-2, root)]
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = strip_yaml_comment(raw_line)
        if not line.strip():
            continue
        if "\t" in line[: len(line) - len(line.lstrip(" \t"))]:
            raise YamlConfigError(f"{path}:{line_number}: tabs are not supported for YAML indentation")
        indent = len(line) - len(line.lstrip(" "))
        if indent % 2 != 0:
            raise YamlConfigError(f"{path}:{line_number}: indentation must use two-space steps")
        content = line.strip()
        if content.startswith("- "):
            raise YamlConfigError(f"{path}:{line_number}: YAML sequences are not supported in tool config")
        if ":" not in content:
            raise YamlConfigError(f"{path}:{line_number}: expected 'key: value' mapping entry")
        key, value = content.split(":", 1)
        key = key.strip()
        if not key:
            raise YamlConfigError(f"{path}:{line_number}: empty YAML mapping key")

        while indent <= stack[-1][0]:
            stack.pop()
        expected_child_indent = 0 if stack[-1][0] < 0 else stack[-1][0] + 2
        if indent > expected_child_indent:
            raise YamlConfigError(f"{path}:{line_number}: missing parent mapping for indentation level")

        parent = stack[-1][1]
        value = value.strip()
        if value == "":
            child: dict[str, Any] = {}
            parent[key] = child
            stack.append((indent, child))
        else:
            parent[key] = parse_yaml_scalar(value)
    return root


def load_config_section(path: Path, section: str, *, required: bool = False) -> dict[str, Any]:
    if not path.exists():
        if required:
            raise YamlConfigError(f"{path}: tool config does not exist")
        return {}
    data = load_yaml_mapping(path)
    value = data.get(section, {})
    if not isinstance(value, dict):
        raise YamlConfigError(f"{path}: section '{section}' must be a YAML mapping")
    return value


def format_yaml_scalar(value: Any) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int) and not isinstance(value, bool):
        return str(value)
    if value is None:
        return "null"
    text = str(value)
    if text and all(char.isalnum() or char in ".-_/:" for char in text):
        return text
    escaped = text.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def dump_yaml_mapping(mapping: dict[str, Any], *, indent: int = 0) -> str:
    lines: list[str] = []
    prefix = " " * indent
    for key, value in mapping.items():
        if isinstance(value, dict):
            lines.append(f"{prefix}{key}:")
            lines.append(dump_yaml_mapping(value, indent=indent + 2).rstrip())
        else:
            lines.append(f"{prefix}{key}: {format_yaml_scalar(value)}")
    return "\n".join(lines) + "\n"


def write_yaml_mapping(path: Path, mapping: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(dump_yaml_mapping(mapping), encoding="utf-8")

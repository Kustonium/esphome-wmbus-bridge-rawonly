#!/usr/bin/env python3
"""Keep the commented options in the example YAMLs honest.

The `*_commented.yaml` examples show optional settings as commented-out lines so
a reader can see what exists without editing anything. That is only useful if a
commented line carries the value the component would use anyway — otherwise
uncommenting it silently changes behaviour, which is the opposite of what the
comment promises.

The single source of truth is the schema in `components/wmbus_radio/__init__.py`
(`cv.Optional(CONF_X, default=Y)`), read here with `ast` so the check runs
without ESPHome installed.

Three rules, checked over every `examples/**/*_commented.yaml`:

  1. a commented option that HAS a schema default must state it inline as
     `# default: <value>`, and that value must match the schema. The commented
     value itself stays an illustration — `# publish_rssi: true` is what a
     reader wants to copy, and the annotation is what tells them what happens
     if they do not;
  2. a commented option name must exist in the schema (typo guard);
  3. an ACTIVE (uncommented) option must not be set to its own default — that
     is noise that later reads as a deliberate override, and it silently pins
     the old behaviour when the schema default changes. Where the pinning IS
     deliberate, say so with a trailing `# pinned: <reason>`.

Options with no schema default (pins, `frequency`, `telegram_topic`, the legacy
`diagnostic_publish_*` flags) are board- or case-specific, so a commented
example value for them is not a default claim and is skipped.

Run: python3 tests/ci/check_example_defaults.py
"""
from __future__ import annotations

import ast
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCHEMA = ROOT / "components" / "wmbus_radio" / "__init__.py"
EXAMPLES = ROOT / "examples"

# Options that carry a default in the schema but must never be advertised in an
# example: turning them on is a documented, deliberate act described elsewhere.
SKIP_OPTIONS = {"cc1101_allow_experimental", "lr1121_allow_experimental",
                "allow_untested_framework"}


def schema_defaults() -> tuple[dict[str, object], set[str]]:
    """Return ({option: default}, {every option name}) from the schema module."""
    tree = ast.parse(SCHEMA.read_text(encoding="utf-8"))

    # CONF_X = "option-name"
    conf_names: dict[str, str] = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.Assign) and len(node.targets) == 1:
            target = node.targets[0]
            if (isinstance(target, ast.Name) and target.id.startswith("CONF_")
                    and isinstance(node.value, ast.Constant)
                    and isinstance(node.value.value, str)):
                conf_names[target.id] = node.value.value

    defaults: dict[str, object] = {}
    known: set[str] = set()
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        func = node.func
        name = func.attr if isinstance(func, ast.Attribute) else getattr(func, "id", "")
        if name not in ("Optional", "Required") or not node.args:
            continue
        arg = node.args[0]
        if isinstance(arg, ast.Name):
            option = conf_names.get(arg.id)
        elif isinstance(arg, ast.Constant) and isinstance(arg.value, str):
            option = arg.value
        else:
            option = None
        if not option:
            continue
        known.add(option)
        for kw in node.keywords:
            if kw.arg == "default":
                try:
                    defaults[option] = ast.literal_eval(kw.value)
                except ValueError:
                    pass  # computed default — nothing to compare against
    return defaults, known


def normalize(raw: str):
    """Turn a YAML scalar as written in an example into a Python value."""
    text = raw.strip()
    if text in ("", "~", "null"):
        return None
    if text in ("[]", "{}"):
        return [] if text == "[]" else {}
    low = text.lower()
    if low in ("true", "yes", "on"):
        return True
    if low in ("false", "no", "off"):
        return False
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'":
        return text[1:-1]
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        pass
    return text


def render(default) -> str:
    """How the annotation should spell a default, so the message is copyable."""
    if isinstance(default, bool):
        return "true" if default else "false"
    if isinstance(default, list) and not default:
        return "[]"
    if isinstance(default, str):
        return default if default.strip() and default.strip() == default else f'"{default}"'
    return str(default)


def matches(raw: str, default) -> bool:
    """Compare a value as written against a schema default.

    The literal text is checked first: YAML reads `off` as a boolean, but for
    `diagnostic_mode` the schema default is the *string* "off", and normalising
    it would turn a correct row into a failure.
    """
    text = raw.strip()
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'":
        text = text[1:-1]
    if isinstance(default, str) and text == default:
        return True
    return same(normalize(raw), default)


def same(value, default) -> bool:
    if isinstance(default, bool) or isinstance(value, bool):
        return value is default
    if isinstance(default, str) and not isinstance(value, str):
        # "234300" in the schema vs 234300 written without quotes
        return str(value) == default
    if isinstance(value, str) and not isinstance(default, str):
        return value == str(default)
    return value == default


# "  # option: value   # trailing note" and its uncommented twin.
COMMENTED = re.compile(r"^\s*#\s*([a-z][a-z0-9_]*)\s*:\s*([^#\n]*?)\s*(?:#(?P<note>.*))?$")
ACTIVE = re.compile(r"^(\s+)([a-z][a-z0-9_]*)\s*:\s*([^#\n]*?)\s*(?:#(?P<note>.*))?$")
DEFAULT_NOTE = re.compile(r"\bdefault:\s*(.+?)\s*$")
PINNED_NOTE = re.compile(r"\bpinned:\s*\S")


def is_prose(raw: str) -> bool:
    """A commented explanation can read like `key: value` mid-sentence.

    Real values here are single tokens ("false", "60s", "3072", "[]") or quoted
    strings; a run of words is a sentence that happens to contain a colon, such
    as the BUSY handshake note in the LR1121 example.
    """
    text = raw.strip()
    if not text or " " not in text:
        return False
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'":
        return False
    return True


# Cells that describe a default in words rather than stating one.
PROSE_CELLS = {"wymagane", "brak", "puste", "mode default", "esphome.name", ""}


def check_reference(defaults: dict[str, object]) -> list[str]:
    """The reference tables carry a `Domyślnie` column — hold it to the schema.

    This is where drift is least visible: nobody rereads a table of sixty rows,
    and a default that changed in the schema goes on reading as true here.
    """
    path = ROOT / "docs" / "CONFIG_REFERENCE_MINIMAL.md"
    if not path.exists():
        return [f"{path.name} is missing"]

    problems: list[str] = []
    default_col: int | None = None
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.startswith("|"):
            default_col = None
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if any(c.lower().startswith("domyśln") for c in cells):
            default_col = next(i for i, c in enumerate(cells)
                               if c.lower().startswith("domyśln"))
            continue
        if default_col is None or default_col >= len(cells):
            continue
        m = re.match(r"^`([a-z][a-z0-9_]*)`$", cells[0])
        if not m:
            continue
        option = m.group(1)
        if option not in defaults:
            continue
        cell = cells[default_col].strip().strip("`")
        if cell.lower() in PROSE_CELLS:
            continue
        if not matches(cell, defaults[option]):
            problems.append(
                f"docs/{path.name}:{lineno}: {option} is documented as "
                f"'{cell}' but the schema default is {defaults[option]!r}")
    return problems


def main() -> int:
    defaults, known = schema_defaults()
    if not known:
        print("FAIL: no options parsed out of the schema", file=sys.stderr)
        return 1

    files = sorted(EXAMPLES.rglob("*_commented.yaml"))
    if not files:
        print("FAIL: no *_commented.yaml examples found", file=sys.stderr)
        return 1

    problems: list[str] = []
    checked = 0
    for path in files:
        rel = path.relative_to(ROOT).as_posix()
        in_block = False
        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if re.match(r"^wmbus_radio:\s*$", line):
                in_block = True
                continue
            # A new top-level key ends the block; comments and indented lines do not.
            if in_block and line and not line[0].isspace() and not line.lstrip().startswith("#"):
                in_block = False
            if not in_block:
                continue

            m = COMMENTED.match(line)
            if m:
                option, raw = m.group(1), m.group(2)
                if option in SKIP_OPTIONS or is_prose(raw):
                    continue
                if option not in known:
                    # Prose in a comment often looks like "key: value"; only flag
                    # names that pretend to be options by sitting alone on a line.
                    if raw == "" or option.startswith(("wmbus", "http", "https")):
                        continue
                    problems.append(f"{rel}:{lineno}: unknown option '{option}'")
                    continue
                if option not in defaults:
                    continue
                checked += 1
                note = DEFAULT_NOTE.search(m.group("note") or "")
                if not note:
                    problems.append(
                        f"{rel}:{lineno}: {option} is missing its "
                        f"'# default: {render(defaults[option])}' annotation")
                elif not matches(note.group(1), defaults[option]):
                    problems.append(
                        f"{rel}:{lineno}: {option} claims default "
                        f"'{note.group(1)}' but the schema says "
                        f"{defaults[option]!r}")
                continue

            m = ACTIVE.match(line)
            if m:
                option, raw = m.group(2), m.group(3)
                if option in SKIP_OPTIONS or option not in defaults or raw == "":
                    continue
                if PINNED_NOTE.search(m.group("note") or ""):
                    continue
                if matches(raw, defaults[option]):
                    problems.append(
                        f"{rel}:{lineno}: {option} is set to its own default "
                        f"({defaults[option]!r}) - comment it out, or add "
                        f"'# pinned: <reason>' if it is deliberate")

    problems.extend(check_reference(defaults))

    for problem in problems:
        print(f"FAIL: {problem}", file=sys.stderr)
    print(f"checked {checked} commented defaults across {len(files)} examples "
          f"plus the reference tables, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())

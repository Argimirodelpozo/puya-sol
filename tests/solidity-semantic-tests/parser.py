"""Parser for Solidity semantic test format.

Each .sol test file has:
  - Solidity source code
  - `// ----` delimiter
  - One or more assertion lines: `// funcName(argTypes): args -> expected`

This module parses the assertion lines into structured test cases.
"""
import re
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class TestCall:
    """A single function call assertion."""
    method_signature: str  # e.g., "f(uint256)" or "g()"
    args: list[str]  # raw arg strings, e.g., ["3", "true", "0x10001"]
    expected: list[str]  # expected return values, e.g., ["3", "3"]
    expect_failure: bool = False
    value_wei: int = 0  # msg.value for payable calls
    raw_line: str = ""

    @property
    def method_name(self):
        return self.method_signature.split("(")[0]


@dataclass
class SemanticTest:
    """A parsed semantic test file."""
    source_path: Path
    source_code: str
    calls: list[TestCall] = field(default_factory=list)
    skipped: bool = False
    skip_reason: str = ""

    @property
    def name(self):
        return self.source_path.stem

    @property
    def category(self):
        return self.source_path.parent.name


def parse_test_file(path: Path) -> SemanticTest:
    """Parse a Solidity semantic test file."""
    content = path.read_text()

    # Split on `// ----`
    parts = content.split("// ----")
    if len(parts) < 2:
        return SemanticTest(source_path=path, source_code=content,
                           skipped=True, skip_reason="no assertion delimiter")

    source_code = parts[0].strip()
    assertion_block = parts[1]

    # Check for features we can't handle
    skip_patterns = [
        ("pragma abicoder v1", "ABIEncoderV1 not supported"),
        ("delegatecall", "delegatecall not supported"),
        ("selfdestruct", "selfdestruct not supported"),
        ("payable", "payable not supported on AVM"),
        ("msg.value", "msg.value not supported on AVM"),
        ("receive()", "receive not supported"),
        ("fallback()", "fallback not supported"),
        ("try {", "try/catch not supported"),
        ("abi.decode", "abi.decode complex cases"),
    ]

    for pattern, reason in skip_patterns:
        if pattern in content:
            return SemanticTest(source_path=path, source_code=source_code,
                               skipped=True, skip_reason=reason)

    calls = []
    for line in assertion_block.strip().split("\n"):
        line = line.strip()
        if not line.startswith("//"):
            continue
        line = line[2:].strip()

        # Skip gas annotations and empty lines
        if line.startswith("gas ") or not line or line.startswith("compileViaYul"):
            continue

        call = _parse_assertion_line(line)
        if call:
            calls.append(call)

    if not calls:
        return SemanticTest(source_path=path, source_code=source_code,
                           skipped=True, skip_reason="no parseable assertions")

    return SemanticTest(source_path=path, source_code=source_code, calls=calls)


def _parse_assertion_line(line: str) -> TestCall | None:
    """Parse a single assertion line like `f(uint256): 3 -> 3, 3`"""
    # Match: methodSig: args -> expected
    # or:    methodSig -> expected  (no args)
    # or:    methodSig(), value -> expected  (payable)

    # Handle value annotations: f(), 1 ether -> ...
    value_wei = 0

    match = re.match(
        r'^([a-zA-Z_]\w*\([^)]*\))'  # method signature
        r'(?:,\s*(\d+)\s+(wei|ether))?'  # optional value
        r'(?:\s*:\s*(.+?))?'  # optional args after ':'
        r'\s*->\s*(.*)$',  # expected after '->' (may be empty for void)
        line
    )

    if not match:
        return None

    method_sig = match.group(1)
    value_amount = match.group(2)
    value_unit = match.group(3)
    args_str = match.group(4)
    expected_str = match.group(5)

    if value_amount and value_unit:
        value_wei = int(value_amount)
        if value_unit == "ether":
            value_wei *= 10**18

    # Parse args
    args = []
    if args_str:
        args = [a.strip() for a in _split_values(args_str)]

    # Parse expected
    expected = [e.strip() for e in _split_values(expected_str)]

    # Check for FAILURE
    expect_failure = any("FAILURE" in e for e in expected)

    return TestCall(
        method_signature=method_sig,
        args=args,
        expected=expected,
        expect_failure=expect_failure,
        value_wei=value_wei,
        raw_line=line,
    )


def _split_values(s: str) -> list[str]:
    """Split comma-separated values, respecting hex strings and nested parens."""
    result = []
    depth = 0
    current = ""
    in_hex = False

    for c in s:
        if c == '"':
            in_hex = not in_hex
            current += c
        elif in_hex:
            current += c
        elif c == '(' :
            depth += 1
            current += c
        elif c == ')':
            depth -= 1
            current += c
        elif c == ',' and depth == 0:
            result.append(current.strip())
            current = ""
        else:
            current += c

    if current.strip():
        result.append(current.strip())
    return result


def parse_value(val_str: str) -> int | bool | bytes | str | None:
    """Convert a semantic test value string to a Python value.

    Examples:
        "0" -> 0
        "true" -> True
        "false" -> False
        "0x10001" -> 65537
        "-1" -> 2**256 - 1 (two's complement)
        "FAILURE" -> None
        'hex"4e487b71"' -> bytes
    """
    val_str = val_str.strip()

    if val_str == "FAILURE":
        return None
    if val_str == "true":
        return True
    if val_str == "false":
        return False
    if val_str == "":
        return None  # void return

    # Strip trailing comments: `24 # empty copy loop #`
    if " #" in val_str:
        val_str = val_str[:val_str.index(" #")].strip()

    # hex literal: hex"abcd"
    if val_str.startswith('hex"') and val_str.endswith('"'):
        return bytes.fromhex(val_str[4:-1])

    # Quoted string literal: "ab" → bytes (ASCII encoding)
    if val_str.startswith('"') and val_str.endswith('"'):
        s = val_str[1:-1]
        # Handle escape sequences
        result = bytearray()
        i = 0
        while i < len(s):
            if s[i] == '\\' and i + 1 < len(s):
                c = s[i + 1]
                if c == 'x' and i + 3 < len(s):
                    result.append(int(s[i+2:i+4], 16))
                    i += 4
                elif c == 'n':
                    result.append(0x0a)
                    i += 2
                elif c == 't':
                    result.append(0x09)
                    i += 2
                elif c == '0':
                    result.append(0)
                    i += 2
                else:
                    result.append(ord(c))
                    i += 2
            else:
                result.append(ord(s[i]))
                i += 1
        return bytes(result)

    # left-padded value: left(0x616263) → integer with left-padding to 32 bytes
    if val_str.startswith("left(") and val_str.endswith(")"):
        inner = val_str[5:-1]
        if inner.startswith("0x"):
            hex_bytes = bytes.fromhex(inner[2:])
            # Left-pad to 32 bytes (shift left)
            padded = hex_bytes + b'\x00' * (32 - len(hex_bytes))
            return int.from_bytes(padded, 'big')
        else:
            return int(inner)

    # Negative numbers → two's complement uint256
    if val_str.startswith("-"):
        try:
            n = int(val_str)
            return (2**256 + n) % (2**256)
        except ValueError:
            return val_str

    # Hex number
    if val_str.startswith("0x"):
        try:
            return int(val_str, 16)
        except ValueError:
            return val_str

    # Decimal number
    try:
        return int(val_str)
    except ValueError:
        return val_str

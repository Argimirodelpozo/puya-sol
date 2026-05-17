"""Multi-source test fixture splitter for Solidity upstream semantic tests.

Solidity's `tests/libsolidity/semanticTests/` format inlines several .sol
files in a single fixture using directive markers:

    ==== Source: a.sol ====
    contract A { ... }
    ==== Source: b.sol ====
    import "a.sol";
    contract B { ... }
    ==== ExternalSource: alias.sol=auxiliary/Other.sol ====

`split_multisource(sol_path, ...)` materialises each declared block to
its own file under a temp directory, resolves `ExternalSource:` against
the upstream Solidity test tree, strips the trailing `// ----` test
expectations from each block, and returns the path to the *main*
source plus the list of every materialised file and the temp dir for
import-path resolution.

This was previously a `puya-sol --split-test` subprocess + a duplicate
implementation in run_tests.py. Both call sites now go through this
single Python entry point — no compiler-side support code is needed.
"""

from __future__ import annotations

import re
import shutil
import tempfile
from pathlib import Path


def _resolve_upstream_root(script_dir: Path) -> Path | None:
    """Walk up from `script_dir` to find `solidity/test/libsolidity/semanticTests/`."""
    cur = script_dir
    for _ in range(5):
        cand = cur / "solidity" / "test" / "libsolidity" / "semanticTests"
        if cand.exists():
            return cand
        cur = cur.parent
    return None


def _upstream_test_dir(sol_path: Path, upstream_root: Path) -> Path:
    """Map `tests/<category>/contracts/<name>.sol` to the upstream `<category>/` directory.

    Our test tree puts contracts under `tests/<cat>/contracts/`, but the
    upstream Solidity tree is flat: `semanticTests/<cat>/<name>.sol`. Drop
    the `contracts` segment when building the upstream lookup path.
    """
    try:
        tests_root = sol_path.parent
        rel_dir_parts: list[str] = []
        walker = tests_root
        while walker.name and walker.name != "tests":
            if walker.name != "contracts":
                rel_dir_parts.append(walker.name)
            walker = walker.parent
        rel_dir = Path(*reversed(rel_dir_parts)) if rel_dir_parts else Path()
        return upstream_root / rel_dir
    except Exception:
        return upstream_root


def split_multisource(
    sol_path: Path,
    upstream_root: Path | None = None,
) -> tuple[Path, list[Path], Path | None, list[str]]:
    """Split a multi-source test fixture into individual files.

    Returns `(main_source_path, [all_source_paths], import_dir, remappings)`.
    If the fixture has no `==== Source: ====` / `==== ExternalSource: ====`
    directives, returns `(sol_path, [sol_path], None, [])` — caller should
    compile `sol_path` directly.

    `remappings` is a list of `<alias>=<filesystem_path>` strings the caller
    should pass via `--remapping` so the Solidity FileReader resolves each
    `import "<alias>"` to its dedicated physical file. This matters when
    two ExternalSource directives share a basename (e.g. `ExtSource.sol`
    and `/ExtSource.sol`) — without per-alias placement they would
    collide on disk and one import would silently shadow the other.

    `upstream_root` overrides upstream-tree discovery; pass None to walk
    up from this script's directory looking for
    `solidity/test/libsolidity/semanticTests/`.
    """
    content = sol_path.read_text()
    has_source = "==== Source:" in content
    has_ext_source = "==== ExternalSource:" in content
    if not has_source and not has_ext_source:
        return sol_path, [sol_path], None, []

    tmp_dir = Path(tempfile.mkdtemp(prefix="multisource_"))
    all_sources: list[Path] = []
    remappings: list[str] = []
    last_name: str | None = None

    # Resolve ExternalSource directives by copying upstream files in.
    if has_ext_source:
        if upstream_root is None:
            upstream_root = _resolve_upstream_root(Path(__file__).resolve().parent)
        if upstream_root is not None:
            test_dir = _upstream_test_dir(sol_path, upstream_root)

            # Two forms:
            #   ==== ExternalSource: path/file.sol ====
            #   ==== ExternalSource: alias.sol=path/file.sol ====
            #
            # Default placement preserves the alias's directory structure
            # (e.g. `_relativeImports/dir/contract.sol` lands at
            # `tmp_dir/_relativeImports/dir/contract.sol`) — this matters
            # for fixtures whose external sources use relative imports
            # like `import "../a.sol"`.
            #
            # Rooted aliases (`/X.sol`) get an isolated subdir + a
            # `--remapping` entry instead, so two aliases sharing a
            # basename (e.g. `ExtSource.sol` and `/ExtSource.sol` in
            # source_remapping.sol) don't overwrite each other. Targets
            # are tmp_dir-relative because Solidity's FileReader resolves
            # remapping targets against its allow-paths (set via
            # `--import-path tmp_dir`).
            ext_re = re.compile(r"^==== ExternalSource: (.+?) ====$", re.MULTILINE)
            for i, m in enumerate(ext_re.finditer(content)):
                raw = m.group(1).strip()
                if "=" in raw:
                    alias, ext_path = raw.split("=", 1)
                    alias = alias.strip()
                    ext_path = ext_path.strip()
                else:
                    alias = raw
                    ext_path = raw
                src = test_dir / ext_path
                if not src.exists():
                    continue

                if alias.startswith("/"):
                    # Rooted alias — isolate to avoid basename collisions.
                    basename = Path(alias).name
                    subdir = tmp_dir / f"_ext_{i}"
                    subdir.mkdir(parents=True, exist_ok=True)
                    dest = subdir / basename
                    shutil.copy(src, dest)
                    all_sources.append(dest)
                    rel_dest = dest.relative_to(tmp_dir)
                    remappings.append(f"{alias}={rel_dest}")
                else:
                    # Non-rooted alias — preserve directory structure.
                    dest = tmp_dir / alias
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy(src, dest)
                    all_sources.append(dest)

    if has_source:
        parts = re.split(r"^==== Source: (.+?) ====$", content, flags=re.MULTILINE)
        if len(parts) < 3:
            # Only ExternalSource — strip directives, keep body as main.
            stripped = re.sub(
                r"^==== ExternalSource: .+? ====\n", "", content, flags=re.MULTILINE
            )
            (tmp_dir / sol_path.name).write_text(stripped)
            return (
                tmp_dir / sol_path.name,
                all_sources + [tmp_dir / sol_path.name],
                tmp_dir,
                remappings,
            )
        for i in range(1, len(parts), 2):
            name = parts[i].strip()
            body = parts[i + 1] if i + 1 < len(parts) else ""
            if "// ----" in body:
                body = body[: body.index("// ----")]
            file_name = name if name.endswith(".sol") else name + ".sol"
            (tmp_dir / file_name).parent.mkdir(parents=True, exist_ok=True)
            (tmp_dir / file_name).write_text(body)
            # Solidity `import "A"` resolves to the literal name first. If
            # the declared section is `==== Source: A ====` (no .sol), the
            # imports use just "A" — write a second copy under the bare
            # name so the FileReader finds it either way.
            if not name.endswith(".sol"):
                (tmp_dir / name).parent.mkdir(parents=True, exist_ok=True)
                (tmp_dir / name).write_text(body)
            all_sources.append(tmp_dir / file_name)
            last_name = name
        main_name = (
            last_name + ".sol" if last_name and not last_name.endswith(".sol")
            else (last_name or sol_path.name)
        )
        return tmp_dir / main_name, all_sources, tmp_dir, remappings
    else:
        # ExternalSource only — strip directives, keep body as main.
        stripped = re.sub(
            r"^==== ExternalSource: .+? ====\n", "", content, flags=re.MULTILINE
        )
        (tmp_dir / sol_path.name).write_text(stripped)
        main_path = tmp_dir / sol_path.name
        return main_path, all_sources + [main_path], tmp_dir, remappings

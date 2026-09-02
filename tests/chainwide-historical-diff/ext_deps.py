#!/usr/bin/env python3
"""Provision dependencies that live ABOVE a verified bundle's root.

Foundry/npm projects verify with remappings like `../../node_modules/<pkg>/`
or `../lib/<forge-lib>/`; Blockscout keeps the project's own files (and
sometimes a stray `../lib/...` file) but nothing can be materialised above the
bundle root. Normalise every `../`-prefixed path under `__ext__/` — files the
bundle carried are relocated, npm packages are installed and copied in (only
the import closure) — and rewrite the remappings + file list consistently.

  python3 ext_deps.py <tag> [--npm pkg@ver ...]
"""
import json, os, re, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CASES = HERE / "cases"
NPM_CACHE = HERE / ".npm_ext"
IMPORT_RE = re.compile(r'^\s*import\s+(?:[^"\']*\s+from\s+)?["\']([^"\']+)["\']', re.M)


def norm_ext(p: str) -> str:
    """'../../node_modules/x' -> '__ext__/node_modules/x'; unchanged otherwise."""
    if not p.startswith("../"):
        return p
    stripped = re.sub(r"^(\.\./)+", "", p)
    return "__ext__/" + stripped


def resolve_import(imp: str, from_file: str, remappings: list[str]) -> str:
    for r in remappings:
        pre, _, tgt = r.partition("=")
        if imp.startswith(pre):
            return os.path.normpath(tgt + imp[len(pre):])
    if imp.startswith("."):
        return os.path.normpath(os.path.join(os.path.dirname(from_file), imp))
    return imp


def main():
    tag = sys.argv[1]
    npm_pkgs = [a for a in sys.argv[3:] if sys.argv[2:3] == ["--npm"]] if "--npm" in sys.argv else []
    case_dir = CASES / tag
    case = json.load(open(case_dir / "case.json"))
    mf = case["multifile"]
    root = case_dir / "src"

    # 1. relocate bundle files that sit above the root
    new_files = []
    for f in mf["files"]:
        nf = norm_ext(f)
        if nf != f:
            src = root / f          # e.g. root/../lib/... -> resolves above root
            dst = root / nf
            src_resolved = (root / f).resolve()
            if src_resolved.exists():
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src_resolved, dst)
        new_files.append(nf)
    mf["main"] = norm_ext(mf["main"])
    mf["remappings"] = [
        (r.partition("=")[0] + "=" + norm_ext(r.partition("=")[2]))
        for r in (mf.get("remappings") or [])
        if not r.partition("=")[2].startswith("/")   # drop absolute dev-machine paths
    ]

    # 2. npm packages -> __ext__/node_modules/<pkg>
    if npm_pkgs:
        NPM_CACHE.mkdir(exist_ok=True)
        subprocess.run(["npm", "install", "--silent", "--no-audit", "--no-fund",
                        "--prefix", str(NPM_CACHE), *npm_pkgs], check=True)
        for pkg in npm_pkgs:
            name = pkg.rsplit("@", 1)[0] if pkg.count("@") > (1 if pkg.startswith("@") else 0) else pkg
            src_pkg = NPM_CACHE / "node_modules" / name
            dst_pkg = root / "__ext__" / "node_modules" / name
            for sol in src_pkg.rglob("*.sol"):
                rel = sol.relative_to(src_pkg)
                d = dst_pkg / rel
                d.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(sol, d)

    # 3. import closure from main over the (rewritten) remappings
    seen, todo = set(), [mf["main"]]
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        p = root / f
        if not p.exists():
            print(f"[ext_deps] {tag}: MISSING {f}", file=sys.stderr)
            continue
        for imp in IMPORT_RE.findall(p.read_text(errors="replace")):
            todo.append(resolve_import(imp, f, mf["remappings"]))
    mf["files"] = sorted(seen)
    json.dump(case, open(case_dir / "case.json", "w"), indent=1)
    missing = [f for f in seen if not (root / f).exists()]
    print(f"[ext_deps] {tag}: {len(seen)} files in closure, {len(missing)} missing; "
          f"remappings={mf['remappings']}")


if __name__ == "__main__":
    main()

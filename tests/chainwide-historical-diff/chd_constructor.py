"""Recover omitted constructor bytes using exact verified solc codegen, not offsets."""
from __future__ import annotations

import hashlib
import json
import re
import sys

from chd_common import verified_compiler_settings


def recover_constructor(verification: dict) -> dict:
    # This entry point runs in EVM_PY, where solcx and eth_abi are installed.
    import solcx
    from eth_abi import decode, encode

    main = verification.get("file_path")
    version = re.fullmatch(r"v?(0\.8\.\d+)(?:\+[^ ]+)?",
                           verification.get("compiler_version") or "")
    if not main or not version:
        raise ValueError("constructor recovery requires verified source name and solc release")
    sources = {main: {"content": verification["source_code"]}}
    for source in verification.get("additional_sources") or []:
        name = source["file_path"]
        if not name or name in sources:
            raise ValueError("constructor recovery requires unique verified source names")
        sources[name] = {"content": source["source_code"]}
    settings = verified_compiler_settings(verification)
    if settings.get("evmVersion") == "default":
        settings.pop("evmVersion")
    settings["outputSelection"] = {"*": {"*": ["abi", "evm.bytecode.object"]}}
    output = solcx.compile_standard({"language": "Solidity", "sources": sources,
                                    "settings": settings}, solc_version=version[1])
    contract = output["contracts"][main][verification["name"]]
    init = bytes.fromhex(contract["evm"]["bytecode"]["object"])
    creation = bytes.fromhex(verification["creation_bytecode"].removeprefix("0x"))
    if not init or not creation.startswith(init):
        raise ValueError("verified solc creation bytecode is not an exact deployment prefix")

    def types(abi):
        def canonical(spec):
            typ = spec["type"]
            if typ.startswith("tuple"):
                return "(" + ",".join(canonical(s) for s in spec["components"]) + ")" + typ[5:]
            return typ
        inputs = next((entry.get("inputs", []) for entry in abi
                       if entry.get("type") == "constructor"), [])
        return [canonical(spec) for spec in inputs]

    constructor_types = types(contract["abi"])
    if constructor_types != types(verification["abi"]):
        raise ValueError("verified constructor ABI disagrees with solc output")
    suffix = creation[len(init):]
    values = decode(constructor_types, suffix)
    if encode(constructor_types, values) != suffix:
        raise ValueError("constructor suffix is not a canonical ABI encoding")
    return {"ctor_args_hex": suffix.hex(), "ctor_args_recovery": {
        "method": "exact verified solc init prefix and canonical ABI suffix",
        "deployment_source": "verification.creation_bytecode",
        "solc_version": version[1], "settings": settings,
        "init_bytes": len(init), "argument_bytes": len(suffix),
        "init_sha256": hashlib.sha256(init).hexdigest(),
        "creation_sha256": hashlib.sha256(creation).hexdigest(),
        "source_sha256": {name: hashlib.sha256(spec["content"].encode()).hexdigest()
                          for name, spec in sources.items()},
    }}


if __name__ == "__main__":
    print(json.dumps(recover_constructor(json.load(sys.stdin))))

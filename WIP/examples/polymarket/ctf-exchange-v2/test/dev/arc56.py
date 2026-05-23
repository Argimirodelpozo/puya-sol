"""ARC-56 spec loading + TEAL compile + memory-init injection."""
from pathlib import Path

import algokit_utils as au
from algosdk import encoding


def load_arc56(p: Path) -> au.Arc56Contract:
    return au.Arc56Contract.from_json(p.read_text())


def compile_teal(algod, teal_text: str) -> bytes:
    return encoding.base64.b64decode(algod.compile(teal_text)["result"])


def inject_memory_init(teal: str) -> str:
    """Prepend the simulated-EVM memory-buffer init to a split helper's TEAL.

    puya only emits the buffer prologue for contracts whose AWST has a
    constructor; helpers extracted by the splitter don't get one. Methods
    that load scratch slot 0 expecting a 4KB zero buffer (for instance
    PolyProxyLib._computeCreationCode) panic without this init.

    Insertion point: directly after the bytecblock, before the router.
    Pattern matches the orchestrator's own memory-init prologue verbatim.
    """
    init = (
        "    pushint 4096\n"
        "    bzero\n"
        "    dup\n"
        "    store 5\n"
        "    store 0\n"
        "    load 0\n"
        "    pushbytes 0x0000000000000000000000000000000000000000000000000000000000000080\n"
        "    replace2 64\n"
        "    store 0\n"
    )
    lines = teal.splitlines()
    out = []
    inserted = False
    for ln in lines:
        out.append(ln)
        if not inserted and ln.lstrip().startswith("bytecblock"):
            out.append(init.rstrip("\n"))
            inserted = True
    return "\n".join(out) + "\n"

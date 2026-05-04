# --uros-splitter integration test

End-to-end demo of the `--uros-splitter` technique on a small Solidity
contract (`Smoke.sol`).

## Setup

```bash
./build.sh
```

This compiles:

- `Smoke.sol` with `--uros-splitter "dec"` →
  - `out/Smoke/Smoke.approval.bin` (main: full surface, `dec` body stubbed)
  - `out/Smoke/__uros_split/Smoke__split.approval.bin` (helper: `dec` real,
    other bodies stubbed)
  - `out/Smoke/deploy.uros.json` (template with both bytecodes + selectors)
- The orchestrator template (`src/splitter/uros_orchestrator.py`) →
  - `out/Orchestrator/UrosOrchestrator.approval.bin`

## Run

```bash
algokit localnet start  # if not already running
python3 test_smoke_dance.py
```

Expected output: `OK`. The test deploys both contracts, populates the
orchestrator's bytecode boxes, primes main's counter to 100, then
submits the dance group `[main.dec(10), orch.dispatch()]`. After the
group commits, main's counter is read back and asserted to be 90.

## What the dance proves

`dec` was COMPILED OUT of main — its body is just `return;`. Yet a call
to `main.dec(10)` followed by `orch.dispatch()` still decrements the
counter by 10. That's because the dispatcher submits a 3-itxn group:

1. UpdateApplication on main with helper bytes
   (now main's program IS the helper's, where dec has its real body).
2. NoOp ApplicationCall on main with the user's selector + args
   (the helper's `dec` runs against main's storage).
3. UpdateApplication on main with main's original bytes (restore).

All three inner-txns atomically succeed or revert, so main's program
is never left in the helper state.

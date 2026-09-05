# Differential fuzzing tools

Experimental tools, separate from the pytest semantic suite. They compare
compiled AVM behavior with either a small Python model or a live solc/py-evm
oracle. The scripts and their fixtures remain in this repository; real-world
contract sources can come from an external checkout.

## Prerequisites

Build `build/puya-sol`, prepare the pinned Puya backend, and use the Python test
environment described in the [semantic test guide](../../solidity-semantic-tests/README.md).
AVM runs require an already-running LocalNet. These commands do not require
resetting its ledger.

Live-EVM tools additionally launch the interpreter at
`tests/WIP/tiny-fuzzing-oracle/.evmvenv/bin/python`. Create that separate
environment from the repository root:

```bash
python3 -m venv tests/WIP/tiny-fuzzing-oracle/.evmvenv
tests/WIP/tiny-fuzzing-oracle/.evmvenv/bin/pip install \
  'eth-tester[py-evm]' py-solc-x web3
```

Package installation and the first solc download need network access. These
research dependencies are not yet locked; record their versions with a run.

## Running from the repository root

```bash
# Fast hand-written oracle: boundary arithmetic/codec checks.
python3 tests/WIP/tiny-fuzzing-oracle/fuzz.py

# Live EVM: pure/view integer computations in a fixture.
python3 tests/WIP/tiny-fuzzing-oracle/fuzz_evm.py \
  tests/WIP/tiny-fuzzing-oracle/contracts/codec_probe.sol --max-per-fn 100

# Real-world stateful sequences over user-supplied sources.
python3 tests/WIP/tiny-fuzzing-oracle/fuzz_realworld.py \
  /path/to/contract-list.txt --limit 10 --max-per-fn 8
```

The contract list contains one `.sol` path per line, absolute or relative to
the working directory. No removed in-tree example collection is required.
`fuzz_realworld.py` checks stateful return values, events, and revert outcomes;
`fuzz_state.py`, `fuzz_seq.py`, and the generator/mutation scripts expose further
campaign modes in their module docstrings.

## Interpreting results

- `oracle.py` is a hand-written Solidity-semantics model. A disagreement may
  be a model bug, so confirm it against the live EVM before changing compiler
  expectations. Solidity builtins and raw EVM opcodes need not have identical
  zero-divisor or overflow behavior.
- `fuzz_evm.py` uses ABI-driven inputs and compares decoded values, not raw
  cross-VM byte strings. The computational subset should agree; address,
  environment, cross-call, and storage paths require profile-aware analysis.
- Known backend defects are not accepted semantic differences. Keep minimized
  reproductions as compiler regressions and consult the
  [current failure baseline](../../solidity-semantic-tests/README.md).
- [EVM_DIVERGENCE.md](../../../EVM_DIVERGENCE.md) is the accepted platform
  policy. A clean campaign proves only its exercised inputs and configured
  profile, not equivalence for arbitrary contracts.

Historical campaign counts and completed findings are available in Git history.
Report new campaigns with the fixture, seed/input set, compiler/backend/oracle
revisions, flags, and pass/divergence counts so they can be reproduced.

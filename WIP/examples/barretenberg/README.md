# barretenberg — UltraHonk Solidity verifier (AVM port, WIP)

Vendored from Aztec's **barretenberg** `sol/` package — the Solidity **UltraHonk**
zkSNARK verifier that `bb` emits for a Noir circuit. Goal: compile, deploy and run
it on the Algorand AVM via puya-sol.

## Provenance
- Upstream: https://github.com/AztecProtocol/aztec-packages — `barretenberg/sol/src/`
- Branch `next`, pinned commit `185f1deddb864bd6a95b3358bbd0d341b8f2a3c2` (`UPSTREAM_COMMIT.txt`)
- License: Apache-2.0 (per-file SPDX headers preserved)
- Mirrored verbatim; the `contracts/` layout matches upstream `src/` so the relative
  imports resolve unchanged.

## Layout
```
contracts/
  honk/
    Fr.sol HonkTypes.sol utils.sol Errors.sol CommitmentScheme.sol
    Transcript.sol ZKTranscript.sol Relations.sol        # library
    BaseHonkVerifier.sol BaseZKHonkVerifier.sol           # base verifiers (generic over a VK)
    instance/  Add2Honk Add2HonkZK Blake Ecdsa Recursive  # thin shims: supply VK + N/LOG_N
    keys/      (EMPTY — see "Missing: verification keys")
  interfaces/IVerifier.sol
reference/   Add2.t.sol TestBaseHonk.sol remappings.txt foundry.toml README.md   # upstream context, NOT compiled
```

## First target: `Add2Honk` (non-ZK)
The smallest circuit (proves `c = a + b`, 3 public inputs). Same verifier logic
(Relations / Transcript / Shplemini) as the others, so it exercises the full
size + frontend challenge with the least proof bulk.

## Missing: verification keys (`keys/*VerificationKey.sol`)
Upstream gitignores `keys/` — `bb`'s `honk_solidity_key_gen` generates one per circuit
(`scripts/init_honk.sh`). `Add2Honk.sol` imports `./../keys/Add2HonkVerificationKey.sol`,
which is not in the repo. **A VK is required to compile.**

## Key facts (investigation, 2026-06-05)
- **Self-contained**: every import is relative; no OpenZeppelin / external remappings.
- **EC pairing**: `utils.sol` uses the BN254 precompiles (`ecAdd`/`ecMul` per EIP-196 +
  a pairing check). On AVM these must map to `ec_add` / `ec_scalar_mul` /
  `ec_pairing_check` (BN254g1/g2, AVM v10).
- **rust-honk fixtures are NOT reusable**: this `next` codegen differs from the existing
  `WIP/examples/rust-honk` blob — `CONST_PROOF_SIZE_LOG_N` 25 vs 28, `NUMBER_OF_ENTITIES`
  41 vs 40, `NUMBER_OF_SUBRELATIONS` 29 vs 26, new wires (Q_NNF / Q_MEMORY / Q_DELTA_RANGE /
  Poseidon2), different VK struct. A matching (VK, proof) must come from the same bb version
  as these sources.
- **Sizes** (Solidity source bytes; compiled AVM differs): `Relations.sol` 32 KB,
  `BaseZKHonkVerifier` 26 KB, `BaseHonkVerifier` 22 KB — expect the same per-program 8 KB
  cap + subroutine-duplication problem documented for rust-honk; the uros IR splitter
  (reachability pruning) is the eventual unlock.

## Plan
1. **Compile**: obtain a shape-correct `Add2HonkVerificationKey.sol` (placeholder or real)
   → run `Add2Honk` through puya-sol; fix frontend gaps (inline assembly, memory structs,
   fixed-size arrays, BN254 precompiles).
2. **Size**: measure per-program bytecode; identify oversize subroutines (Relations, Base).
3. **Split**: apply the splitter to fit the 8 KB cap.
4. **Verify E2E**: real matching VK + proof from `bb`; deploy + `verify(proof, publicInputs)`
   on localnet returns true; pairing checks pass on-AVM.

## Build/test
TBD — will mirror `WIP/examples/rust-honk/test/` (pytest: compile → deploy suite → `verify`).

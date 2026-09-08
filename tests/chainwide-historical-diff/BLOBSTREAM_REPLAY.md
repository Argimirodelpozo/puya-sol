# SP1 Blobstream checkpoint replay — 2026-09-08

**Partial result, not a green end-to-end differential replay.** Four real
header-range submissions pass through the actual bridge, gateway, and PLONK
verifier on a local EVM. All three AVM elements now compile, but the PLONK
verifier exceeds the AVM program-size limit. No verifier stub,
answer tape, Groth16 substitution, or rewritten proof logic was used.

Repository base: `rev-2`, `5dee40fa5a0e8794f4c416729e4b34ed20d17c73`, plus
the existing uncommitted chainwide-harness work and the subsequent
[Yul subroutine lowering](../../docs/yul-subroutines.md). The original replay
changed no `src/` code; the follow-up changes the compiler, not the memory
model. Bridge replay execution used isolated local ledgers, not live
transactions or a shared LocalNet. The separate compiler semantic suite uses
LocalNet with reset disabled.

## Target and provenance

[SP1 Blobstream](https://github.com/succinctlabs/sp1-blobstream/blob/main/contracts/src/SP1Blobstream.sol)
is a Celestia data-attestation/light-client bridge, not a token-delivery bridge.
Its [deployment registry](https://succinctlabs.github.io/sp1-blobstream/deployments.html)
identifies the Ethereum proxy used here:

| Element | Historical address | Exact EVM compiler |
|---|---|---|
| ERC-1967 proxy | `0x7cf3876f681dbb6eda8f6ffc45d66b996df08fae` | solc 0.8.22 |
| SP1Blobstream implementation | `0x46ebfc399d3913bb9b99e73675722417f9c5d416` | solc 0.8.26 |
| SP1VerifierGateway | `0x3b6041173b80e77f038f3f2c0f9744f04837185e` | solc 0.8.20 |
| SP1Verifier v6.1.0 PLONK | `0xc3c6dddac8829b233dc6536ec024775a57b0af2a` | solc 0.8.20 |

Verified source trees, original compiler settings, transactions, raw traces,
and receipts are cached from Blockscout. EVM compilation uses each element's
recorded solc version, source bytes, and settings. The AVM source-rewrite
manifests contain 38 source entries (30 bridge, five gateway, three verifier);
all manifest transformations are Solidity pragma relaxation only.

This deployment uses proof selector `5a093a2f` and program key
`00b451fcd696cd0a4025e30bfed96343b1767ac6523a360fee1183f9e2e20745`.
The earlier Groth16 campaign's proofs are not valid inputs for this bridge.
All four raw traces show the same implementation, gateway, and verifier,
including the actual nested bridge → gateway → verifier calls. The verifier
also makes a self-call to its lower-level `Verify` method.

## Checkpoint and EVM results

Four consecutive successful submissions were selected at Ethereum blocks
25,921,054; 25,921,271; 25,921,488; and 25,921,710. The preceding block,
**25,921,053**, is the independent checkpoint: historical `eth_call` and
`eth_getStorageAt` responses were obtained through dRPC and retained verbatim
in [checkpoint.json](cases/zk_blobstream_plonk_checkpoint/checkpoint.json).

The checkpoint has Celestia height **13,811,200**, proof nonce **16,012**,
relayer checking enabled, an approved historical submitting relayer, the
expected verifier/program key, and `frozen = false`. The trusted header is
`66c8f6847da032420ecf918df96efcc62d5cf3c6d0c7b2c3d7e6f13e97a2cd56`.
These values were read independently, not inferred solely from proof inputs.

The local EVM deploys the real implementation and ERC-1967 proxy, initializes
the proxy with the checkpoint header/key and the local gateway, and registers
the real PLONK verifier through `addRoute`. The historical relayer is
impersonated only on this isolated ledger. Guardian/admin ownership belongs
to the local fixture creator; this is not a full governance-state restoration.

Initialization starts the nonce at one. Its historical value is imported into
proxy storage using **solc's `storageLayout` fact**: `state_proofNonce`, slot
252, byte offset zero, `uint256`. No handwritten storage-layout reconstruction
is used. Older header/commitment mappings beyond the required checkpoint are
not restored. The local clock is synthetic; this is not a historical fork of
every account, balance, block field, or governance setting.

[The EVM report](cases/zk_blobstream_plonk_checkpoint/evm-check.json) records:

- **4/4 original proof submissions succeed**, with real nested gateway and
  verifier execution. No selected submission is skipped.
- All **12 bridge events** match the historical receipt topics and data
  exactly, including the original nonce; no event-value normalization is used.
- After each proof, getters confirm the latest height, target header,
  commitment mapping entry, and next nonce. The final height is 13,814,400 and
  the next nonce is 16,016.
- Four additional local duplicate submissions each reject with the precise
  solc-ABI `TrustedHeaderMismatch()` error, emit no events, and never reach
  the gateway. The four checked state values remain unchanged. Subsequent
  historical proofs still succeed. These are synthetic controls, not four
  more historical transactions or tests of invalid-proof rollback.

## AVM results and blocker

EVM wire ABI and EVM-slot storage are enabled. The bridge additionally opts
into `--proxy-adaptation`; xchain uses the existing harness template/profile.
The default five memory pages remain unchanged.

| Element | Approval bytes | Clear bytes | Observed result |
|---|---:|---:|---|
| SP1Blobstream | 8,414 | 4 | Compiles; initialization/getter smoke test passes |
| SP1VerifierGateway | 2,065 | 4 | Compiles |
| SP1Verifier PLONK | 16,597 | 4 | Compiles after Yul outlining and memory-word sharing; 217 combined bytes too large |

The bridge/gateway numbers are the original campaign measurements. The
verifier number is the subsequent compiler result, not a fresh replay of all
three elements with that compiler.

[The AVM bootstrap report](cases/zk_blobstream_plonk_checkpoint/avm-bootstrap.json)
records **11 successful calls**: initialization, two relayer configuration
calls, and eight getter checks. The deferred implementation constructor is
deliberately not invoked against proxy-runtime storage. This smoke test
deploys no gateway/verifier dependencies and executes no proof path; the
configured gateway address is a value only. Its nonce remains the initializer's
one, not the imported historical nonce used in the EVM proof replay.

Before outlining, the PLONK frontend emitted two contract roots and a **424,222,548-byte AWST JSON
file (404.6 MiB)**. Its log reaches `Invoking puya backend...`; the compile
invocation hit the 900-second limit without producing verifier TEAL or
bytecode. That original result established only a compilation scaling
blocker. Subsequent profiling identified initial Puya SSA construction and
its repeated whole-function trivial-phi replacement scans.

The frontend now preserves 40 reachable Yul helpers per contract as AWST
subroutines, with explicit synthetic-calldata arguments and shared scratch
memory. Same-source-path AWST measurements fall from **469,895,934** to
**142,356,970 bytes**, roughly 70%; the original cached baseline has shorter
source-path metadata. The outlining-stage normal full compiler invocation completed
both contract targets in **119.8 seconds**, with no Puya changes. Detailed
timings, calling-convention coverage and caveats are in the
[subroutine assessment](../../docs/yul-subroutines.md).

The [memory-word sharing follow-up](../../docs/memory-word-subroutines.md)
further reduces the same-path AWST to **62,633,831 bytes** and the derived
verifier to **16,597 approval + 4 clear bytes**. The normal two-contract
compile completes in **52.6 seconds**. Solc-backed call-argument facts add no
further size reduction on this input. The result remains **217 bytes over**
the **16,384-byte** combined limit. Runtime proof cost has not been measured,
and no full AVM proof replay or paired bridge result exists yet.

## Next compiler work and limits

The subroutine investigation is implemented and removes the compilation
blocker. Whole-call successful-termination helpers still use a conservative
inline fallback; recursive functions requiring that convention are rejected.
Memory-word sharing removes 51.01% of the derived verifier's approval bytes;
further code-size reduction or explicit contract splitting is still needed.
Neither a split nor a memory-model redesign is part of this result.

Require a verifier artifact within AVM limits and
rerun the four proofs with both genuine inner-call hops on the AVM. Compare
events and state, then add proof-rejection/rollback controls and real
`verifyAttestation` inputs. None of those later checks is claimed green here.

Upgrade behavior is also outside this result. The bridge compilation warns
about an ERC-1967 slot escaping through `StorageSlot`, unsupported delegatecall
paths, and try/catch adaptation. The accepted staticcall warning means AVM
does not enforce cross-contract read-only execution; it is not a verification
failure to suppress or a guarantee supplied by this campaign.

## Retained local artifacts and rerunning

The new ignored directory
[cases/zk_blobstream_plonk_checkpoint](cases/zk_blobstream_plonk_checkpoint)
retains the source caches, traces, receipts, checkpoint RPC evidence, exact
solc artifacts, compile outputs/logs, and five small experiment scripts. It is
not a maintained general-purpose bridge runner and is not tracked by Git.
The large failed-compile AWST is retained there for diagnosis, not staged.

From `tests/chainwide-historical-diff`, with the existing solc installations
and avm-prover oracle available:

```bash
../WIP/tiny-fuzzing-oracle/.evmvenv/bin/python \
  cases/zk_blobstream_plonk_checkpoint/evm_check.py
python3 cases/zk_blobstream_plonk_checkpoint/avm_bootstrap.py
```

Both commands use the cached inputs and isolated local ledgers; no network
is needed. `collect.py` and `checkpoint.py` document the read-only collection
procedure but are unnecessary for these reruns. Compilation can be retried
separately; verifier compilation is expected to hit the recorded blocker on
the unchanged compiler:

```bash
PUYA_SOL_NO_SERVE=1 PUYA_SOL_COMPILE_TIMEOUT=900 python3 \
  cases/zk_blobstream_plonk_checkpoint/compile_avm.py bridge gateway verifier
```

The existing chainwide-harness unit suite was rerun: **91 passed**. No compiler
semantic-suite certification follows from this partial campaign.

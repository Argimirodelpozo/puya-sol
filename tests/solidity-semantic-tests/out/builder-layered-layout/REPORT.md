# Builder directory-layout verification

Move commit: `ef7caaf0fd8bd42628edbb7393df50e9618fd7d0` on `builder-layered-layout`, based on `5f5cda4d0d9dbc655356308928bfff6cfa34d215`.

Moved **262 files** with `git mv`; rewrote **1007 include lines** and two header-comment paths.
The source-equivalence check covered **332 C++ files**: only mapped include strings and the two permitted comment paths changed.
No function bodies, declarations, namespaces, include ordering, or source formatting changed. The two `.hpp` extensions and all Yul partial filenames are unchanged.

## Verification

- Corpus: **1762 Solidity sources × two storage modes = 3524 cases**.
- Before and after: **3269 successful frontend compilations, 255 recorded failures**, no timeouts.
- **Zero differing exit codes or artifact hashes** across 6538 emitted AWST/options files. Neither AWST nor options JSON was normalized.
- Both captures used identical absolute source arguments and identical relative output arguments under separate before/after working directories. Research-divergence flags came directly from the suite command builder.
- The required clean-first CMake rebuild passed; the explicit source list is grouped in layer order and sorted within each directory.
- Native CTest: **24/24 passed**, 3.57 seconds.
- Full semantic suite: **2507 passed, 1 failed, 100 XFAIL, 39 XPASS**, 1138.01 seconds.
- Test-by-test comparison: **0 changed existing outcomes, 0 removed**. The requested full command also covered **13 pre-existing framework cases** absent from the prior checkpoint XML; all passed. Those cases were already present at the base commit, and this task changed no Python test bodies.
- LocalNet reset was disabled. No compiler rebuild, shared-cache clearing, marker change, Puya edit, or protocol/budget change occurred during the semantic run.
- Tested compiler SHA-256: `739d564f06a612c7a0d98d7648381813822db75480cb4d058431bb25571a996c`.

The corpus manifests retain every per-case exit code and each raw artifact SHA-256, including failed frontend compilations. The semantic XML and CTest output are alongside this report.

### Artifact retention

The results commit includes the suite README, `results.txt`, this report, JUnit/CTest results, and complete hash manifests. As explicitly approved, the 32,419 raw semantic-output files touched by the run (10,096,708,884 bytes) remain local and ignored; they are not added to Git. Their paths, sizes, and SHA-256 hashes are retained in `semantic-output-manifest.json`. The three pre-existing local notes remain untracked.

### Cache investigation

The shared backend cache gained 1001 entries. Of those, 924 already had identical keys in the preceding checkpoint's private full-suite cache at `/tmp/puya-sol-reductions.WEHy6G/cache-full-final`; this run used the shared cache instead. The other 77 keys were not all individually classified.

A separate read-only probe confirmed a pre-existing source-path cause: the base64 multi-source fixture embeds its random `/tmp/multisource_*` root in AWST source locations. Across the prior and current suite runs, that sample differs only in that temporary root; its raw options are identical. Those invocations use different temporary input paths. This diagnostic does not weaken the required corpus comparison: all 3,524 corpus invocations used stable input/output spellings and compared raw hashes without normalization.

No cache was cleared or switched while the suite ran. Cache-key/source-location cleanup is outside this pure-motion task.

## Missing historical entries

- `SecpRangeCheck.h`: already deleted before this task; skipped.
- `sol-eb/BuilderRegistry.cpp`: already deleted before this task; skipped.
- `sol-eb/BuilderRegistry.h`: already deleted before this task; skipped.

## Residual upward includes

The exact requested placement leaves 55 existing direct include edges pointing to a higher layer. These dependencies were preserved, not fixed. Same-layer dependencies are not counted; both `ast/` and `yul/` are L8, all `lowering/` directories are L7, and all storage directories are L4.

| Including file | Layer | Included file | Layer |
| --- | ---: | --- | ---: |
| `ast/calls/SolNewExpression.cpp:17` | L8 | `contract/PostInitTriggers.h` | L9 |
| `ast/exprs/SolAssignment.cpp:21` | L8 | `contract/ContractBuilder.h` | L9 |
| `ast/exprs/SolAssignmentTuple.cpp:6` | L8 | `contract/ContractBuilder.h` | L9 |
| `ast/exprs/SolIdentifier.cpp:7` | L8 | `contract/ContractBuilder.h` | L9 |
| `ast/members/SolIntrinsicAccess.cpp:6` | L8 | `contract/RouterConditions.h` | L9 |
| `ast/members/SolIntrinsicAccess.cpp:10` | L8 | `contract/SelectorRouter.h` | L9 |
| `ast/stmts/SolExpressionStatement.cpp:9` | L8 | `contract/AWSTBuilder.h` | L9 |
| `ast/stmts/SolVariableDeclaration.cpp:8` | L8 | `contract/AWSTBuilder.h` | L9 |
| `codec/ByteSlice.cpp:2` | L3 | `context/BuildArtifacts.h` | L5 |
| `codec/EvmAbiDecode.cpp:9` | L3 | `context/BuildArtifacts.h` | L5 |
| `codec/EvmAbiEncode.cpp:5` | L3 | `context/BuildArtifacts.h` | L5 |
| `codec/EvmMemoryCodec.cpp:6` | L3 | `context/BuildArtifacts.h` | L5 |
| `codec/EvmMemoryCodec.cpp:7` | L3 | `yul/AssemblyBuilder.h` | L8 |
| `codec/SelectorSemantics.cpp:4` | L3 | `lowering/itxn/InnerCallHandlers.h` | L7 |
| `codec/SelectorSemantics.cpp:5` | L3 | `context/ContractContext.h` | L5 |
| `context/CompilationSession.h:9` | L5 | `lowering/calls/FunctionPointerBuilder.h` | L7 |
| `context/ContractContext.cpp:7` | L5 | `ast/SolExpressionDispatch.h` | L8 |
| `context/ContractContext.cpp:8` | L5 | `eb/BinaryOpBuilder.h` | L6 |
| `context/ContractContext.cpp:9` | L5 | `eb/SolIntegerBuilder.h` | L6 |
| `context/ContractContext.cpp:10` | L5 | `eb/SolBoolBuilder.h` | L6 |
| `context/ContractContext.cpp:11` | L5 | `eb/SolAddressBuilder.h` | L6 |
| `context/ContractContext.cpp:12` | L5 | `eb/SolArrayBuilder.h` | L6 |
| `context/ContractContext.cpp:13` | L5 | `eb/SolStructBuilder.h` | L6 |
| `context/ContractContext.cpp:14` | L5 | `eb/SolEnumBuilder.h` | L6 |
| `context/ContractContext.cpp:15` | L5 | `eb/SolFixedBytesBuilder.h` | L6 |
| `context/FunctionIdRegistry.cpp:3` | L5 | `lowering/calls/FunctionPointerBuilder.h` | L7 |
| `context/ProgramAnalysis.cpp:4` | L5 | `lowering/intrinsics/AsaIntrinsics.h` | L7 |
| `context/ProgramAnalysis.cpp:5` | L5 | `lowering/calls/CallResolver.h` | L7 |
| `eb/BuiltinCallables.cpp:6` | L6 | `lowering/itxn/NativePayment.h` | L7 |
| `eb/BuiltinCallables.cpp:7` | L6 | `lowering/itxn/Precompile.h` | L7 |
| `eb/ResolvedLValue.cpp:6` | L6 | `ast/exprs/SolIndexAccess.h` | L8 |
| `eb/ResolvedLValue.cpp:8` | L6 | `yul/AssemblyBuilder.h` | L8 |
| `lowering/abi/AbiSelectorCalldataBuilder.cpp:4` | L7 | `ast/members/SolSelectorAccess.h` | L8 |
| `lowering/calls/FunctionPointerBuilder.cpp:22` | L7 | `ast/calls/RevertBlob.h` | L8 |
| `solc/AsmScan.h:7` | L0 | `context/ProgramAnalysis.h` | L5 |
| `solc/SolcConstFold.cpp:5` | L0 | `types/TypeCoercion.h` | L2 |
| `solc/SolcConstFold.cpp:6` | L0 | `types/TypeMapper.h` | L2 |
| `solc/StorageRefPointer.h:13` | L0 | `context/ProgramAnalysis.h` | L5 |
| `storage/StorageMapper.cpp:9` | L4 | `context/ProgramAnalysis.h` | L5 |
| `storage/StorageRuntimePlan.cpp:3` | L4 | `context/ProgramAnalysis.h` | L5 |
| `storage/slot/EvmSlotLowering.cpp:8` | L4 | `context/TranslationContext.h` | L5 |
| `storage/slot/EvmSlotLowering.h:15` | L4 | `context/ContractContext.h` | L5 |
| `storage/slot/EvmSlotValueLowering.cpp:7` | L4 | `context/BuildArtifacts.h` | L5 |
| `storage/slot/EvmSlotValueLowering.cpp:8` | L4 | `context/TranslationContext.h` | L5 |
| `target/EvmFeaturePolicy.cpp:3` | L1 | `context/BuildArtifacts.h` | L5 |
| `target/EvmFeaturePolicy.cpp:4` | L1 | `types/TypeMapper.h` | L2 |
| `types/CallBoundaryPlan.cpp:2` | L2 | `codec/EvmMemoryCodec.h` | L3 |
| `types/CallBoundaryPlan.cpp:8` | L2 | `codec/EvmAbiDecode.h` | L3 |
| `types/CallBoundaryPlan.cpp:9` | L2 | `codec/EvmValueCodec.h` | L3 |
| `types/RefParamPassing.h:12` | L2 | `codec/Arc4Defaults.h` | L3 |
| `types/ReturnWirePlan.cpp:2` | L2 | `context/ProgramAnalysis.h` | L5 |
| `types/ReturnWirePlan.cpp:6` | L2 | `codec/Arc4Defaults.h` | L3 |
| `types/TypeCoercion.cpp:13` | L2 | `codec/Arc4Defaults.h` | L3 |
| `types/TypeCoercion.cpp:14` | L2 | `codec/Arc4ArrayWidening.h` | L3 |
| `types/TypeMapper.cpp:3` | L2 | `codec/Arc4Defaults.h` | L3 |

## Out-of-scope observations

- The known Puya DCE failure remains a correctness bug: an unused division/modulo can lose its zero-divisor trap. It remains a normal failing test.
- The directory move does not by itself establish a dependency DAG. In particular, some L0 fact helpers still consume context or type/coercion headers, and codecs still consume build state or Yul emission. Eliminating those edges requires later declaration/implementation splits.
- Older path references in `puyabug.md`, the historical untracked reduction audit, and basename-only references in existing comments were left alone: they are outside the listed path-update scope.
- The pre-existing local analysis notes remain untracked unless their owner explicitly requests otherwise. Path-only updates were made to `memory_redesign.md` and `chainwide_overnight.md`; no `TODO.md` is committed.

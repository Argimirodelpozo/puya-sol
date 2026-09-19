# Open engineering follow-ups

Maintained follow-ups carried forward from the September 2026 audits. Completed
audit/refactor reports have been removed; tracked originals remain in Git
history and local-only notebooks were archived before removal. This list is not
a new security audit or a complete catalogue of miscompilations. The compiler
remains experimental and unsuitable for production funds.

- **Backend optimization semantics:** the literal-fold divide/modulo regression
  is fixed in `6d940b1e43` by explicit frontend zero-divisor guards. The pinned
  Puya optimizer itself is unchanged; this is not a general proof that it
  preserves every unused may-trap expression. The latest full-suite result is
  in the [semantic test guide](../tests/solidity-semantic-tests/README.md).
- **Continuous semantic evidence:** clean-build/native CI exists, but scheduled
  LocalNet semantic/differential gates, sanitizer/fuzz coverage, and review of
  non-strict xpasses remain follow-ups. Publish results against exact root and
  dependency revisions rather than accumulating version-numbered text dumps.
- **Reproducible test environment:** build inputs and Puya are pinned, but the
  root Python test dependencies and LocalNet environment need a declared,
  reproducible setup. Artifact reproducibility checks and dependency/SBOM
  reporting remain separate from the existing build manifest.
- **Project metadata:** the root still needs an owner-selected license,
  vulnerability-reporting policy, and contribution/release ownership guidance.
  Dependency licenses do not substitute for first-party project metadata.
- **Dirty scalar transport:** narrow scalar words survive separate assembly
  blocks and same-type local copies; this is not a promise of arbitrary
  dirty-word transport across function/ABI boundaries. Boundary cleanup and
  validation follow the relevant solc operation.
- **Large real-workload deployment limits:** host-aware callable reachability
  fixes FireBridge's foreign virtual-call compile failure. Its current exact
  workload compiles to 22,084 approval plus 4 clear bytes, still above the
  replay's 16,384-byte limit; compilation success is not a successful replay.
  PrivacyPoolSimple also remains over that limit and has separate closed-world
  replay exclusions. The [historical workload report](../tests/sizes/reports/reference-boundaries-real-workloads.json)
  retains the earlier compiler failure, solc results and replay details.
- **Remapping diagnostics:** malformed import remappings still emit warnings.
  Whether they should fail immediately remains a policy decision; log-file
  opening and source-read failures now have explicit error handling and tests.

The former modifier write-through/rebind limitation is no longer current:
modifier parameters use distinct pointer locals over shared roots, including
branch/loop, tuple and Yul rebinds. The storage format and remaining direct-copy
capacities are documented in [storage-format.md](storage-format.md). Self-call
frame limitations and intentional cross-contract behavior remain in the
divergence policy, not a claim of full EVM frame emulation.

The former internal calldata-return rejection is also resolved: scalar and tuple
bindings preserve input coordinates and their immutable byte view across calls,
modifiers and function pointers. Public-library calls retain their ABI copy
boundary, and native ARC4 `msg.data` retains its documented compatibility view.
This does not change the memory model or remove the target's size limits.

Intentional behavior differences belong in [EVM_DIVERGENCE.md](../EVM_DIVERGENCE.md),
not in the bug backlog. In particular, missing cross-contract static-call
read-only enforcement is accepted and warning-only. Proxy implementation
boundaries and future directions remain in [proxy.md](../proxy.md).

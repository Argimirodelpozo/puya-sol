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
- **Calldata-local alias provenance:** ordinary calldata alias initialization
  does not always initialize the raw Yul pointer metadata. The subarray tests
  that explicitly assign both `.offset` and `.length` do not cover implicit
  alias initialization. This is separate from the memory-model experiment.
- **Raw Yul scalar lifetime:** narrow unsigned, bool and fixed-bytes locals now
  preserve full words within an assembly block. That does not establish full
  dirty-word preservation across separate blocks and intervening high-level use.
- **Remapping diagnostics:** malformed import remappings still emit warnings.
  Whether they should fail immediately remains a policy decision; log-file
  opening and source-read failures now have explicit error handling and tests.

The former modifier write-through/rebind limitation is no longer current:
modifier parameters use distinct pointer locals over shared roots, including
branch/loop, tuple and Yul rebinds. The storage format and remaining direct-copy
capacities are documented in [storage-format.md](storage-format.md). Self-call
frame limitations and intentional cross-contract behavior remain in the
divergence policy, not a claim of full EVM frame emulation.

Intentional behavior differences belong in [EVM_DIVERGENCE.md](../EVM_DIVERGENCE.md),
not in the bug backlog. In particular, missing cross-contract static-call
read-only enforcement is accepted and warning-only. Proxy implementation
boundaries and future directions remain in [proxy.md](../proxy.md).

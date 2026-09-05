# Open engineering follow-ups

Maintained follow-ups carried forward from the September 2026 audits. Completed
audit/refactor reports have been removed; their tracked originals remain in Git
history before this cleanup. This list is not a new security audit or a complete
catalogue of miscompilations. The compiler remains experimental and unsuitable
for production funds.

- **Backend correctness:** the pinned Puya optimizer still drops a required
  divide-by-zero revert in the literal-fold DCE regression. The exact test and
  current full-suite result are in the [semantic test guide](../tests/solidity-semantic-tests/README.md).
  Keep that failure visible until the backend fix is available in the pinned
  dependency; it is not an accepted divergence.
- **Native-payment policy coverage:** high-level `selfdestruct` and
  value-bearing Yul `call` construct payments outside the shared receiver/policy
  boundary. The ordinary high-level payment path checks divergence acceptance
  and xchain mapping, but those other paths do not. See the
  [address/value-transfer limitations](../EVM_DIVERGENCE.md).
- **Continuous semantic evidence:** clean-build/native CI exists, but scheduled
  LocalNet semantic/differential gates, sanitizer/fuzz coverage, and review of
  non-strict xpasses remain follow-ups. Publish results against exact root and
  dependency revisions rather than accumulating version-numbered text dumps.
- **Reproducible test environment:** build inputs and Puya are pinned, but the
  root Python test dependencies and LocalNet environment need a declared,
  reproducible setup. Artifact reproducibility checks and dependency/SBOM
  reporting remain separate from the existing build manifest.
- **Generated-output hygiene:** many historical compiler artifacts outside the
  removed example collections are still tracked. Stop tracking them in a
  separately scoped cleanup, retaining only deliberate golden fixtures and
  external test evidence. Removing files now does not shrink existing Git
  history; a history rewrite would require a separate decision.
- **Project metadata:** the root still needs an owner-selected license,
  vulnerability-reporting policy, and contribution/release ownership guidance.
  Dependency licenses do not substitute for first-party project metadata.
- **Operational diagnostics:** the earlier audit identified unchecked log-file
  opening/source-read failures and warning-only invalid remappings. Revisit
  those entry points and filesystem exception handling with focused negative
  tests; they were not part of the builder refactor.

Intentional behavior differences belong in [EVM_DIVERGENCE.md](../EVM_DIVERGENCE.md),
not in the bug backlog. In particular, missing cross-contract static-call
read-only enforcement is accepted and warning-only. Proxy implementation
boundaries and future directions remain in [proxy.md](../proxy.md).

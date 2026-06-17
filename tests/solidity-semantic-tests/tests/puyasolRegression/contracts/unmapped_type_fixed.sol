// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// A value-carrying unmapped type (fixed-point) must HARD-ERROR — there is no AVM
// mapping, so a silent bytes fallback would diverge from EVM. Guards the selective
// unmapped-type hard-error (TypeMapper map() default case). Meta-types and slices
// still map to bytes (covered by the vendored suite); only genuine value types error.
contract C {
    function f(fixed x) public pure returns (uint256) {
        return 0;
    }
}

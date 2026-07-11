// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    // const outer, variable inner element (executes when inner non-empty)
    function innerLoop(uint256[][] calldata a) external pure returns (uint256 s) {
        if (a.length == 0) return 0;
        for (uint j; j < a[0].length; j++) s += a[0][j];
    }
    // both variable (= the failing 'nested')
    function bothVar(uint256[][] calldata a) external pure returns (uint256 s) {
        for (uint i; i < a.length; i++) for (uint j; j < a[i].length; j++) s += a[i][j];
    }
    // EXTRACT the inner array reference to a local, then index the local (your idea, in spirit)
    function viaLocal(uint256[][] calldata a) external pure returns (uint256 s) {
        for (uint i; i < a.length; i++) {
            uint256[] calldata inner = a[i];
            for (uint j; j < inner.length; j++) s += inner[j];
        }
    }
}

// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;
contract EffectsAudit {
    enum E { A, B }
    function discardIndex(uint256 i) external pure returns (uint256) {
        uint256[1] memory a; a[i]; return 7;
    }
    function discardBytes(uint256 i) external pure returns (uint256) {
        bytes memory a = hex"01"; a[i]; return 7;
    }
    function discardEnum(uint256 x) external pure returns (uint256) {
        E(x); return 7;
    }
    function discardAdd(uint256 x) external pure returns (uint256) {
        x + 1; return 7;
    }
    function discardNegate(int256 x) external pure returns (uint256) {
        -x; return 7;
    }
    function discardDecode(bytes calldata data) external pure returns (uint256) {
        abi.decode(data, (uint256[])); return 7;
    }
    function discardTuple(uint256 x) external pure returns (uint256) {
        (1 / x, x + 1); return 7;
    }
    function boom() internal pure returns (bool) { revert("boom"); }
    function shortCircuit(bool flag) external pure returns (uint256) {
        flag && boom(); return 7;
    }
    function shortCircuitOr(bool flag) external pure returns (uint256) {
        flag || boom(); return 7;
    }
    function discardedConditional(bool flag) external pure returns (uint256) {
        flag ? boom() : true; return 7;
    }
    function leaveControl() external pure returns (uint256 r) {
        assembly { function f() -> value { value := 3 leave value := 4 } r := add(f(), 7) }
    }
    function halt() internal pure returns (uint256) { assembly { return(0, 0) } }
    function haltControl() external pure returns (uint256) { halt(); return 9; }
    function returnControl() external pure returns (uint256 r) {
        assembly { function haltYul() { return(0, 0) } haltYul() r := 9 }
    }
}

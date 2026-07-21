// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards Batch A correctness fixes (fable-review-3):
// M4  transient assignment-as-expression yields the ASSIGNED value, not a
//     stale storage re-read (the write is queued post-pending);
// M16 self-call via encodeWithSignature resolves the OVERLOAD by full
//     signature (not first same-arity match) + names the overload-suffixed
//     target so puya can resolve it.
contract BatchACorrectness {
    uint256 transient t;

    function m4Transient() external returns (uint256 a, uint256 tval) {
        a = (t = 5);   // must be 5 (assigned value), not the stale pre-write t
        tval = t;      // must be 5
    }

    function f(uint256 x) public pure returns (uint256) { return x + 1000; }
    function f(bool b) public pure returns (uint256) { return b ? 7 : 8; }

    function m16Uint() external returns (uint256) {
        (, bytes memory r) = address(this).call(abi.encodeWithSignature("f(uint256)", uint256(5)));
        return abi.decode(r, (uint256)); // 1005
    }

    function m16Bool() external returns (uint256) {
        (, bytes memory r) = address(this).call(abi.encodeWithSignature("f(bool)", true));
        return abi.decode(r, (uint256)); // 7
    }
}

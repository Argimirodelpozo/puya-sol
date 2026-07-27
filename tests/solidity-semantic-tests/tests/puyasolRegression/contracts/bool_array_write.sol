// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
// CUSTOM regression fixture (NOT vendored). Guards WRITING a bool[] element
// (`f[i] = v`). The write target is the raw arc4.bool array element, so the RHS
// native bool must be ARC4-encoded — but applyArc4EncodeIfNeeded's kind-based
// targetIsArc4 check missed arc4.bool (kind `Basic`), leaving the value native
// bool → puya "assignment target type differs from expression value type".
// Fixed by encoding native bool into an arc4.bool target. Companion to the READ
// fix (bool_array_condition.sol / 19d7e1ba32).
contract BoolArrWrite {
    function writeRead(uint256 i, bool v) external pure returns (bool) {
        bool[] memory f = new bool[](8);
        f[i] = v;
        return f[i];
    }
    function writeMulti(bool a, bool b, bool c, uint256 i) external pure returns (bool) {
        bool[] memory f = new bool[](3);
        f[0] = a; f[1] = b; f[2] = c;
        return f[i];
    }
    function toggle(uint256 i, bool first) external pure returns (bool) {
        bool[] memory f = new bool[](4);
        f[i] = first;
        f[i] = !first;
        return f[i];
    }
}

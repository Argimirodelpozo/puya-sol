// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the fail-loud on conditional storage-pointer reassignment: `p = a2`
// lowers to a COMPILE-TIME alias rebind (no runtime artifact), so inside an
// if-branch it applied unconditionally — `if (c) p = a2; p.push(1);` always
// pushed to a2, even when c was false. Until a runtime lowering exists this
// must be a compile error, not a silent miscompile.
contract CondStoragePtrReassign {
    uint256[] a1;
    uint256[] a2;

    function f(bool c) external returns (uint256) {
        uint256[] storage p = a1;
        if (c) {
            p = a2;
        }
        p.push(1);
        return p.length;
    }
}

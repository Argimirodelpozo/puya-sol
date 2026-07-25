// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the pop(call(...)) form: a call whose success flag is DISCARDED must
// still perform the inner txn AND copy returndata to the output offset. Was a
// silent no-op (the `pop` handler discarded its arg unevaluated; call in
// expression context returned 1 with no itxn), so `r` read stale memory
// (0x80 = 128) instead of the callee's return. This is also exactly the shape
// UnusedPruner produces (--yul-prepass) from `let unused := call(...)`.
contract PopCallCallee {
    function ping(uint256 x) external pure returns (uint256) { return x + 1000; }
}

contract PopCallCaller {
    function h(uint256 x) external returns (uint256 r) {
        PopCallCallee c = new PopCallCallee();
        address t = address(c);
        uint32 sel = uint32(PopCallCallee.ping.selector);
        assembly {
            mstore(0x00, sel)
            mstore(0x20, x)
            pop(call(gas(), t, 0, 0x1c, 0x24, 0x40, 0x20))
            r := mload(0x40)
        }
    }
}

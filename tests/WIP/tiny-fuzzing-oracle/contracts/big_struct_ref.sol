// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Handle-model Stage 1a validation: a struct large enough to be ALWAYS box-backed
// (4 uint256 = 128B) passed BY REFERENCE to a contract-method internal function.
// Pre-handle-model this hit copy+write-back, which doesn't reach contract methods →
// the write was lost (would return 0). With the box-key handle it writes through → 5.
contract BigStructRef {
    struct Big { uint256 a; uint256 b; uint256 c; uint256 d; }
    Big big;

    function refWritesThrough() external returns (uint256) {
        big.a = 0;
        _bump(big);          // pass the storage struct ref to a contract method
        return big.a;        // EVM: 5
    }
    function _bump(Big storage s) internal { s.a = 5; }

    // also exercise a local storage-ref binding + a direct read (must stay correct)
    function localRefAndDirect() external returns (uint256) {
        big.b = 7;
        Big storage s = big;
        s.c = 9;
        return big.b + s.c;  // EVM: 16
    }
}

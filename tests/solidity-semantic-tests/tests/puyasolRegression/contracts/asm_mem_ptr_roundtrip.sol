// SPDX-License-Identifier: MIT
pragma solidity >=0.8.17 <0.9.0;

// CUSTOM regression fixture (NOT vendored). Guards the memory-pointer-seam
// round-trip: an asm `mstore` into a `new bytes` buffer must be visible to a
// later asm `mload` of that same buffer. Such a buffer is blob-backed (scratch
// slots addressed by an offset var); the write was mis-routed to an
// uninitialised value local while the read used the scratch blob, so mload
// returned 0. Fixed by routing blob-backed aggregates through the generic
// scratch path for BOTH mstore and mload (matchBytesMemoryDataPtr excludes
// them). Also covers the exp(256,N) constant-fold used by ENS AddrResolver's
// addr<->bytes asm. See ens-compile / memory-pointer-seam.
contract AsmMemPtrRoundtrip {
    // mstore then mload of the same new-bytes buffer, one asm block.
    function rt(uint256 v) external pure returns (uint256 r) {
        bytes memory b = new bytes(32);
        assembly { mstore(add(b, 32), v)  r := mload(add(b, 32)) }
    }

    // Same, split across two asm blocks (the write-back must still be visible).
    function rt2(uint256 v) external pure returns (uint256 r) {
        bytes memory b = new bytes(32);
        assembly { mstore(add(b, 32), v) }
        assembly { r := mload(add(b, 32)) }
    }

    // ENS AddrResolver shape: exp(256,12)=2^96 fold + new bytes(20) + mstore/mload.
    // uint160 avoids address-encoding noise; (v << 96) >> 96 == v for 160-bit v.
    function expRt(uint160 v) external pure returns (uint160 r) {
        bytes memory b = new bytes(20);
        assembly { mstore(add(b, 32), mul(v, exp(256, 12))) }
        assembly { r := div(mload(add(b, 32)), exp(256, 12)) }
    }
}

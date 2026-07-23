// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the storage-layout drift the item-7 solc-layout tripwire caught: a
// denomination-sized fixed array (uint[2 ether]) spans ~2e18 slots; the layout
// walk's old `unsigned` cast saturated storageSize() at 2^32-1 and shifted
// every FOLLOWING state var to a wrong slot. `after` must read/write its own
// slot, uncorrupted by the giant array before it.
contract DenominationArrayLayout {
    uint256 first;
    uint256[2 ether] big; // ~2e18 slots
    uint256 after_;

    // `big` is present ONLY to exercise the layout walk (its huge span used
    // to saturate the slot counter). We do not access its storage — a 2e18-
    // element array's box backing is a separate concern from slot ASSIGNMENT.
    function setFirst(uint256 v) external { first = v; }
    function setAfter(uint256 v) external { after_ = v; }
    function getFirst() external view returns (uint256) { return first; }
    function getAfter() external view returns (uint256) { return after_; }
    function bigLen() external view returns (uint256) { return big.length; }
}

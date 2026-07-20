// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the __postInit creator-only auth (fable-review-3 M19). A box state var
// forces the deferred __postInit deploy path; the constructor sets owner and
// seeds state. The legitimate create+postInit (same sender = creator) must
// succeed; a front-runner (different sender) would revert on the creator check.
contract PostInitCreatorOnly {
    address public owner;
    uint256[] public arr;

    constructor(uint256 seed) {
        owner = msg.sender;
        arr.push(seed);
        arr.push(seed + 1);
    }

    function len() external view returns (uint256) {
        return arr.length;
    }
}

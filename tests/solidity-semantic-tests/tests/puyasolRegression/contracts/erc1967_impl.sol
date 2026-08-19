// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// CUSTOM puya-sol regression — EIP-1967 slot lowering (proxy.md §1).
// The admin slot becomes a synthesized app global gating NATIVE app updates;
// the implementation slot reads as this app's own identity; upgradeTo's
// slot write is a runtime trap (the AVM upgrade is an UpdateApplication
// transaction, not an in-contract call).
contract Erc1967Impl {
    bytes32 private constant _ADMIN_SLOT =
        0xb53127684a568b3173ae13b9f8a6016e243e63b6e8ee1178d6a717850b5d6103;
    bytes32 private constant _IMPL_SLOT =
        0x360894a13ba1a3210667c828492db98dca3e2076cc3735a920a3ca505d382bbc;
    bytes32 private constant _BEACON_SLOT =
        0xa3f0ad74e5423aebfd80d3ef4346578335a9a72aeaee59ff6cb3582b35133d50;

    uint256 public value;

    function admin() public view returns (address a) {
        assembly { a := sload(_ADMIN_SLOT) }
    }

    function initAdmin(address a) public {
        require(admin() == address(0), "already initialized");
        assembly { sstore(_ADMIN_SLOT, a) }
    }

    function changeAdmin(address n) public {
        require(msg.sender == admin(), "not admin");
        assembly { sstore(_ADMIN_SLOT, n) }
    }

    function implementation() public view returns (address i) {
        assembly { i := sload(_IMPL_SLOT) }
    }

    function upgradeTo(address n) public {
        require(msg.sender == admin(), "not admin");
        assembly { sstore(_IMPL_SLOT, n) }
    }

    function setValue(uint256 v) public { value = v; }

    // Beacon slot has no AVM analogue — both directions are runtime traps
    // (compile-time warnings; the call sites revert if reached).
    function beacon() public view returns (address b) {
        assembly { b := sload(_BEACON_SLOT) }
    }

    function setBeacon(address n) public {
        assembly { sstore(_BEACON_SLOT, n) }
    }

    // A real 1967 proxy always has a fallback. Its presence switches dispatch
    // to the custom (receive/fallback) shape, which pre-A2-fix approved bare
    // UpdateApplication calls without ever reaching the admin gate — the
    // bare-update legs below pin that interaction.
    uint256 public fallbackCount;
    fallback() external { fallbackCount += 1; }
}

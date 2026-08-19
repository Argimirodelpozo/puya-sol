// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// CUSTOM puya-sol regression — new_review.md A3: admin-slot assembly reached
// through a LIBRARY (OZ's ERC1967Utils shape) must arm the update gate on the
// contract whose call graph reaches it — not on whichever contract the unit
// builds first.
library AdminLib {
    bytes32 internal constant _ADMIN_SLOT =
        0xb53127684a568b3173ae13b9f8a6016e243e63b6e8ee1178d6a717850b5d6103;

    function getAdmin() internal view returns (address a) {
        assembly { a := sload(_ADMIN_SLOT) }
    }

    function setAdmin(address a) internal {
        assembly { sstore(_ADMIN_SLOT, a) }
    }
}

// Deliberately FIRST in the unit and 1967-free: the pre-fix unit-global flag
// handed THIS contract the admin global and gate.
contract Unrelated {
    uint256 public x;
    function setX(uint256 v) public { x = v; }
}

// UUPS shape: implementation slot only, no admin-slot use anywhere. Gets NO
// update gate — native updates stay rejected (fail-closed, see proxy.md §1).
contract ImplOnly {
    bytes32 internal constant _IMPL_SLOT =
        0x360894a13ba1a3210667c828492db98dca3e2076cc3735a920a3ca505d382bbc;

    function implementation() public view returns (address i) {
        assembly { i := sload(_IMPL_SLOT) }
    }
}

contract LibProxy {
    uint256 public value;

    function admin() public view returns (address) {
        return AdminLib.getAdmin();
    }

    function initAdmin(address a) public {
        require(AdminLib.getAdmin() == address(0), "already initialized");
        AdminLib.setAdmin(a);
    }

    function setValue(uint256 v) public { value = v; }
}

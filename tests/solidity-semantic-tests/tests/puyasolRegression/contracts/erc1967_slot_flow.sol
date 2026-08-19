// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// CUSTOM puya-sol regression — EIP-1967 slot-constant data flow (proxy.md §1).

// OZ ERC1967Utils body shape: the slot constant is let-bound before the
// sload/sstore. The let is folded so classification still fires.
contract LetSlot {
    bytes32 private constant _ADMIN_SLOT =
        0xb53127684a568b3173ae13b9f8a6016e243e63b6e8ee1178d6a717850b5d6103;

    function admin() public view returns (address a) {
        assembly {
            let s := _ADMIN_SLOT
            a := sload(s)
        }
    }

    function initAdmin(address a) public {
        require(admin() == address(0), "already initialized");
        assembly {
            let s := _ADMIN_SLOT
            sstore(s, a)
        }
    }
}

// Contract-valued admin (the transparent-proxy ProxyAdmin topology): a
// contract IDENTITY admin is the word bytes24 ++ app id, which the gate
// matches against that app's ESCROW address — an EOA can never satisfy
// either contract form.
contract SelfAdmin {
    bytes32 private constant _ADMIN_SLOT =
        0xb53127684a568b3173ae13b9f8a6016e243e63b6e8ee1178d6a717850b5d6103;

    function admin() public view returns (address a) {
        assembly { a := sload(_ADMIN_SLOT) }
    }

    // Identity form: the raw word IS bytes24(0) ++ itob(appId).
    function initAdminApp(uint256 appId) public {
        assembly { sstore(_ADMIN_SLOT, appId) }
    }

    // Escrow form: Solidity `address(this)` is the app's escrow account.
    function initAdminSelf() public {
        address self = address(this);
        assembly { sstore(_ADMIN_SLOT, self) }
    }
}

// OZ StorageSlot escape shape: the slot constant flows through a function
// parameter, so no sload/sstore site can classify it — the compiler warns
// that storage through the derived slot splits from the native proxy model.
library StorageSlotLike {
    struct AddressSlot { address value; }

    function getAddressSlot(bytes32 slot) internal pure
        returns (AddressSlot storage r)
    {
        assembly { r.slot := slot }
    }
}

contract EscapeSlot {
    bytes32 private constant _ADMIN_SLOT =
        0xb53127684a568b3173ae13b9f8a6016e243e63b6e8ee1178d6a717850b5d6103;

    function admin() public view returns (address) {
        return StorageSlotLike.getAddressSlot(_ADMIN_SLOT).value;
    }
}

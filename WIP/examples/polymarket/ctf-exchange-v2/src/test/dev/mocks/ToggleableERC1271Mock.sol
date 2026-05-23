// SPDX-License-Identifier: MIT
pragma solidity 0.8.34;

contract ToggleableERC1271Mock {
    address public signer;
    bool public disabled;

    bytes4 internal constant MAGIC_VALUE_1271 = 0x1626ba7e;

    constructor(address _signer) {
        signer = _signer;
    }

    function disable() external {
        disabled = true;
    }

    // AVM-PORT-ADAPTATION: see PUYA_BLOCKERS.md §2 — same r/s/v + ecrecover
    // workaround as ERC1271Mock and src/exchange/mixins/Signatures.sol.
    function isValidSignature(bytes32 hash, bytes memory signature) public view returns (bytes4) {
        require(!disabled, "disabled");
        if (signature.length != 65) return bytes4(0);
        bytes32 r;
        bytes32 s;
        uint8 v;
        assembly {
            r := mload(add(signature, 0x20))
            s := mload(add(signature, 0x40))
            v := byte(0, mload(add(signature, 0x60)))
        }
        address recovered = ecrecover(hash, v, r, s);
        if (recovered == address(0) || recovered != signer) return bytes4(0);
        return MAGIC_VALUE_1271;
    }

    function onERC1155Received(address, address, uint256, uint256, bytes calldata) external pure returns (bytes4) {
        return 0xf23a6e61;
    }

    function onERC1155BatchReceived(address, address, uint256[] calldata, uint256[] calldata, bytes calldata)
        external
        pure
        returns (bytes4)
    {
        return 0xbc197c81;
    }
}

// SPDX-License-Identifier: MIT
pragma solidity 0.8.34;

interface IERC20Min {
    function approve(address spender, uint256 amount) external returns (bool);
}

/// @title MakerWallet1271
/// @notice ERC-1271 maker wallet — bridges an Ethereum-style ECDSA signer to
/// a Polymarket order maker on AVM. Solves the "eth-key holders can't sign
/// Algorand approve" gap: each maker is wrapped in one of these contracts,
/// the contract holds pUSD on Algorand, and admin (test setup) calls
/// approveERC20 to grant the exchange spender (h1) allowance.
///
/// Order's `maker` and `signer` fields are this contract's address.
/// matchOrders' POLY_1271 path calls isValidSignature(orderHash, sig) which
/// recovers the eth signer and matches it against the stored owner.
contract MakerWallet1271 {
    address public owner; // eth-style ECDSA signer (20-byte, padded)
    address public admin;

    bytes4 internal constant MAGIC_VALUE_1271 = 0x1626ba7e;

    constructor(address _owner, address _admin) {
        owner = _owner;
        admin = _admin;
    }

    /// @notice Test-only: grant `_spender` allowance for `_amount` of `_token`.
    /// Caller must be `admin`. Used during test setup to plumb pUSD allowance
    /// for the exchange's collateral pull (h1.transferFromERC20).
    function approveERC20(address _token, address _spender, uint256 _amount) external {
        require(msg.sender == admin, "not admin");
        require(IERC20Min(_token).approve(_spender, _amount), "approve failed");
    }

    // AVM-PORT-ADAPTATION: see PUYA_BLOCKERS.md §2 — same r/s/v + ecrecover
    // workaround as ERC1271Mock and src/exchange/mixins/Signatures.sol.
    function isValidSignature(bytes32 hash, bytes memory signature) public view returns (bytes4) {
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
        if (recovered == address(0) || recovered != owner) return bytes4(0);
        return MAGIC_VALUE_1271;
    }

    // ERC1155 callbacks (so the wallet can hold outcome tokens after MERGE/redeem flows).
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

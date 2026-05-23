// SPDX-License-Identifier: BUSL-1.1
pragma solidity ^0.8.0;

import { EIP712 } from "@solady/src/utils/EIP712.sol";

import { IHashing } from "../interfaces/IHashing.sol";

import { Order, ORDER_TYPEHASH } from "../libraries/Structs.sol";

abstract contract Hashing is EIP712, IHashing {
    string internal constant DOMAIN_NAME = "Polymarket CTF Exchange";
    string internal constant DOMAIN_VERSION = "2";

    constructor() EIP712() { }

    function _domainNameAndVersion() internal pure override returns (string memory name, string memory version) {
        return (DOMAIN_NAME, DOMAIN_VERSION);
    }

    /// @notice Computes the hash for an order
    /// @param order - The order to be hashed
    function hashOrder(Order memory order) public view override returns (bytes32) {
        return _hashTypedData(_createStructHash(order));
    }

    /// @notice Creates the struct hash for an order
    /// @dev This does not include the signature; the signature is downstream of this hash
    // AVM-PORT-ADAPTATION: see PUYA_BLOCKERS.md §4. Original used solady's
    // gas-saving inline-asm pattern `keccak256(sub(order, 0x20), 0x180)`
    // which reads 32 bytes BEFORE the struct's memory pointer. puya-sol
    // doesn't bind Solidity memory-struct pointers to simulated AVM memory
    // offsets, so `sub(order, 0x20)` evaluates to 2^64-32 and the
    // extraction reverts. Fall back to `keccak256(abi.encode(...))` which
    // puya-sol translates cleanly.
    function _createStructHash(Order memory order) internal pure returns (bytes32) {
        return keccak256(
            abi.encode(
                ORDER_TYPEHASH,
                order.salt,
                order.maker,
                order.signer,
                order.tokenId,
                order.makerAmount,
                order.takerAmount,
                order.side,
                order.signatureType,
                order.timestamp,
                order.metadata,
                order.builder
            )
        );
    }
}

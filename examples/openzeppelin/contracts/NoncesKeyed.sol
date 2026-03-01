// SPDX-License-Identifier: MIT
// OpenZeppelin Contracts (last updated v5.2.0)
// Flattened: Nonces + NoncesKeyed
// Source: https://github.com/OpenZeppelin/openzeppelin-contracts/blob/master/contracts/utils/
// Modification: Nonces._nonces visibility changed from private to internal for testability

pragma solidity ^0.8.20;

// --- Nonces.sol (utils/Nonces.sol) ---

/**
 * @dev Provides tracking nonces for addresses. Nonces will only increment.
 */
abstract contract Nonces {
    /**
     * @dev The nonce used for an `account` is not the expected current nonce.
     */
    error InvalidAccountNonce(address account, uint256 currentNonce);

    mapping(address account => uint256) internal _nonces;

    /**
     * @dev Returns the next unused nonce for an address.
     */
    function nonces(address owner) public view virtual returns (uint256) {
        return _nonces[owner];
    }

    /**
     * @dev Consumes a nonce.
     *
     * Returns the current value and increments nonce.
     */
    function _useNonce(address owner) internal virtual returns (uint256) {
        // For each account, the nonce has an initial value of 0, can only be incremented by one, and cannot be
        // decremented or reset. This guarantees that the nonce never overflows.
        unchecked {
            // It is important to do x++ and not ++x here.
            return _nonces[owner]++;
        }
    }

    /**
     * @dev Same as {_useNonce} but checking that `nonce` is the next valid for `owner`.
     */
    function _useCheckedNonce(address owner, uint256 nonce) internal virtual {
        uint256 current = _useNonce(owner);
        if (nonce != current) {
            revert InvalidAccountNonce(owner, current);
        }
    }
}

// --- NoncesKeyed.sol (utils/NoncesKeyed.sol) ---

/**
 * @dev Alternative to {Nonces}, that supports key-ed nonces.
 *
 * Follows the ERC-4337 semi-abstracted nonce system.
 *
 * NOTE: This contract inherits from {Nonces} and reuses its storage for the first nonce key (i.e. `0`). This
 * makes upgrading from {Nonces} to {NoncesKeyed} safe when using their upgradeable versions.
 * Doing so will NOT reset the current state of nonces, avoiding replay attacks where a nonce is reused after the upgrade.
 */
abstract contract NoncesKeyed is Nonces {
    mapping(address owner => mapping(uint192 key => uint64)) internal _keyedNonces;

    /// @dev Returns the next unused nonce for an address and key. Result contains the key prefix.
    function nonces(address owner, uint192 key) public view virtual returns (uint256) {
        return key == 0 ? nonces(owner) : _pack(key, _keyedNonces[owner][key]);
    }

    /**
     * @dev Consumes the next unused nonce for an address and key.
     *
     * Returns the current value without the key prefix. Consumed nonce is increased, so calling this function twice
     * with the same arguments will return different (sequential) results.
     */
    function _useNonce(address owner, uint192 key) internal virtual returns (uint256) {
        // For each account, the nonce has an initial value of 0, can only be incremented by one, and cannot be
        // decremented or reset. This guarantees that the nonce never overflows.
        unchecked {
            // It is important to do x++ and not ++x here.
            return key == 0 ? _useNonce(owner) : _pack(key, _keyedNonces[owner][key]++);
        }
    }

    /**
     * @dev Same as {_useNonce} but checking that `nonce` is the next valid for `owner`.
     *
     * This version takes the key and the nonce in a single uint256 parameter:
     * - use the first 24 bytes for the key
     * - use the last 8 bytes for the nonce
     */
    function _useCheckedNonce(address owner, uint256 keyNonce) internal virtual override {
        (uint192 key, ) = _unpack(keyNonce);
        if (key == 0) {
            super._useCheckedNonce(owner, keyNonce);
        } else {
            uint256 current = _useNonce(owner, key);
            if (keyNonce != current) revert InvalidAccountNonce(owner, current);
        }
    }

    /**
     * @dev Same as {_useNonce} but checking that `nonce` is the next valid for `owner`.
     *
     * This version takes the key and the nonce as two different parameters.
     */
    function _useCheckedNonce(address owner, uint192 key, uint64 nonce) internal virtual {
        _useCheckedNonce(owner, _pack(key, nonce));
    }

    /// @dev Pack key and nonce into a keyNonce
    function _pack(uint192 key, uint64 nonce) private pure returns (uint256) {
        return (uint256(key) << 64) | nonce;
    }

    /// @dev Unpack a keyNonce into its key and nonce components
    function _unpack(uint256 keyNonce) private pure returns (uint192 key, uint64 nonce) {
        return (uint192(keyNonce >> 64), uint64(keyNonce));
    }
}

// Test contract — standard usage of OZ NoncesKeyed (not part of OZ source)
contract NoncesKeyedTest is NoncesKeyed {
    function initNonce(address owner) external {
        _nonces[owner] = 0;
    }

    function initKeyedNonce(address owner, uint192 key) external {
        _keyedNonces[owner][key] = 0;
    }

    function useNonce(address owner, uint192 key) external returns (uint256) {
        return _useNonce(owner, key);
    }

    function useCheckedNonce(address owner, uint256 nonce) external {
        _useCheckedNonce(owner, nonce);
    }

    function nonces(address owner) public view override returns (uint256) {
        return super.nonces(owner);
    }

    function nonces(address owner, uint192 key) public view override returns (uint256) {
        return super.nonces(owner, key);
    }
}

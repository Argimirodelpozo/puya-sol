// SPDX-License-Identifier: MIT
// OpenZeppelin Contracts (last updated v5.0.0) (utils/Nonces.sol)
// Source: https://github.com/OpenZeppelin/openzeppelin-contracts/blob/v5.0.0/contracts/utils/Nonces.sol
// Flattened with _nonces visibility changed from private to internal for testability

pragma solidity ^0.8.20;

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

// Test contract — standard usage of OZ Nonces (not part of OZ source)
contract NoncesTest is Nonces {
    // Initialize the nonce box for an owner (AVM boxes must be created before read+write)
    function initNonce(address owner) external {
        _nonces[owner] = 0;
    }

    function useNonce(address owner) external returns (uint256) {
        return _useNonce(owner);
    }

    function useCheckedNonce(address owner, uint256 nonce) external {
        _useCheckedNonce(owner, nonce);
    }

    function nonces(address owner) public view override returns (uint256) {
        return _nonces[owner];
    }
}

// SPDX-License-Identifier: MIT
// OpenZeppelin Contracts (last updated v5.0.0)
// Flattened: Context.sol + Pausable.sol
// Source: https://github.com/OpenZeppelin/openzeppelin-contracts/blob/v5.0.0/contracts/
// UNMODIFIED — exact copies with imports resolved

pragma solidity ^0.8.20;

// --- Context.sol (utils/Context.sol) ---

abstract contract Context {
    function _msgSender() internal view virtual returns (address) {
        return msg.sender;
    }

    function _msgData() internal view virtual returns (bytes calldata) {
        return msg.data;
    }
}

// --- Pausable.sol (utils/Pausable.sol) ---

abstract contract Pausable is Context {
    bool private _paused;

    event Paused(address account);
    event Unpaused(address account);

    error EnforcedPause();
    error ExpectedPause();

    constructor() {
        _paused = false;
    }

    modifier whenNotPaused() {
        _requireNotPaused();
        _;
    }

    modifier whenPaused() {
        _requirePaused();
        _;
    }

    function paused() public view virtual returns (bool) {
        return _paused;
    }

    function _requireNotPaused() internal view virtual {
        if (paused()) {
            revert EnforcedPause();
        }
    }

    function _requirePaused() internal view virtual {
        if (!paused()) {
            revert ExpectedPause();
        }
    }

    function _pause() internal virtual whenNotPaused {
        _paused = true;
        emit Paused(_msgSender());
    }

    function _unpause() internal virtual whenPaused {
        _paused = false;
        emit Unpaused(_msgSender());
    }
}

// Test contract — standard usage (not part of OZ source)
contract PausableTest is Pausable {
    function pause() public {
        _pause();
    }

    function unpause() public {
        _unpause();
    }

    function doSomething() public whenNotPaused returns (uint256) {
        return 42;
    }
}

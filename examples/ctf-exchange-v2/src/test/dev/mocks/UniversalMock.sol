// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

/// @dev Stand-in for any token-ish contract the v2 exchange constructor
/// touches during deploy. We don't model real ERC20/ERC1155 semantics —
/// each method is a no-op that returns "ok" so the inner-app calls
/// resolve without erroring. Used for the `collateral`, `ctf`,
/// `ctfCollateral`, `outcomeTokenFactory` slots in test deployments.
contract UniversalMock {
    /// @notice ERC20.approve — no-op, always returns true
    function approve(address, uint256) external pure returns (bool) {
        return true;
    }

    /// @notice ERC1155.setApprovalForAll — no-op
    function setApprovalForAll(address, bool) external pure {}

    /// @notice ERC20.transfer — no-op, returns true
    function transfer(address, uint256) external pure returns (bool) {
        return true;
    }

    /// @notice ERC20.transferFrom — no-op, returns true
    function transferFrom(address, address, uint256) external pure returns (bool) {
        return true;
    }

    /// @notice ERC20.balanceOf — returns max so balance checks pass
    function balanceOf(address) external pure returns (uint256) {
        return type(uint256).max;
    }

    /// @notice ERC1155.balanceOf — returns max so balance checks pass
    function balanceOf(address, uint256) external pure returns (uint256) {
        return type(uint256).max;
    }

    /// @notice ERC1155.safeTransferFrom — no-op
    function safeTransferFrom(address, address, uint256, uint256, bytes calldata) external pure {}

    /// @notice ProxyFactory/SafeFactory.getImplementation — return self.
    /// Used by the v2 exchange constructor to fetch proxy/safe impl
    /// addresses; we just return our own so any subsequent call lands
    /// back here as another no-op.
    function getImplementation() external view returns (address) {
        return address(this);
    }

    /// @notice Gnosis Safe factory's `proxyCreationCode()` getter — return
    /// a 1-byte placeholder so puya's runtime length checks don't panic.
    function proxyCreationCode() external pure returns (bytes memory) {
        return hex"00";
    }

    /// @notice Safe factory's `masterCopy()` — returns self
    function masterCopy() external view returns (address) {
        return address(this);
    }
}

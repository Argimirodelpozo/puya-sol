// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * M33: Cross-contract call via .call(abi.encodeCall(...)).
 * Tests the SafeTransferLib pattern where low-level .call() is used
 * with abi.encodeCall to invoke interface methods on another contract.
 */

interface IERC20 {
    function transfer(address to, uint256 amount) external returns (bool);
    function balanceOf(address account) external view returns (uint256);
}

/// Caller that uses .call(abi.encodeCall(...)) — the SafeTransferLib pattern.
contract SafeTransferCaller {
    function safeTransfer(address token, address to, uint256 amount) external {
        (bool success, ) = token.call(abi.encodeCall(IERC20.transfer, (to, amount)));
        require(success, "transfer failed");
    }
}

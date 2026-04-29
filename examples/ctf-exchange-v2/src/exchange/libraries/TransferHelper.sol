// SPDX-License-Identifier: BUSL-1.1
pragma solidity ^0.8.0;

import { ERC1155 } from "@solady/src/tokens/ERC1155.sol";

// AVM-PORT-ADAPTATION: see PUYA_BLOCKERS.md §3.
// Original v2 used solady's SafeTransferLib for ERC20 paths. solady's
// inline assembly `call(gas, token, 0, ptr, len, retPtr, retLen)` to a
// non-constant `token` doesn't currently translate to AVM `itxn
// ApplicationCall` — puya-sol's Yul `call` handler stubs the call as
// success without firing the inner-txn, so transfers silently no-op.
//
// Plain Solidity-interface calls translate cleanly. Use a minimal IERC20
// variant rather than pulling in solmate or forge-std.
interface IERC20Min {
    function transfer(address to, uint256 amount) external returns (bool);
    function transferFrom(address from, address to, uint256 amount) external returns (bool);
}

/// @title TransferHelper
/// @notice Helper method to transfer tokens
library TransferHelper {
    /// @notice Transfers tokens from msg.sender to a recipient
    function _transferERC20(address token, address to, uint256 amount) internal {
        require(IERC20Min(token).transfer(to, amount), "ERC20 transfer failed");
    }

    /// @notice Transfers tokens from the targeted address to the given destination
    function _transferFromERC20(address token, address from, address to, uint256 amount) internal {
        require(IERC20Min(token).transferFrom(from, to, amount), "ERC20 transferFrom failed");
    }

    /// @notice Transfer an ERC1155 token
    function _transferFromERC1155(address token, address from, address to, uint256 id, uint256 amount) internal {
        ERC1155(token).safeTransferFrom(from, to, id, amount, "");
    }

    /// @notice Transfers a set of ERC1155 tokens
    function _batchTransferFromERC1155(
        address token,
        address from,
        address to,
        uint256[] memory ids,
        uint256[] memory amounts
    ) internal {
        ERC1155(token).safeBatchTransferFrom(from, to, ids, amounts, "");
    }
}

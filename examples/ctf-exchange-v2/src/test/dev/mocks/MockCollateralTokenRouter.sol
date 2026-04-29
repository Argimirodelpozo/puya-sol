// SPDX-License-Identifier: MIT
pragma solidity 0.8.34;

import { SafeTransferLib } from "@solady/src/utils/SafeTransferLib.sol";

import { ICollateralTokenCallbacks } from "../../../collateral/interfaces/ICollateralTokenCallbacks.sol";

interface ICollateralTokenWrap {
    function wrap(address asset, address to, uint256 amount, address callbackReceiver, bytes calldata data) external;
    function unwrap(address asset, address to, uint256 amount, address callbackReceiver, bytes calldata data)
        external;
}

/// @notice Test-only router that drives CollateralToken wrap/unwrap with the
/// wrap/unwrapCallback flow. Mirrors the inline `MockCollateralTokenRouter`
/// in `src/test/CollateralToken.t.sol` so it can be deployed standalone on
/// AVM (Foundry tests can only declare contracts inside the .t.sol file —
/// AVM tests need a separate compilation unit).
contract MockCollateralTokenRouter is ICollateralTokenCallbacks {
    using SafeTransferLib for address;

    address public immutable collateralToken;

    constructor(address _collateralToken) {
        collateralToken = _collateralToken;
    }

    function wrap(address _asset, address _to, uint256 _amount) external {
        bytes memory data = abi.encode(msg.sender);
        ICollateralTokenWrap(collateralToken).wrap(_asset, _to, _amount, address(this), data);
    }

    function unwrap(address _asset, address _to, uint256 _amount) external {
        bytes memory data = abi.encode(msg.sender);
        ICollateralTokenWrap(collateralToken).unwrap(_asset, _to, _amount, address(this), data);
    }

    function wrapCallback(address _asset, address, uint256 _amount, bytes calldata _data) external {
        address from = abi.decode(_data, (address));
        _asset.safeTransferFrom(from, collateralToken, _amount);
    }

    function unwrapCallback(address, address, uint256 _amount, bytes calldata _data) external {
        address from = abi.decode(_data, (address));
        collateralToken.safeTransferFrom(from, collateralToken, _amount);
    }
}

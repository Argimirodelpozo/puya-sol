// SPDX-License-Identifier: MIT
pragma solidity 0.8.34;

// AVM-PORT-ADAPTATION: inline the callback interface rather than importing
// `../../../collateral/interfaces/ICollateralTokenCallbacks.sol` — puya-sol's
// import resolver doesn't allow upward-traversal through `../` past the
// source file's parent dir, so the relative import in the audited Foundry
// version of this mock can't be reused on AVM. The signatures must match
// `CollateralToken.sol`'s callback callsites exactly.
interface ICollateralTokenCallbacks {
    function wrapCallback(address _asset, address _to, uint256 _amount, bytes calldata _data) external;
    function unwrapCallback(address _asset, address _to, uint256 _amount, bytes calldata _data) external;
}

interface ICollateralTokenWrap {
    function wrap(address asset, address to, uint256 amount, address callbackReceiver, bytes calldata data) external;
    function unwrap(address asset, address to, uint256 amount, address callbackReceiver, bytes calldata data)
        external;
}

// AVM-PORT-ADAPTATION: matches CollateralToken.sol — Solady's SafeTransferLib
// emits inline-asm `call` to a non-constant token, which puya-sol stubs as
// success without firing an itxn. Plain interface calls translate cleanly.
interface IERC20Min {
    function transferFrom(address from, address to, uint256 amount) external returns (bool);
}

/// @notice Test-only router that drives CollateralToken wrap/unwrap with the
/// wrap/unwrapCallback flow. Mirrors the inline `MockCollateralTokenRouter`
/// in `src/test/CollateralToken.t.sol` so it can be deployed standalone on
/// AVM (Foundry tests can only declare contracts inside the .t.sol file —
/// AVM tests need a separate compilation unit).
///
/// AVM-PORT-ADAPTATION: stores two views of the collateral token's address.
/// `collateralToken` is the puya-sol storage encoding (`\x00*24 || itob(app_id)`)
/// — used as inner-tx targets so puya-sol's `extract_uint64` at offset 24
/// resolves to the app id. `collateralTokenAccount` is the real Algorand
/// 32-byte address (sha512_256("appID" || app_id)) — used as the recipient
/// arg of inner ERC20 calls so the receiver-mock credits the same storage
/// key that ct's later `usdc.transfer(VAULT, …)` reads from
/// (`balances[msg.sender]`, where msg.sender at the inner level is ct_real).
/// On EVM the two collapse into the same 20-byte value; on AVM they don't.
contract MockCollateralTokenRouter is ICollateralTokenCallbacks {
    address public immutable collateralToken;
    address public immutable collateralTokenAccount;

    constructor(address _collateralToken, address _collateralTokenAccount) {
        collateralToken = _collateralToken;
        collateralTokenAccount = _collateralTokenAccount;
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
        require(
            IERC20Min(_asset).transferFrom(from, collateralTokenAccount, _amount),
            "ERC20 transferFrom failed"
        );
    }

    function unwrapCallback(address, address, uint256 _amount, bytes calldata _data) external {
        address from = abi.decode(_data, (address));
        require(
            IERC20Min(collateralToken).transferFrom(from, collateralTokenAccount, _amount),
            "ERC20 transferFrom failed"
        );
    }
}

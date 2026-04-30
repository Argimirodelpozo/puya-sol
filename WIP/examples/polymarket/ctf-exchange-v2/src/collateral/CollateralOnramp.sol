// SPDX-License-Identifier: BUSL-1.1
pragma solidity 0.8.34;

import { OwnableRoles } from "@solady/src/auth/OwnableRoles.sol";

import { CollateralErrors } from "./abstract/CollateralErrors.sol";
import { Pausable } from "./abstract/Pausable.sol";

import { CollateralToken } from "./CollateralToken.sol";

// AVM-PORT-ADAPTATION: see PUYA_BLOCKERS.md §3 and CollateralToken.sol's
// matching note. Solady's SafeTransferLib emits inline-asm `call` to a
// non-constant `token`, which puya-sol's Yul handler stubs as success
// without firing the inner-txn — transfers silently no-op. A plain
// Solidity-interface call lowers cleanly to an itxn, so we use the same
// minimal IERC20 shape that CollateralToken / TransferHelper use.
interface IERC20Min {
    function transferFrom(address from, address to, uint256 amount) external returns (bool);
}

/// @title CollateralOnramp
/// @author Polymarket
/// @notice Onramp for the PolymarketCollateralToken
/// @notice ADMIN_ROLE: Admin
contract CollateralOnramp is OwnableRoles, CollateralErrors, Pausable {

    /*--------------------------------------------------------------
                                 STATE
    --------------------------------------------------------------*/

    /// @notice The collateral token address.
    address public immutable COLLATERAL_TOKEN;

    /*--------------------------------------------------------------
                              CONSTRUCTOR
    --------------------------------------------------------------*/

    /// @notice Deploys the onramp contract.
    /// @param _owner The contract owner.
    /// @param _admin The initial admin address.
    /// @param _collateralToken The collateral token address.
    constructor(address _owner, address _admin, address _collateralToken) {
        COLLATERAL_TOKEN = _collateralToken;

        _initializeOwner(_owner);
        _grantRoles(_admin, ADMIN_ROLE);
    }

    /*--------------------------------------------------------------
                                EXTERNAL
    --------------------------------------------------------------*/

    /// @notice Wraps a supported asset into the collateral token
    /// @param _asset The asset to wrap
    /// @param _to The address to wrap the asset to
    /// @param _amount The amount of asset to wrap
    /// @dev The asset must not be paused
    function wrap(address _asset, address _to, uint256 _amount) external onlyUnpaused(_asset) {
        // AVM-PORT-ADAPTATION: see the IERC20Min note above; was
        // `_asset.safeTransferFrom(msg.sender, COLLATERAL_TOKEN, _amount);`.
        require(
            IERC20Min(_asset).transferFrom(msg.sender, COLLATERAL_TOKEN, _amount),
            "ERC20 transferFrom failed"
        );
        // forgefmt: disable-next-item
        CollateralToken(COLLATERAL_TOKEN).wrap({
            _asset: _asset,
            _to: _to,
            _amount: _amount,
            _callbackReceiver: address(0),
            _data: ""
        });
    }

    /*--------------------------------------------------------------
                               ONLY ADMIN
    --------------------------------------------------------------*/

    /// @notice Adds a new admin to the contract
    /// @param _admin The address of the new admin
    function addAdmin(address _admin) external onlyRoles(ADMIN_ROLE) {
        _grantRoles(_admin, ADMIN_ROLE);
    }

    /// @notice Removes an admin from the contract
    /// @param _admin The address of the admin to remove
    function removeAdmin(address _admin) external onlyRoles(ADMIN_ROLE) {
        _removeRoles(_admin, ADMIN_ROLE);
    }
}

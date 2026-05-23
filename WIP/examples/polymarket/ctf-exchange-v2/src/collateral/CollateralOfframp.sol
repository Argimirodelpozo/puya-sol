// SPDX-License-Identifier: BUSL-1.1
pragma solidity 0.8.34;

import { OwnableRoles } from "@solady/src/auth/OwnableRoles.sol";

import { CollateralErrors } from "./abstract/CollateralErrors.sol";
import { Pausable } from "./abstract/Pausable.sol";

import { CollateralToken } from "./CollateralToken.sol";

// AVM-PORT-ADAPTATION: see PUYA_BLOCKERS.md §3 and CollateralToken.sol's
// matching note. Solady SafeTransferLib's inline-asm `call` doesn't lower
// to an itxn on AVM (silently no-ops); a plain Solidity-interface call
// does. Mirroring CollateralToken/Onramp's IERC20Min shim.
interface IERC20Min {
    function transferFrom(address from, address to, uint256 amount) external returns (bool);
}

/// @title CollateralOfframp
/// @author Polymarket
/// @notice Offramp for the PolymarketCollateralToken
/// @notice ADMIN_ROLE: Admin
contract CollateralOfframp is OwnableRoles, CollateralErrors, Pausable {

    /*--------------------------------------------------------------
                                 STATE
    --------------------------------------------------------------*/

    /// @notice The collateral token address.
    address public immutable COLLATERAL_TOKEN;

    /*--------------------------------------------------------------
                              CONSTRUCTOR
    --------------------------------------------------------------*/

    /// @notice Deploys the offramp contract.
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

    /// @notice Unwraps a supported asset from the collateral token
    /// @param _asset The asset to unwrap
    /// @param _to The address to unwrap the asset to
    /// @param _amount The amount of asset to unwrap
    /// @dev The asset must not be paused
    function unwrap(address _asset, address _to, uint256 _amount) external onlyUnpaused(_asset) {
        // AVM-PORT-ADAPTATION: see the IERC20Min note above; was
        // `COLLATERAL_TOKEN.safeTransferFrom(msg.sender, COLLATERAL_TOKEN, _amount);`.
        // _avmAlgodAddrFor: see CollateralOnramp.sol for the rationale.
        require(
            IERC20Min(COLLATERAL_TOKEN).transferFrom(msg.sender, _avmAlgodAddrFor(COLLATERAL_TOKEN), _amount),
            "ERC20 transferFrom failed"
        );
        // forgefmt: disable-next-item
        CollateralToken(COLLATERAL_TOKEN).unwrap({
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

    /*--------------------------------------------------------------
                              AVM-PORT-ADAPTATION
    --------------------------------------------------------------*/

    /// @dev AVM-PORT-ADAPTATION: puya-sol intercepts this call.
    /// See CollateralOnramp.sol for the full rationale.
    function _avmAlgodAddrFor(address app) internal pure returns (address) {
        return app;
    }
}

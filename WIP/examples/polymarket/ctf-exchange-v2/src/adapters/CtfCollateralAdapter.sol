// SPDX-License-Identifier: BUSL-1.1
pragma solidity 0.8.34;

import { IConditionalTokens } from "./interfaces/IConditionalTokens.sol";
import { CTFHelpers } from "./libraries/CTFHelpers.sol";
import { CollateralToken } from "../collateral/CollateralToken.sol";
import { Pausable } from "../collateral/abstract/Pausable.sol";
import { ERC1155TokenReceiver } from "../exchange/mixins/ERC1155TokenReceiver.sol";

// AVM-PORT-ADAPTATION: see PUYA_BLOCKERS.md §3 / TransferHelper.sol /
// CollateralToken.sol. Solady's SafeTransferLib emits inline-asm `call`
// to a non-constant token, which puya-sol stubs as success without
// firing an itxn — transfers + approvals silently no-op. Plain
// interface calls translate cleanly.
interface IERC20Min {
    function approve(address spender, uint256 amount) external returns (bool);
    function balanceOf(address account) external view returns (uint256);
    function transfer(address to, uint256 amount) external returns (bool);
    function transferFrom(address from, address to, uint256 amount) external returns (bool);
}

/// @title CtfCollateralAdapter
/// @author Polymarket
/// @notice An adapter for interfacing with ConditionalTokens Markets
///         using the PolymarketCollateralToken
contract CtfCollateralAdapter is Pausable, ERC1155TokenReceiver {

    /*--------------------------------------------------------------
                                 STATE
    --------------------------------------------------------------*/

    /// @notice The legacy Conditional Tokens Framework contract.
    IConditionalTokens public immutable CONDITIONAL_TOKENS;

    /// @notice The collateral token (PMCT) contract address.
    address public immutable COLLATERAL_TOKEN;

    /// @notice The USDC.e token address.
    address public immutable USDCE;

    /*--------------------------------------------------------------
                              CONSTRUCTOR
    --------------------------------------------------------------*/

    /// @notice Deploys the CTF collateral adapter.
    /// @param _owner The contract owner.
    /// @param _admin The initial admin address.
    /// @param _conditionalTokens The legacy CTF contract address.
    /// @param _collateralToken The collateral token (PMCT) address.
    /// @param _usdce The USDC.e token address.
    constructor(address _owner, address _admin, address _conditionalTokens, address _collateralToken, address _usdce) {
        CONDITIONAL_TOKENS = IConditionalTokens(_conditionalTokens);
        COLLATERAL_TOKEN = _collateralToken;
        USDCE = _usdce;

        _initializeOwner(_owner);
        _grantRoles(_admin, ADMIN_ROLE);

        require(IERC20Min(_usdce).approve(_conditionalTokens, type(uint256).max), "ERC20 approve failed");
    }

    /*--------------------------------------------------------------
                                EXTERNAL
    --------------------------------------------------------------*/

    /// @notice Splits collateral into conditional token positions
    /// @dev Unnamed params retained for IConditionalTokens interface compatibility
    /// @param _conditionId The condition ID to split on
    /// @param _amount The amount of collateral to split
    function splitPosition(address, bytes32, bytes32 _conditionId, uint256[] calldata, uint256 _amount)
        external
        onlyUnpaused(USDCE)
    {
        require(
            IERC20Min(COLLATERAL_TOKEN).transferFrom(msg.sender, COLLATERAL_TOKEN, _amount),
            "ERC20 transferFrom failed"
        );
        // forgefmt: disable-next-item
        CollateralToken(COLLATERAL_TOKEN).unwrap({
            _asset: USDCE,
            _to: address(this),
            _amount: _amount,
            _callbackReceiver: address(0),
            _data: ""
        });

        _splitPosition(_conditionId, _amount);

        uint256[] memory positionIds = _getPositionIds(_conditionId);
        uint256[] memory amounts = new uint256[](2);
        amounts[0] = _amount;
        amounts[1] = _amount;

        CONDITIONAL_TOKENS.safeBatchTransferFrom(address(this), msg.sender, positionIds, amounts, "");
    }

    /// @notice Merges conditional token positions back into collateral
    /// @dev Unnamed params retained for IConditionalTokens interface compatibility
    /// @param _conditionId The condition ID to merge on
    /// @param _amount The amount of each position to merge
    function mergePositions(address, bytes32, bytes32 _conditionId, uint256[] calldata, uint256 _amount)
        external
        onlyUnpaused(USDCE)
    {
        uint256[] memory positionIds = _getPositionIds(_conditionId);

        uint256[] memory amounts = new uint256[](2);
        amounts[0] = _amount;
        amounts[1] = _amount;

        CONDITIONAL_TOKENS.safeBatchTransferFrom(msg.sender, address(this), positionIds, amounts, "");

        _mergePositions(_conditionId, _amount);

        require(IERC20Min(USDCE).transfer(COLLATERAL_TOKEN, _amount), "ERC20 transfer failed");
        // forgefmt: disable-next-item
        CollateralToken(COLLATERAL_TOKEN).wrap({
            _asset: USDCE,
            _to: msg.sender,
            _amount: _amount,
            _callbackReceiver: address(0),
            _data: ""
        });
    }

    /// @notice Redeems conditional token positions for collateral after resolution
    /// @dev Unnamed params retained for IConditionalTokens interface compatibility
    /// @param _conditionId The condition ID to redeem
    function redeemPositions(address, bytes32, bytes32 _conditionId, uint256[] calldata) external onlyUnpaused(USDCE) {
        uint256[] memory positionIds = _getPositionIds(_conditionId);

        uint256[] memory amounts = new uint256[](2);
        amounts[0] = CONDITIONAL_TOKENS.balanceOf(msg.sender, positionIds[0]);
        amounts[1] = CONDITIONAL_TOKENS.balanceOf(msg.sender, positionIds[1]);

        CONDITIONAL_TOKENS.safeBatchTransferFrom(msg.sender, address(this), positionIds, amounts, "");

        _redeemPositions(_conditionId, CTFHelpers.partition());

        uint256 amount = IERC20Min(USDCE).balanceOf(address(this));

        require(IERC20Min(USDCE).transfer(COLLATERAL_TOKEN, amount), "ERC20 transfer failed");
        // forgefmt: disable-next-item
        CollateralToken(COLLATERAL_TOKEN).wrap({
            _asset: USDCE,
            _to: msg.sender,
            _amount: amount,
            _callbackReceiver: address(0),
            _data: ""
        });
    }

    /*--------------------------------------------------------------
                               ONLY ADMIN
    --------------------------------------------------------------*/

    /// @notice Adds a new admin
    /// @param _admin The address of the new admin
    function addAdmin(address _admin) external onlyRoles(ADMIN_ROLE) {
        _grantRoles(_admin, ADMIN_ROLE);
    }

    /// @notice Removes an admin
    /// @param _admin The address of the admin to remove
    function removeAdmin(address _admin) external onlyRoles(ADMIN_ROLE) {
        _removeRoles(_admin, ADMIN_ROLE);
    }

    /*--------------------------------------------------------------
                                INTERNAL
    --------------------------------------------------------------*/

    /// @dev Returns the YES and NO position IDs for a condition.
    function _getPositionIds(bytes32 _conditionId) internal view virtual returns (uint256[] memory) {
        return CTFHelpers.positionIds(USDCE, _conditionId);
    }

    /// @dev Splits collateral into positions via the legacy CTF.
    function _splitPosition(bytes32 _conditionId, uint256 _amount) internal virtual {
        // forgefmt: disable-next-item
        CONDITIONAL_TOKENS.splitPosition({
            collateralToken: USDCE,
            parentCollectionId: bytes32(0),
            conditionId: _conditionId,
            partition: CTFHelpers.partition(),
            amount: _amount
        });
    }

    /// @dev Merges positions back into collateral via the legacy CTF.
    function _mergePositions(bytes32 _conditionId, uint256 _amount) internal virtual {
        // forgefmt: disable-next-item
        CONDITIONAL_TOKENS.mergePositions({
            collateralToken: USDCE,
            parentCollectionId: bytes32(0),
            conditionId: _conditionId,
            partition: CTFHelpers.partition(),
            amount: _amount
        });
    }

    /// @dev Redeems resolved positions via the legacy CTF.
    function _redeemPositions(bytes32 _conditionId, uint256[] memory indexSets) internal virtual {
        // forgefmt: disable-next-item
        CONDITIONAL_TOKENS.redeemPositions({
            collateralToken: USDCE,
            parentCollectionId: bytes32(0),
            conditionId: _conditionId,
            indexSets: indexSets
        });
    }
}

// SPDX-License-Identifier: BUSL-1.1
pragma solidity ^0.8.0;

import { ERC20 } from "@solady/src/tokens/ERC20.sol";
import { ERC1155 } from "@solady/src/tokens/ERC1155.sol";

import { IAssets } from "../interfaces/IAssets.sol";
import { IERC20Min } from "../libraries/TransferHelper.sol";

abstract contract Assets is IAssets {
    /// @notice The Collateral token address
    address internal immutable collateral;

    /// @notice The Conditional Tokens Framework address
    address internal immutable ctf;

    /// @notice The collateral address used by the CTF for position ID derivation
    address internal immutable ctfCollateral;

    /// @notice The address that facilitates Outcome Token minting or merging
    address internal immutable outcomeTokenFactory;

    constructor(address _collateral, address _ctf, address _ctfCollateral, address _outcomeTokenFactory) {
        collateral = _collateral;
        ctf = _ctf;
        ctfCollateral = _ctfCollateral;
        outcomeTokenFactory = _outcomeTokenFactory;
        // AVM-PORT-ADAPTATION: route approve through IERC20Min so the
        // selector stays at `approve(address,uint256)bool`. ERC20-as-Solady
        // would compile to `approve(address,uint512)bool` — that breaks
        // dispatch when the collateral slot is a Solady-derived
        // (puya-sol-compiled) contract that exposes the uint512 selector.
        // Solady-compiled contracts add a uint256 IERC20Min-shim approve.
        require(
            IERC20Min(_collateral).approve(_outcomeTokenFactory, type(uint256).max),
            "ERC20 approve failed"
        );
        ERC1155(_ctf).setApprovalForAll(_outcomeTokenFactory, true);
    }

    /// @notice Returns the collateral token address
    function getCollateral() public view override returns (address) {
        return collateral;
    }

    /// @notice Returns the Conditional Tokens Framework address
    function getCtf() public view override returns (address) {
        return ctf;
    }

    /// @notice Returns the collateral address used by the CTF for position ID derivation
    function getCtfCollateral() public view override returns (address) {
        return ctfCollateral;
    }

    /// @notice Returns the address that facilitates outcome token minting or merging
    function getOutcomeTokenFactory() public view override returns (address) {
        return outcomeTokenFactory;
    }
}

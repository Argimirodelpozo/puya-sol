// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/tokens/ERC4626.sol";

contract ERC4626Wrapper is ERC4626 {
    address private _asset;

    constructor(address asset_) {
        _asset = asset_;
    }

    function name() public pure override returns (string memory) {
        return "Vault Token";
    }

    function symbol() public pure override returns (string memory) {
        return "vTKN";
    }

    function asset() public view override returns (address) {
        return _asset;
    }

    function getTotal() external view returns (uint256) {
        return totalSupply();
    }

    function getAssets(uint256 shares) external view returns (uint256) {
        return convertToAssets(shares);
    }

    function getShares(uint256 assets) external view returns (uint256) {
        return convertToShares(assets);
    }
}

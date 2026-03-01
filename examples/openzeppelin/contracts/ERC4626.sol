// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Simplified ERC4626 vault demonstrating deposit/withdraw pattern.
 * 1:1 share-to-asset ratio for simplicity.
 */
contract ERC4626Test {
    string private _name;
    string private _symbol;

    mapping(address => uint256) private _shares;
    uint256 private _totalShares;
    uint256 private _totalAssets;

    constructor() {
        _name = "Vault Token";
        _symbol = "vTKN";
    }

    function name() external view returns (string memory) {
        return _name;
    }

    function symbol() external view returns (string memory) {
        return _symbol;
    }

    function totalSupply() external view returns (uint256) {
        return _totalShares;
    }

    function totalAssets() external view returns (uint256) {
        return _totalAssets;
    }

    function balanceOf(address account) external view returns (uint256) {
        return _shares[account];
    }

    // Preview how many shares for given assets (1:1 ratio)
    function convertToShares(uint256 assets) external pure returns (uint256) {
        return assets;
    }

    // Preview how many assets for given shares (1:1 ratio)
    function convertToAssets(uint256 shares) external pure returns (uint256) {
        return shares;
    }

    // Maximum deposit amount (unlimited)
    function maxDeposit(address) external pure returns (uint256) {
        return type(uint256).max;
    }

    // Deposit assets and receive shares
    function deposit(uint256 assets, address receiver) external returns (uint256) {
        require(assets > 0, "ERC4626: zero deposit");
        uint256 shares = assets;  // 1:1 ratio
        _shares[receiver] += shares;
        _totalShares += shares;
        _totalAssets += assets;
        return shares;
    }

    // Withdraw assets by burning shares
    function withdraw(uint256 assets, address receiver, address owner) external returns (uint256) {
        require(assets > 0, "ERC4626: zero withdraw");
        uint256 shares = assets;  // 1:1 ratio
        require(_shares[owner] >= shares, "ERC4626: insufficient shares");
        _shares[owner] -= shares;
        _totalShares -= shares;
        _totalAssets -= assets;
        // In a real vault, this would transfer the underlying asset
        return shares;
    }

    // Redeem shares for assets
    function redeem(uint256 shares, address receiver, address owner) external returns (uint256) {
        require(shares > 0, "ERC4626: zero redeem");
        require(_shares[owner] >= shares, "ERC4626: insufficient shares");
        uint256 assets = shares;  // 1:1 ratio
        _shares[owner] -= shares;
        _totalShares -= shares;
        _totalAssets -= assets;
        return assets;
    }
}

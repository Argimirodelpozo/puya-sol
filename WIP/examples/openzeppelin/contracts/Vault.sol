// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Vault {
    address private admin;
    uint256 private totalAssets;
    uint256 private totalShares;
    uint256 private depositorCount;

    mapping(address => uint256) internal _shareBalance;
    mapping(address => uint256) internal _assetDeposited;
    mapping(address => uint256) internal _depositorIndex; // 1-based

    constructor() {
        admin = msg.sender;
    }

    function deposit(address depositor, uint256 assets) public returns (uint256) {
        require(assets > 0, "Vault: zero assets");

        uint256 shares;
        if (totalAssets == 0) {
            shares = assets;
        } else {
            shares = (assets * totalShares) / totalAssets;
        }
        require(shares > 0, "Vault: zero shares");

        _shareBalance[depositor] += shares;
        _assetDeposited[depositor] += assets;
        totalAssets += assets;
        totalShares += shares;

        if (_depositorIndex[depositor] == 0) {
            depositorCount += 1;
            _depositorIndex[depositor] = depositorCount;
        }

        return shares;
    }

    function withdraw(address depositor, uint256 shares) public returns (uint256) {
        require(shares > 0, "Vault: zero shares");
        require(_shareBalance[depositor] >= shares, "Vault: insufficient shares");

        uint256 assets = (shares * totalAssets) / totalShares;

        _shareBalance[depositor] -= shares;
        _assetDeposited[depositor] -= assets;
        totalAssets -= assets;
        totalShares -= shares;

        return assets;
    }

    function shareBalanceOf(address who) public view returns (uint256) {
        return _shareBalance[who];
    }

    function assetsOf(address who) public view returns (uint256) {
        if (totalShares == 0) {
            return 0;
        }
        return (_shareBalance[who] * totalAssets) / totalShares;
    }

    function getTotalAssets() public view returns (uint256) {
        return totalAssets;
    }

    function getTotalShares() public view returns (uint256) {
        return totalShares;
    }

    function getDepositorCount() public view returns (uint256) {
        return depositorCount;
    }

    function previewDeposit(uint256 assets) public view returns (uint256) {
        if (totalAssets == 0) {
            return assets;
        }
        return (assets * totalShares) / totalAssets;
    }

    function previewWithdraw(uint256 shares) public view returns (uint256) {
        if (totalShares == 0) {
            return 0;
        }
        return (shares * totalAssets) / totalShares;
    }

    function getAdmin() public view returns (address) {
        return admin;
    }
}

contract VaultTest is Vault {
    constructor() Vault() {}

    function initDepositor(address depositor) public {
        _shareBalance[depositor] = 0;
        _assetDeposited[depositor] = 0;
        _depositorIndex[depositor] = 0;
    }
}

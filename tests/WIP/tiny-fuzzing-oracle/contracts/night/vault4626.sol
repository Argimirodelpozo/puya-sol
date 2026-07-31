// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract Vault4626 {
    uint256 public totalAssets;
    uint256 public totalShares;
    mapping(address => uint256) public shares;
    function _mulDiv(uint256 x, uint256 y, uint256 d) internal pure returns (uint256) { return (x * y) / d; }
    function convertToShares(uint256 assets) public view returns (uint256) {
        return totalShares == 0 ? assets : _mulDiv(assets, totalShares, totalAssets);
    }
    function convertToAssets(uint256 sh) public view returns (uint256) {
        return totalShares == 0 ? sh : _mulDiv(sh, totalAssets, totalShares);
    }
    function deposit(uint256 assets, address to) external returns (uint256 sh) {
        sh = convertToShares(assets);
        totalAssets += assets; totalShares += sh; shares[to] += sh;
    }
    function mint(uint256 sh, address to) external returns (uint256 assets) {
        assets = totalShares == 0 ? sh : _mulDiv(sh, totalAssets, totalShares);
        if (_mulDiv(assets, totalShares, totalAssets == 0 ? 1 : totalAssets) < sh && totalShares != 0) assets += 1; // roundUp
        totalAssets += assets; totalShares += sh; shares[to] += sh;
    }
    function withdraw(uint256 assets, address from) external returns (uint256 sh) {
        sh = totalAssets == 0 ? 0 : _mulDiv(assets, totalShares, totalAssets);
        shares[from] -= sh; totalShares -= sh; totalAssets -= assets;
    }
    function redeem(uint256 sh, address from) external returns (uint256 assets) {
        assets = convertToAssets(sh);
        shares[from] -= sh; totalShares -= sh; totalAssets -= assets;
    }
    function yield_(uint256 gain) external { totalAssets += gain; }
}

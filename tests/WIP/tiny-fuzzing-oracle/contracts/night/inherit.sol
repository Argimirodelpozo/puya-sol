// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
abstract contract Base {
    uint256 public v;
    modifier addsOne() { v += 1; _; v += 10; }
    function step(uint256 x) public virtual returns (uint256) { v += x; return v; }
    function label() public pure virtual returns (uint256) { return 1; }
}
abstract contract Mid is Base {
    modifier doubles() { v *= 2; _; }
    function step(uint256 x) public virtual override returns (uint256) { return super.step(x) + 100; }
    function label() public pure virtual override returns (uint256) { return super.label() + 10; }
}
contract Leaf is Mid {
    function step(uint256 x) public override addsOne doubles returns (uint256) { return super.step(x); }
    function label() public pure override returns (uint256) { return super.label() + 100; }
    function callStep(uint256 x) external returns (uint256) { return step(x); }
    function getLabel() external pure returns (uint256) { return label(); }
    function reset() external { v = 0; }
}

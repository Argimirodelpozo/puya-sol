// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Registration intentionally does not depend on an OpenZeppelin spelling.
/// @custom:avm-proxy uups
abstract contract NativeUpgradeBase {
    function _authorizeUpgrade(address) internal virtual;
    function _checkProxy() internal view virtual { require(address(this) != address(0)); }
    function upgradeTo(address proposed) external { _checkProxy(); _authorizeUpgrade(proposed); }
}

abstract contract OwnerPolicy is NativeUpgradeBase {
    address internal owner;
    uint256 public checks;
    function initialize() external { require(owner == address(0)); owner = msg.sender; }
    modifier onlyOwner() { require(msg.sender == owner); checks += 1; _; checks += 10; }
    // A real overload, deliberately before the address hook in source order.
    function _authorizeUpgrade(uint256 n) internal pure returns (uint256) { return n + 7; }
    function overload() external pure returns (uint256) { return _authorizeUpgrade(uint256(5)); }
    function _authorizeUpgrade(address) internal virtual override onlyOwner { checks += 100; }
}

abstract contract SecondPolicy is NativeUpgradeBase {
    function _authorizeUpgrade(address) internal virtual override { require(msg.sender != address(0)); }
}

contract DiamondPolicy is OwnerPolicy, SecondPolicy {
    function _authorizeUpgrade(address) internal override(OwnerPolicy, SecondPolicy) onlyOwner {
        checks += 1000;
    }
}

// Native lifecycle itself is a reachability root: no EVM entry calls this hook.
/// @custom:avm-proxy uups
abstract contract LifecycleOnlyBase {
    function _authorizeUpgrade(address) internal virtual;
}

contract LifecycleOnly is LifecycleOnlyBase {
    address private owner;
    uint256 public checks;
    function initialize() external { require(owner == address(0)); owner = msg.sender; }
    function _authorizeUpgrade(address) internal override {
        require(msg.sender == owner);
        ++checks;
    }
}

contract AdminPolicy {
    bytes32 private constant ADMIN = 0xb53127684a568b3173ae13b9f8a6016e243e63b6e8ee1178d6a717850b5d6103;
    function admin() public view returns (address value) { assembly { value := sload(ADMIN) } }
    function initialize() external {
        require(admin() == address(0));
        address sender = msg.sender;
        assembly { sstore(ADMIN, sender) }
    }
    function changeAdmin(address value) external {
        require(msg.sender == admin());
        assembly { sstore(ADMIN, value) }
    }
    function value() external pure returns (uint256) { return 17; }
}

contract AdminConstructor is AdminPolicy {
    uint256[] private initialized;
    constructor() { initialized.push(1); }
}

contract AdminFallback is AdminPolicy {
    fallback() external {}
    receive() external payable {}
}

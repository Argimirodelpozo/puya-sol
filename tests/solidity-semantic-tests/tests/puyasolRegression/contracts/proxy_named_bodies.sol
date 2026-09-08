// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Names alone must not change ordinary source semantics in default builds.
library ERC1967Utils {
    function getImplementation() internal pure returns (address) { return address(17); }
    function getAdmin() internal pure returns (address) { return address(23); }
    function getBeacon() internal pure returns (address) { return address(29); }
}

abstract contract Proxy {
    function _delegate(address) internal pure returns (uint256) { return 37; }
}

abstract contract UUPSUpgradeable {
    uint256 public count;

    function _checkProxy() internal { count += 1; }
    function _checkNotDelegated() internal { count += 2; }
    function _authorizeUpgrade(address) internal pure {}
    function upgradeToAndCall(address, bytes memory) public { count = 31; }
}

contract ProxyNamedBodies is Proxy, UUPSUpgradeable {
    function check() external returns (uint256) {
        _checkProxy();
        _checkNotDelegated();
        return count;
    }

    function delegatedValue() external pure returns (uint256) {
        return _delegate(address(0));
    }

    function libraryValues() external pure returns (address, address, address) {
        return (ERC1967Utils.getImplementation(), ERC1967Utils.getAdmin(), ERC1967Utils.getBeacon());
    }
}

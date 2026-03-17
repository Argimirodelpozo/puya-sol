// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

contract Base {
    uint256 public baseValue;

    function setBase(uint256 v) external virtual {
        baseValue = v;
    }

    function getBase() external view returns (uint256) {
        return baseValue;
    }
}

contract Child is Base {
    uint256 public childValue;

    function setBase(uint256 v) external override {
        baseValue = v * 2;  // override: doubles the value
    }

    function setChild(uint256 v) external {
        childValue = v;
    }

    function getBoth() external view returns (uint256, uint256) {
        return (baseValue, childValue);
    }
}

abstract contract AbstractBase {
    function abstractMethod() external virtual returns (uint256);
}

contract ConcreteChild is AbstractBase {
    function abstractMethod() external pure override returns (uint256) {
        return 42;
    }
}

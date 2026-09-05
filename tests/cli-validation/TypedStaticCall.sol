// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

interface StaticTarget {
    function read() external view returns (uint256);
    function pureRead() external pure returns (uint256);
    function write() external returns (uint256);
}

contract StaticGetterTarget {
    uint256 public value;
}

contract TypedStaticCall {
    function viaView(StaticTarget target) external view returns (uint256) {
        return target.read{gas: 100000}();
    }
    function viaPure(StaticTarget target) external view returns (uint256) {
        return target.pureRead();
    }
    function viaGetter(StaticGetterTarget target) external view returns (uint256) {
        return target.value();
    }
    function viaPointer(function () external view returns (uint256) pointer)
        external view returns (uint256)
    {
        return pointer();
    }
    // Neither ordinary external calls nor pure internal calls are static calls.
    function mutating(StaticTarget target) external returns (uint256) {
        return target.write();
    }
    function local() external pure returns (uint256) { return inner(); }
    function inner() internal pure returns (uint256) { return 1; }
}

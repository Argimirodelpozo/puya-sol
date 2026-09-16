pragma solidity ^0.8.20;

contract FallbackProbe {
    fallback(bytes calldata) external returns (bytes memory) {
        return hex"aabbcc";
    }

    function ping() external pure returns (uint256) { return 42; }

    function test() external returns (bool, uint256, uint256, uint256) {
        (bool first,) = address(this).call(abi.encodeCall(this.ping, ()));
        uint256 beforeSize;
        assembly { beforeSize := returndatasize() }
        (bool second, bytes memory output) = address(this).call(hex"deadbeef");
        uint256 afterSize;
        assembly { afterSize := returndatasize() }
        return (first && second, beforeSize, output.length, afterSize);
    }
}

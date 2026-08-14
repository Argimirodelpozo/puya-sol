pragma solidity ^0.8.20;

contract OnDemand {
    function target() internal pure returns (uint256) { return 7; }

    function unusedFunctionPointer() external pure returns (uint256) {
        function() internal pure returns (uint256) pointer = target;
        pointer;
        return 1;
    }

    function unusedRecursiveYul() external pure returns (uint256 value) {
        assembly {
            function recurse(x) -> y { y := recurse(x) }
            value := 1
        }
    }

    function foldedRipemd() external pure returns (bytes20) {
        return ripemd160("");
    }
}

// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract ArrayOperandTarget {
    function observe(uint256[] calldata values, uint256 stamp) external pure returns (uint256) {
        return values[0] + stamp * 10;
    }
}

contract AbiReferenceOperands {
    function change(uint256[] memory values) internal pure returns (uint256) {
        values[0] = 9;
        return 1;
    }
    function externalArgument(ArrayOperandTarget target) external returns (uint256) {
        uint256[] memory values = new uint256[](1);
        values[0] = 2;
        return target.observe(values, change(values));
    }
    function encodedArgument() external pure returns (uint256) {
        uint256[] memory values = new uint256[](1);
        values[0] = 2;
        (uint256[] memory result, uint256 stamp) = abi.decode(abi.encode(values, change(values)), (uint256[], uint256));
        return result[0] + stamp * 10;
    }
    function bareArgument(ArrayOperandTarget target) external returns (uint256) {
        uint256[] memory values = new uint256[](1);
        values[0] = 2;
        (bool ok, bytes memory result) = address(target).call(abi.encodeCall(target.observe, (values, change(values))));
        require(ok);
        return abi.decode(result, (uint256));
    }
}

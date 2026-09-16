pragma solidity ^0.8.20;

contract A {
    function f(uint256[] calldata values) external pure virtual returns (uint256) {
        return values[0];
    }
}

contract B is A {
    function f(uint256[] memory values) public pure override returns (uint256) {
        return values[0] + 1;
    }

    function test(address other, uint256[] calldata values) external view returns (uint256) {
        function(uint256[] memory) external pure returns (uint256) target = A(other).f;
        return target(values);
    }
}

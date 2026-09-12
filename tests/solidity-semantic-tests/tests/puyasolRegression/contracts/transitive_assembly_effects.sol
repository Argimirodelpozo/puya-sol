pragma solidity ^0.8.20;

contract AssemblyEffectsBase {
    function touch(uint256[1] memory value) internal pure virtual {
        assembly { mstore(value, 7) }
    }
    function wrapper(uint256[1] memory value) internal pure returns (uint256) {
        touch(value);
        return 1;
    }
}

contract TransitiveAssemblyEffects is AssemblyEffectsBase {
    function touch(uint256[1] memory value) internal pure override {
        assembly { mstore(value, 9) }
    }
    function recurse(uint256[1] memory value, uint256 depth) internal pure returns (uint256) {
        if (depth == 0) return wrapper(value);
        return recurse(value, depth - 1);
    }
    function direct(uint256[1] memory value) internal pure returns (uint256) {
        assembly { mstore(value, 7) }
        return 1;
    }
    function viaDirect() external pure returns (uint256) {
        uint256[1] memory value = [uint256(5)];
        return direct(value) + value[0];
    }
    function viaWrapper() external pure returns (uint256) {
        uint256[1] memory value = [uint256(5)];
        return wrapper(value) + value[0];
    }
    function viaRecursion() external pure returns (uint256) {
        uint256[1] memory value = [uint256(5)];
        return recurse(value, 2) + value[0];
    }
    function viaAlias() external pure returns (uint256, uint256) {
        uint256[1] memory value = [uint256(5)];
        uint256[1] memory other = value;
        uint256 result = other[0] + wrapper(value);
        return (result, other[0]);
    }
    modifier writing(uint256[1] memory value) {
        assembly { mstore(value, 11) }
        _;
    }
    function modified(uint256[1] memory value) internal pure writing(value) returns (uint256) {
        return 1;
    }
    function viaModifier() external pure returns (uint256) {
        uint256[1] memory value = [uint256(5)];
        return modified(value) + value[0];
    }
}

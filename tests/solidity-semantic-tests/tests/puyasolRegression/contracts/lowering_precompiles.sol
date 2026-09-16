pragma solidity ^0.8.20;
contract PrecompileProbe {
    address constant SHA = address(2);
    function direct(bytes memory x) external view returns (bool, bytes memory) {
        return address(2).staticcall(x);
    }
    function wrapped(bytes memory x) external view returns (bool, bytes memory) {
        return (address(2)).staticcall(x);
    }
    function constantTarget(bytes memory x) external view returns (bool, bytes memory) {
        return SHA.staticcall(x);
    }
    function encodedInput() external view returns (bool, bytes memory) {
        return address(2).staticcall(abi.encodeWithSignature("f()"));
    }
}

// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract EventOnlyFallback {
    event Seen(uint64 marker);
    uint64 public marker;
    receive() external payable { marker = 1; emit Seen(marker); }
    fallback() external { marker = 2; emit Seen(marker); }
    function ping() external { marker = 3; emit Seen(marker); }
    // A void Solidity signature does not imply an empty actual EVM result.
    function raw() external pure { assembly { mstore(0, 7) return(0, 32) } }
}

contract FallbackResultFacts {
    function typedRaw(address target) external returns (uint256 size) {
        EventOnlyFallback(payable(target)).raw();
        assembly { size := returndatasize() }
    }
    function typed(address target) external returns (uint256 size) {
        EventOnlyFallback(payable(target)).ping();
        assembly { size := returndatasize() }
    }
    function pointer(address target) external returns (uint256 size) {
        function() external p = EventOnlyFallback(payable(target)).ping;
        p();
        assembly { size := returndatasize() }
    }
    function encoded(address target) external returns (bytes memory result, uint256 size) {
        bool ok;
        (ok, result) = target.call(abi.encodeCall(EventOnlyFallback(payable(target)).ping, ()));
        require(ok);
        assembly { size := returndatasize() }
    }
    function run(address target, bytes calldata data) external returns (bytes memory result, uint256 size) {
        bool ok;
        (ok, result) = target.call(data);
        require(ok);
        assembly { size := returndatasize() }
    }
}

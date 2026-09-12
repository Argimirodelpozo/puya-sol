// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract ApplicationTargetProbe {
    function ping() external pure returns (uint64) { return 7; }
    fallback(bytes calldata data) external returns (bytes memory) {
        return abi.encode(data.length, keccak256(data));
    }
}

contract ApplicationTargets {
	uint256 public constructionSize;
	constructor() {
		uint256 size;
		assembly { size := extcodesize(address()) }
		constructionSize = size;
	}
    function ping() external pure returns (uint64) { return 11; }
    function typed(address target) external view returns (uint64) {
        return ApplicationTargetProbe(target).ping();
    }
    function bare(address target) external returns (uint64) {
        (bool ok, bytes memory result) = target.call(abi.encodeCall(ApplicationTargetProbe.ping, ()));
        require(ok);
        return abi.decode(result, (uint64));
    }
    function staticTarget(address target) external view returns (uint64) {
        (bool ok, bytes memory result) = target.staticcall(abi.encodeCall(ApplicationTargetProbe.ping, ()));
        require(ok);
        return abi.decode(result, (uint64));
    }
    function size(address target) external view returns (uint256) { return target.code.length; }
    function yulRaw(address target, uint256 size) external view returns (uint256 n, uint256 count, uint256 hash) {
        assembly {
            mstore(128, 0x123456789a000000000000000000000000000000000000000000000000000000)
            if iszero(staticcall(gas(), target, 128, size, 512, 32)) { revert(0, 0) }
            n := returndatasize()
            returndatacopy(512, 0, n)
            count := mload(512) hash := mload(544)
        }
    }
    function yulSize(address target) external view returns (uint256 n) { assembly { n := extcodesize(target) } }
}

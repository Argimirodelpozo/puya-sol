// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// EVM-profile fallback/receive dispatch matrix, expectations pinned from
// solc's LEGACY dispatcher (ContractCompiler::appendFunctionSelector):
//   calldatasize < 4  -> fallback (receive ONLY when calldatasize == 0);
//   unmatched selector -> fallback; no fallback -> revert, empty returndata;
//   fallback(bytes)->bytes returns its bytes RAW (no ABI encoding).

contract EchoFallback {
    uint256 public hits;

    // returns-form fallback: the returned bytes ARE the returndata verbatim.
    fallback(bytes calldata data) external returns (bytes memory) {
        hits += 1;
        return abi.encodePacked(hex"aa", data);
    }
}

contract ReceiveAndFallback {
    uint256 public marker;
    bytes public seen;

    receive() external payable {
        marker = 1;
    }

    fallback() external {
        marker = 2;
        seen = msg.data;
    }
}

contract NoFallback {
    function ping() public pure returns (uint256) {
        return 7;
    }
}

contract FallbackCaller {
    // Fixed shapes: the EVM profile's low-level-call lowering requires the
    // calldata to be provable (unresolved shapes are a fail-loud error).
    function callUnmatched(address t)
        public
        returns (bool ok, bytes memory ret)
    {
        (ok, ret) = t.call(abi.encodeWithSelector(0xdeadbeef));
    }

    function callEcho(address t) public returns (bool ok, bytes memory ret) {
        (ok, ret) = t.call(hex"deadbeef0102030405");
    }

    function callShort(address t) public returns (bool ok, bytes memory ret) {
        (ok, ret) = t.call(hex"beef");
    }

    function callEmpty(address t) public returns (bool ok) {
        (ok, ) = t.call("");
    }
}

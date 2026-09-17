// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;

contract EmptyCallReceiver {
    uint256 public calls;
    uint256 public received;
    receive() external payable { ++calls; received = msg.value; }
}

contract EmptyCallFallback {
    uint256 public calls;
    uint256 public received;
    fallback(bytes calldata) external payable returns (bytes memory) {
        ++calls;
        received = msg.value;
        return hex"123456";
    }
}

contract EmptyCallRejecting {
    receive() external payable { revert("rejected"); }
}

contract LoweringEmptyCalls {
    function noValue(address target, bytes memory input, bool literal)
        external returns (bytes memory data, uint256 size)
    {
        (bool seeded, bytes memory previous) = address(4).staticcall(abi.encode(uint256(7)));
        require(seeded && previous.length == 32);
        bool ok;
        if (literal) (ok, data) = target.call("");
        else (ok, data) = target.call(input);
        require(ok);
        assembly { size := returndatasize() }
    }
    function run(address target, uint256 amount, bytes memory input, bool literal)
        external returns (bytes memory data, uint256 size)
    {
        (bool seeded, bytes memory previous) = address(4).staticcall(abi.encode(uint256(7)));
        require(seeded && previous.length == 32);
        bool ok;
        if (literal) (ok, data) = target.call{value: amount}("");
        else (ok, data) = target.call{value: amount}(input);
        require(ok);
        assembly { size := returndatasize() }
    }
    function zero() external returns (uint256 beforeCall, uint256 afterCall, uint256 size) {
        (bool seeded, bytes memory previous) = address(4).staticcall(abi.encode(uint256(7)));
        require(seeded && previous.length == 32);
        assembly { beforeCall := returndatasize() }
        (bool ok, bytes memory data) = address(0).call("");
        require(ok);
        size = data.length;
        assembly { afterCall := returndatasize() }
    }
}

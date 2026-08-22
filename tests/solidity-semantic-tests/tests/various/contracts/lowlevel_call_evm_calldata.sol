// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Guard: `target.call(abi.encodeWithSignature(...))` between two DEFAULT-profile
// contracts. The abi.* builtins emit canonical EVM calldata in every profile
// (keccak selector + EVM-encoded args); the callee's approval program now
// mounts the EVM route arms as an alias ahead of its ARC-4 router, so this
// dispatches instead of erring in the router's match table.
contract Callee {
    uint256 public value;
    string public tag;
    function setValue(uint256 v) public { value += v; }
    function setTag(string memory t) public returns (uint256) { tag = t; return bytes(t).length; }
}

contract Caller {
    function callSet(address callee, uint256 v) public {
        (bool ok, ) = callee.call(abi.encodeWithSignature("setValue(uint256)", v));
        require(ok);
    }
    // dynamic argument + used return value, through the same alias
    function callTag(address callee, string memory t) public returns (uint256) {
        (bool ok, bytes memory ret) = callee.call(abi.encodeWithSignature("setTag(string)", t));
        require(ok);
        return abi.decode(ret, (uint256));
    }
    // encodeCall spelling of the same transport
    function callSetTyped(address callee, uint256 v) public {
        (bool ok, ) = callee.call(abi.encodeCall(Callee.setValue, (v)));
        require(ok);
    }
}

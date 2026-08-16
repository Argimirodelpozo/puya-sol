// SPDX-License-Identifier: MIT
pragma solidity >=0.8.20;

interface ISelectorSemantics {
    function alpha(uint256 value) external returns (bool);
    function noReturn(bytes32 value) external;
}

contract EvmSelectorSemantics is ISelectorSemantics {
    event Seen(uint256 indexed value);
    error Boom(uint256 value);

    function alpha(uint256 value) external pure override returns (bool) {
        return value == 7;
    }

    function noReturn(bytes32) external pure override {}

    function functionSelector() external view returns (bytes4) {
        return this.alpha.selector;
    }

    function eventSelector() external pure returns (bytes32) {
        return Seen.selector;
    }

    function errorSelector() external pure returns (bytes4) {
        return Boom.selector;
    }

    function interfaceId() external pure returns (bytes4) {
        return type(ISelectorSemantics).interfaceId;
    }

    function encodedCallSelector() external view returns (bytes4 result) {
        bytes memory payload = abi.encodeCall(this.alpha, (7));
        assembly {
            result := mload(add(payload, 32))
        }
    }

    function encodedSignatureSelector() external pure returns (bytes4 result) {
        bytes memory payload = abi.encodeWithSignature("alpha(uint256)", 7);
        assembly {
            result := mload(add(payload, 32))
        }
    }

    function pointerSelector() external view returns (bytes4) {
        function(uint256) external returns (bool) pointer = this.alpha;
        return pointer.selector;
    }

    function pointerCall(uint256 value) external returns (bool) {
        function(uint256) external returns (bool) pointer = this.alpha;
        return pointer(value);
    }

    function innerMsgSig() public view returns (bytes4) {
        return msg.sig;
    }

    function outerMsgSig() external view returns (bytes4) {
        return innerMsgSig();
    }

    function directMsgSig() external view returns (bytes4) {
        return msg.sig;
    }

    // A runtime calldata offset forces the synthetic EVM calldata blob. At
    // offset zero its first four bytes must follow the same selector policy as
    // msg.sig, while ApplicationArgs[0] remains the ARC-4 routing selector.
    function assemblyMsgSig(uint256 offset) external pure returns (bytes4 result) {
        assembly {
            result := calldataload(offset)
        }
    }
}

// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

type SmallUint is uint16;
type SmallInt is int16;
type SmallBytes is bytes2;
type SmallBool is bool;

contract PackedAbiFacts {
    enum Choice { A, B }

    function scalars() external pure returns (bytes memory) {
        return abi.encodePacked(SmallUint.wrap(0x1234), SmallInt.wrap(-2),
            SmallBytes.wrap(0xabcd), SmallBool.wrap(true), address(0x1234), Choice.B);
    }

    function fixedArrays() external pure returns (bytes memory) {
        SmallBytes[2] memory b = [SmallBytes.wrap(0x1234), SmallBytes.wrap(0xabcd)];
        SmallUint[2] memory u = [SmallUint.wrap(1), SmallUint.wrap(65535)];
        SmallInt[2] memory s = [SmallInt.wrap(-2), SmallInt.wrap(32767)];
        bool[9] memory bits;
        bits[0] = true; bits[7] = true; bits[8] = true;
        return abi.encodePacked(b, u, s, bits);
    }

    function dynamicArrays(uint256 n) external pure returns (bytes memory) {
        SmallUint[] memory u = new SmallUint[](n);
        SmallBytes[] memory b = new SmallBytes[](n);
        bool[] memory bits = new bool[](n);
        for (uint256 i; i < n; ++i) {
            u[i] = SmallUint.wrap(uint16(i + 1));
            b[i] = SmallBytes.wrap(bytes2(uint16(i + 1)));
            bits[i] = i % 2 == 0;
        }
        return abi.encodePacked(u, b, bits);
    }

    function selectors(bytes4 sel, string memory signature) external pure returns (bytes memory) {
        return bytes.concat(abi.encodeWithSelector(0x12345678, uint16(7)),
            abi.encodeWithSelector(sel, uint16(8)), abi.encodeWithSignature(signature, uint16(9)));
    }

    function literals() external pure returns (bytes memory) {
        return bytes.concat(abi.encodePacked(hex"ff0080", "OK"), abi.encode(hex"ff0080"));
    }
}

contract AbiEffectFacts {
    uint64 private hits;

    function payload() internal returns (bytes memory) {
        ++hits;
        return abi.encode(uint256(7), uint256(9));
    }

    function decodeOnce() external returns (uint256, uint256, uint64) {
        hits = 0;
        (uint256 a, uint256 b) = abi.decode(payload(), (uint256, uint256));
        return (a, b, hits);
    }

    function self(uint64 digit) internal returns (AbiEffectFacts) {
        hits = hits * 10 + digit;
        return this;
    }

    function callee(uint256 a) external pure returns (uint256) { return a; }

    function encodeTarget() external returns (uint64, bytes memory) {
        hits = 0;
        bytes memory result = abi.encodeCall(self(1).callee, (7));
        return (hits, result);
    }

    function encodeConditional(bool choice) external returns (uint64, bytes memory) {
        hits = 0;
        bytes memory result = abi.encodeCall(choice ? self(1).callee : self(2).callee, (7));
        return (hits, result);
    }

    function encodeDeclaration() external pure returns (bytes memory) {
        return abi.encodeCall(AbiEffectFacts.callee, (7));
    }

    function arrayCallee(uint256[2] memory a) external pure returns (uint256) { return a[0]; }

    function encodeInlineArray() external pure returns (bytes memory) {
        return abi.encodeCall(AbiEffectFacts.arrayCallee, [uint256(7), 9]);
    }

    function chooseLiteral(bool choice) internal returns (bool) { ++hits; return choice; }

    function literalChoice(bool choice) external returns (uint64, bytes memory) {
        hits = 0;
        bytes memory result = abi.encodePacked(chooseLiteral(choice) ? hex"ff" : hex"0080");
        return (hits, result);
    }
}

contract ShadowedIntrinsicFacts {
    struct Block { uint256 number; uint256 timestamp; }
    struct Message { uint256 value; bytes4 sig; bytes data; }
    struct Transaction { uint256 gasprice; address origin; }

    function shadowed() external pure returns (uint256, bytes4, bytes memory, uint256) {
        Block memory block = Block(17, 19);
        Message memory msg = Message(23, 0x12345678, hex"abcd");
        Transaction memory tx = Transaction(29, address(31));
        return (block.number + (block).timestamp + msg.value + tx.gasprice, msg.sig, msg.data, uint160(tx.origin));
    }

    function realBlock() external view returns (bool) {
        return (block).number > 0 && (block).timestamp > 0;
    }
}

// SPDX-License-Identifier: MIT
pragma solidity ^0.8.34;

type Word is uint256;
function rawAdd(Word, Word) pure returns (Word) { assembly { return(0, 0) } }
using {rawAdd as +} for Word global;

contract RawReturnFrames {
    uint256 public marker;
    modifier around() { marker = 1; _; marker = 2; }

    function empty() external pure returns (uint256) { assembly { return(0, 0) } }
    function stopped() external pure { assembly { stop() } }
    function word() external pure returns (uint256) { assembly { mstore(0, 7) return(0, 32) } }
    function narrow() external pure returns (uint8) { assembly { mstore(0, 7) return(0, 32) } }
    function pair() external pure returns (uint8, bytes3) {
        assembly { mstore(0, 7) mstore(32, shl(232, 0x010203)) return(0, 64) }
    }
    function opaque() external pure returns (uint256) { assembly { mstore(0, 42) return(31, 1) } }
    function runtimeEmpty(uint256 size) external pure returns (uint256) { assembly { return(not(0), size) } }
    function halt() internal pure returns (uint256) { assembly { return(0, 0) } }
    function middle() internal pure returns (uint256) { halt(); return 11; }
    function modified() external around returns (uint256) { middle(); marker = 3; return 9; }
    function overloaded() external pure returns (uint256) {
        Word value = Word.wrap(1) + Word.wrap(2); return Word.unwrap(value) + 99;
    }
    function indirect() external pure returns (uint256) {
        function() internal pure returns (uint256) pointer = middle;
        pointer(); return 99;
    }

    function lowLevel(uint256 which) external returns (bool, bytes memory, uint256) {
        bytes memory payload = which == 0 ? abi.encodeWithSelector(this.empty.selector)
            : which == 1 ? abi.encodeWithSelector(this.opaque.selector)
            : which == 2 ? abi.encodeWithSelector(this.modified.selector)
            : which == 4 ? abi.encodeWithSelector(this.overloaded.selector)
            : which == 5 ? abi.encodeWithSelector(this.indirect.selector) : bytes(hex"12345678");
        (bool ok, bytes memory data) = address(this).call(payload);
        return (ok, data, marker + 10);
    }
    function typedWord() external view returns (uint256) { return this.word() + 1; }
    function typedNarrow() external view returns (uint256) { return this.narrow() + 1; }
    function typedPair() external view returns (uint256, bytes3) { return this.pair(); }
    function typedEmpty() external view returns (uint256) { return this.empty() + 1; }
    function typedStop() external view returns (uint256) { this.stopped(); return 17; }
    function pointerWord() external view returns (uint256) {
        function() external pure returns (uint256) pointer = this.word;
        return pointer() + 1;
    }
    fallback() external { assembly { mstore(0, 43) return(31, 1) } }
}

// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

enum Tri { A, B, C }

contract TypeProbe {
    // T2: rational-literal folding — solc computes in unbounded exact rationals.
    function rats() public pure returns (uint256, uint256, uint256, int256, uint256, uint256) {
        uint256 a = 1.5 * 2;                 // 3
        uint256 b = 2**800 / 2**790;         // 1024 (huge intermediates)
        uint256 c = 7 / 2 + 1 / 2;           // 3.5 + 0.5 = 4 exact
        int256  d = -1.5 * 2;                // -3
        uint256 e = (0.1 + 0.2) * 10;        // 3 EXACT (no binary-float error)
        uint256 f = 1.5e1;                   // 15
        return (a, b, c, d, e, f);
    }

    // T3: type(T).min/max on ints and enums, type(C).name.
    function minmax() public pure returns (uint8, int56, int56, uint8, uint8) {
        return (
            type(uint8).max,
            type(int56).min,
            type(int56).max,
            uint8(type(Tri).min),
            uint8(type(Tri).max)
        );
    }

    function cname() public pure returns (string memory) {
        return type(TypeProbe).name;
    }

    // T5: literal common ("mobile") types — array literal + ternary.
    function mobile(bool p) public pure returns (uint256, uint256) {
        uint16[3] memory arr = [1, 2, 300];  // common type uint16
        uint256 t = p ? 1 : 2**200;          // common type uint256
        return (arr[2], t);
    }

    // T4: function-type mutability lattice — pure fn assigned to view slot.
    function fpure() public pure returns (uint256) { return 21; }
    function lattice() public view returns (uint256) {
        function() internal pure returns (uint256) g = fpure;
        function() internal view returns (uint256) h = g;  // pure -> view widen
        return h() + g();
    }
}

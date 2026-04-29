// SPDX-License-Identifier: MIT
pragma solidity >=0.8.8;

// ─── External Library ───────────────────────────────────────────
library MathLib {
    function square(uint256 x) external pure returns (uint256) {
        return x * x;
    }

    function double(uint256 x) internal pure returns (uint256) {
        return x + x;
    }
}

// ─── Abstract Contract with Virtual/Override ─────────────────────
abstract contract BaseProcessor {
    uint256 public immutable multiplier;

    constructor(uint256 _multiplier) {
        multiplier = _multiplier;
    }

    function getMultiplier() internal view virtual returns (uint256);

    function process(uint256 x) external view returns (uint256) {
        return x * getMultiplier();
    }
}

// ─── Concrete Contract ──────────────────────────────────────────
contract AdvancedFeatureTest is BaseProcessor(7) {

    // Override the virtual function
    function getMultiplier() internal view override returns (uint256) {
        return multiplier;
    }

    // ── abi.encodePacked with static uint256 array ──
    function testEncodePackedUintArray(
        uint256 a, uint256 b, uint256 c
    ) external pure returns (bytes32) {
        uint256[3] memory data;
        data[0] = a;
        data[1] = b;
        data[2] = c;
        return keccak256(abi.encodePacked(data));
    }

    // ── abi.encodePacked with mixed scalar + array ──
    function testEncodePackedMixed(
        uint256 prefix, uint256 x, uint256 y
    ) external pure returns (bytes32) {
        uint256[2] memory arr;
        arr[0] = x;
        arr[1] = y;
        return keccak256(abi.encodePacked(prefix, arr));
    }

    // ── External library function ──
    function testExternalLib(uint256 x) external pure returns (uint256) {
        return MathLib.square(x);
    }

    // ── Internal library function ──
    function testInternalLib(uint256 x) external pure returns (uint256) {
        return MathLib.double(x);
    }

    // ── Inherited virtual/override ──
    // process() is inherited from BaseProcessor and uses getMultiplier() override
    // We test it directly via the inherited method

    // ── High-level .staticcall stub ──
    function testStaticCall() external view returns (bool) {
        // This tests that .staticcall compiles without error
        // The result is stubbed as (true, empty) on AVM
        (bool success, ) = address(this).staticcall("");
        return success;
    }
}

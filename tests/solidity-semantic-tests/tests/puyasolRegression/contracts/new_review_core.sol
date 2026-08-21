// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

// Guards the expression/control-flow review batch.
contract NewReviewCore {
    enum Small { A, B }

    bool private gate;
    uint256 public hits;

    modifier gated() {
        if (gate) _;
    }

    function setGate(bool value) external {
        gate = value;
    }

    // B3: the user assignment belongs behind the modifier placeholder.
    function modifierReturn() external gated returns (uint256 result) {
        result = 5;
    }

    function pair(uint256 x) external pure returns (uint256, uint256) {
        return (x + 1, x + 2);
    }

    // C19: self-call low-level returndata retains every return component.
    function selfPair(uint256 x) external returns (uint256, uint256) {
        (bool ok, bytes memory data) = address(this).call(
            abi.encodeCall(this.pair, (x)));
        require(ok);
        return abi.decode(data, (uint256, uint256));
    }

    // C8: nested type conversion around `this` is still a direct self call.
    function convertedSelfPair(uint256 x) external returns (uint256, uint256) {
        return NewReviewCore(address(this)).pair(x);
    }

    function one(uint256 x) external pure returns (uint256) {
        return x + 10;
    }

    function receiver() internal returns (NewReviewCore) {
        hits += 1;
        return this;
    }

    // D5: constant-folding the selector must retain receiver()'s effects.
    function selectorEffect() external returns (uint256 count, uint256 value) {
        hits = 0;
        (bool ok, bytes memory data) = address(this).call(
            abi.encodeWithSelector(receiver().one.selector, uint256(7)));
        require(ok);
        return (hits, abi.decode(data, (uint256)));
    }

    // C13: leave exits the Yul function through a nested for-loop.
    function leaveLoop(uint256 wanted) external pure returns (uint256 result) {
        assembly {
            function find(x) -> z {
                z := 5
                for { let i := 0 } lt(i, 3) { i := add(i, 1) } {
                    if eq(i, x) {
                        z := add(i, 10)
                        leave
                    }
                    z := add(z, 1)
                }
                z := 77
            }
            result := find(wanted)
        }
    }

    function bump(uint256 value) internal returns (uint256) {
        hits += 1;
        return value;
    }

    // C15: slice bounds execute once and the checked values are reused.
    function sliceOnce(bytes calldata data)
        external returns (bytes memory result, uint256 evaluations)
    {
        hits = 0;
        bytes calldata part = data[bump(1):bump(3)];
        return (part, hits);
    }

    // D8: a byte-element assignment expression yields the byte, not its array.
    function chainedByte(bytes memory a, bytes memory b, bytes1 value)
        external pure returns (bytes1, bytes1)
    {
        a[0] = b[1] = value;
        return (a[0], b[1]);
    }

    // D12: abi.decode rejects enum ordinals outside the declaration range.
    function decodeSmall(bytes calldata encoded) external pure returns (Small) {
        return abi.decode(encoded, (Small));
    }
}

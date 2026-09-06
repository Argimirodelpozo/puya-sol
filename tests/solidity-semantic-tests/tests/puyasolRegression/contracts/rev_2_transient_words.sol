// SPDX-License-Identifier: MIT
pragma solidity >=0.8.28;

type TransientAddress is address;
type TransientSigned is int24;

contract TransientLeft { int16 transient left; }
contract TransientRight {
    address transient direct;
    uint64 transient neighbor;
    bool transient flag;
}

contract TransientWords is TransientLeft, TransientRight {
    TransientAddress transient wrapped;
    uint96 transient tail;
    TransientSigned transient small;
    bytes3 transient marker;
    TransientWords transient target;
    function() internal pure returns (uint256) transient callback;
    function() external pure returns (uint256) transient externalCallback;
    int128 transient wide;
    uint128 transient wideNeighbor;

    function answer() internal pure returns (uint256) { return 17; }
    function answerExternal() external pure returns (uint256) { return 23; }

    function initialize() internal {
        left = -2;
        direct = msg.sender;
        neighbor = 0x0102030405060708;
        flag = true;
        wrapped = TransientAddress.wrap(msg.sender);
        tail = 0x0102030405060708090a0b0c;
        small = TransientSigned.wrap(-8388607);
        marker = hex"123456";
        target = this;
        callback = answer;
        externalCallback = this.answerExternal;
        wide = -170141183460469231731687303715884105727;
        wideNeighbor = 123;
    }

    function layoutA() external pure returns (uint256 a, uint256 b, uint256 c, uint256 d) {
        assembly {
            a := add(mul(left.slot, 32), left.offset)
            b := add(mul(direct.slot, 32), direct.offset)
            c := add(mul(neighbor.slot, 32), neighbor.offset)
            d := add(mul(flag.slot, 32), flag.offset)
        }
    }
    function layoutB() external pure returns (uint256 a, uint256 b, uint256 c, uint256 d, uint256 e) {
        assembly {
            a := add(mul(wrapped.slot, 32), wrapped.offset)
            b := add(mul(tail.slot, 32), tail.offset)
            c := add(mul(small.slot, 32), small.offset)
            d := add(mul(marker.slot, 32), marker.offset)
            e := add(mul(target.slot, 32), target.offset)
        }
    }
    function layoutC() external pure returns (uint256 a, uint256 b, uint256 c, uint256 d) {
        assembly {
            a := add(mul(callback.slot, 32), callback.offset)
            b := add(mul(externalCallback.slot, 32), externalCallback.offset)
            c := add(mul(wide.slot, 32), wide.offset)
            d := add(mul(wideNeighbor.slot, 32), wideNeighbor.offset)
        }
    }

    function typed() external returns (bool, bool, int24, bytes3) {
        initialize();
        neighbor++;
        require(callback() == 17 && target == this);
        require(TransientWords.callback() == 17 && TransientRight.direct == msg.sender);
        require(externalCallback.selector == this.answerExternal.selector);
        require(externalCallback.address == address(this));
        require(wide == -170141183460469231731687303715884105727 && wideNeighbor == 123);
        return (direct == msg.sender, TransientAddress.unwrap(wrapped) == msg.sender,
            TransientSigned.unwrap(small), marker);
    }

    function canonicalWords() external returns (uint256 a, uint256 b, uint256 c) {
        left = -2;
        direct = address(0x1234);
        neighbor = 0x0102030405060708;
        flag = true;
        wrapped = TransientAddress.wrap(address(0x5678));
        tail = 0x0102030405060708090a0b0c;
        small = TransientSigned.wrap(-2);
        marker = hex"123456";
        assembly { a := tload(direct.slot) b := tload(wrapped.slot) c := tload(small.slot) }
    }

    function fromRaw(uint256 a, uint256 b, uint256 c) external returns (bool, int24, bytes3) {
        assembly { tstore(direct.slot, a) tstore(wrapped.slot, b) tstore(small.slot, c) }
        return (left == -2 && direct == address(0x1234) && flag
            && neighbor == 0x0102030405060708 && TransientAddress.unwrap(wrapped) == address(0x5678)
            && tail == 0x0102030405060708090a0b0c, TransientSigned.unwrap(small), marker);
    }

    function afterRaw() external returns (bool, bool, bool, bool) {
        initialize();
        assembly {
            function nextSlot() -> s { tstore(127, add(tload(127), 1)) s := 0 }
            tstore(nextSlot(), tload(direct.slot))
            if iszero(eq(tload(127), 1)) { revert(0, 0) }
        }
        return (direct == msg.sender, TransientAddress.unwrap(wrapped) == msg.sender,
            left == -2 && flag && neighbor == 0x0102030405060708, target == this);
    }

    function produce() internal returns (address) { left++; return msg.sender; }
    function effectful() external returns (int16, bool, bool) {
        direct = produce();
        wrapped = TransientAddress.wrap(produce());
        return (left, direct == msg.sender, TransientAddress.unwrap(wrapped) == msg.sender);
    }

    function clear() external returns (bool) {
        initialize();
        delete direct;
        require(TransientAddress.unwrap(wrapped) == msg.sender);
        wrapped = TransientAddress.wrap(address(0));
        small = TransientSigned.wrap(0);
        return direct == address(0) && TransientAddress.unwrap(wrapped) == address(0)
            && left == -2 && flag && neighbor == 0x0102030405060708
            && tail == 0x0102030405060708090a0b0c && marker == hex"123456"
            && TransientSigned.unwrap(small) == 0;
    }

    function empty() external view returns (bool) {
        return left == 0 && direct == address(0) && TransientAddress.unwrap(wrapped) == address(0)
            && neighbor == 0 && tail == 0 && !flag && wide == 0;
    }
}

contract TransientReverse is TransientRight, TransientLeft {
    function layout() external pure returns (uint256 a, uint256 b, uint256 c, uint256 d) {
        assembly { a := direct.offset b := neighbor.offset c := flag.offset d := left.offset }
    }
    function check() external returns (bool) {
        direct = msg.sender;
        neighbor = 11;
        flag = true;
        left = -7;
        return direct == msg.sender && neighbor == 11 && flag && left == -7;
    }
}

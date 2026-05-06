pragma solidity >=0.8.21;

import {Honk} from "./HonkTypes.sol";
import {Fr, FrLib} from "./Fr.sol";

uint256 constant Q = 21888242871839275222246405745257275088696311157297823662689037894645226208583; // EC group order. F_q

// EC Point utilities

function convertProofPoint(Honk.G1ProofPoint memory input) pure returns (Honk.G1Point memory) {
    return Honk.G1Point({x: input.x_0 | (input.x_1 << 136), y: input.y_0 | (input.y_1 << 136)});
}

function ecMul(Honk.G1Point memory point, Fr scalar) view returns (Honk.G1Point memory) {
    bytes memory input = abi.encodePacked(point.x, point.y, Fr.unwrap(scalar));
    (bool success, bytes memory result) = address(0x07).staticcall(input);
    require(success, "ecMul failed");

    (uint256 x, uint256 y) = abi.decode(result, (uint256, uint256));
    return Honk.G1Point({x: x, y: y});
}

function ecAdd(Honk.G1Point memory point0, Honk.G1Point memory point1) view returns (Honk.G1Point memory) {
    bytes memory input = abi.encodePacked(point0.x, point0.y, point1.x, point1.y);
    (bool success, bytes memory result) = address(0x06).staticcall(input);
    require(success, "ecAdd failed");

    (uint256 x, uint256 y) = abi.decode(result, (uint256, uint256));
    return Honk.G1Point({x: x, y: y});
}

function ecSub(Honk.G1Point memory point0, Honk.G1Point memory point1) view returns (Honk.G1Point memory) {
    // We negate the second point
    uint256 negativePoint1Y = (Q - point1.y) % Q;
    bytes memory input = abi.encodePacked(point0.x, point0.y, point1.x, negativePoint1Y);
    (bool success, bytes memory result) = address(0x06).staticcall(input);
    require(success, "ecAdd failed");

    (uint256 x, uint256 y) = abi.decode(result, (uint256, uint256));
    return Honk.G1Point({x: x, y: y});
}

function negateInplace(Honk.G1Point memory point) pure returns (Honk.G1Point memory) {
    point.y = (Q - point.y) % Q;
    return point;
}

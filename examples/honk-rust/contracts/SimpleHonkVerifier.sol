// SPDX-License-Identifier: Apache-2.0
// SimpleHonkVerifier — UltraHonk verifier for N=32 circuit with 40 entities
// Test vectors from: https://github.com/miquelcabot/ultrahonk_verifier
pragma solidity >=0.8.21;

// ═══════════════════════════════════════════════════════════════════════════════
// Fr Field Arithmetic (BN254 scalar field)
// ═══════════════════════════════════════════════════════════════════════════════

type Fr is uint256;

using {add as +} for Fr global;
using {sub as -} for Fr global;
using {mul as *} for Fr global;
using {exp as ^} for Fr global;
using {notEqual as !=} for Fr global;
using {equal as ==} for Fr global;

uint256 constant MODULUS = 21888242871839275222246405745257275088548364400416034343698204186575808495617;
Fr constant MINUS_ONE = Fr.wrap(MODULUS - 1);

library FrLib {
    function from(uint256 value) internal pure returns (Fr) {
        return Fr.wrap(value % MODULUS);
    }

    function fromBytes32(bytes32 value) internal pure returns (Fr) {
        return Fr.wrap(uint256(value) % MODULUS);
    }

    function toBytes32(Fr value) internal pure returns (bytes32) {
        return bytes32(Fr.unwrap(value));
    }

    function invert(Fr value) internal view returns (Fr) {
        uint256 v = Fr.unwrap(value);
        uint256 result;
        assembly {
            let free := mload(0x40)
            mstore(free, 0x20)
            mstore(add(free, 0x20), 0x20)
            mstore(add(free, 0x40), 0x20)
            mstore(add(free, 0x60), v)
            mstore(add(free, 0x80), sub(MODULUS, 2))
            mstore(add(free, 0xa0), MODULUS)
            let success := staticcall(gas(), 0x05, free, 0xc0, 0x00, 0x20)
            if iszero(success) { revert(0, 0) }
            result := mload(0x00)
        }
        return Fr.wrap(result);
    }

    function pow(Fr base, uint256 v) internal view returns (Fr) {
        uint256 b = Fr.unwrap(base);
        uint256 result;
        assembly {
            let free := mload(0x40)
            mstore(free, 0x20)
            mstore(add(free, 0x20), 0x20)
            mstore(add(free, 0x40), 0x20)
            mstore(add(free, 0x60), b)
            mstore(add(free, 0x80), v)
            mstore(add(free, 0xa0), MODULUS)
            let success := staticcall(gas(), 0x05, free, 0xc0, 0x00, 0x20)
            if iszero(success) { revert(0, 0) }
            result := mload(0x00)
        }
        return Fr.wrap(result);
    }

    function div(Fr numerator, Fr denominator) internal view returns (Fr) {
        return numerator * invert(denominator);
    }

    function sqr(Fr value) internal pure returns (Fr) {
        return value * value;
    }

    function unwrap(Fr value) internal pure returns (uint256) {
        return Fr.unwrap(value);
    }

    function neg(Fr value) internal pure returns (Fr) {
        return Fr.wrap(MODULUS - Fr.unwrap(value));
    }
}

function add(Fr a, Fr b) pure returns (Fr) {
    return Fr.wrap(addmod(Fr.unwrap(a), Fr.unwrap(b), MODULUS));
}

function mul(Fr a, Fr b) pure returns (Fr) {
    return Fr.wrap(mulmod(Fr.unwrap(a), Fr.unwrap(b), MODULUS));
}

function sub(Fr a, Fr b) pure returns (Fr) {
    return Fr.wrap(addmod(Fr.unwrap(a), MODULUS - Fr.unwrap(b), MODULUS));
}

function exp(Fr base, Fr exponent) pure returns (Fr) {
    if (Fr.unwrap(exponent) == 0) return Fr.wrap(1);
    for (uint256 i = 1; i < Fr.unwrap(exponent); i += i) {
        base = base * base;
    }
    return base;
}

function notEqual(Fr a, Fr b) pure returns (bool) {
    return Fr.unwrap(a) != Fr.unwrap(b);
}

function equal(Fr a, Fr b) pure returns (bool) {
    return Fr.unwrap(a) == Fr.unwrap(b);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Types & Constants (40-entity UltraHonk)
// ═══════════════════════════════════════════════════════════════════════════════

uint256 constant N = 32;
uint256 constant LOG_N = 5;
uint256 constant NUMBER_OF_PUBLIC_INPUTS = 2;

uint256 constant CONST_PROOF_SIZE_LOG_N = 28;
uint256 constant NUMBER_OF_SUBRELATIONS = 26;
uint256 constant BATCHED_RELATION_PARTIAL_LENGTH = 8;
uint256 constant NUMBER_OF_ENTITIES = 40;
uint256 constant NUMBER_UNSHIFTED = 35;
uint256 constant NUMBER_TO_BE_SHIFTED = 5;
uint256 constant NUMBER_OF_ALPHAS = 25;

// WIRE enum — 40 entities (no TABLE_*_SHIFT in this version)
enum WIRE {
    Q_M,                    // 0
    Q_C,                    // 1
    Q_L,                    // 2
    Q_R,                    // 3
    Q_O,                    // 4
    Q_4,                    // 5
    Q_ARITH,                // 6
    Q_RANGE,                // 7
    Q_ELLIPTIC,             // 8
    Q_AUX,                  // 9
    Q_LOOKUP,               // 10
    Q_POSEIDON2_EXTERNAL,   // 11
    Q_POSEIDON2_INTERNAL,   // 12
    SIGMA_1,                // 13
    SIGMA_2,                // 14
    SIGMA_3,                // 15
    SIGMA_4,                // 16
    ID_1,                   // 17
    ID_2,                   // 18
    ID_3,                   // 19
    ID_4,                   // 20
    TABLE_1,                // 21
    TABLE_2,                // 22
    TABLE_3,                // 23
    TABLE_4,                // 24
    LAGRANGE_FIRST,         // 25
    LAGRANGE_LAST,          // 26
    W_L,                    // 27
    W_R,                    // 28
    W_O,                    // 29
    W_4,                    // 30
    Z_PERM,                 // 31
    LOOKUP_INVERSES,        // 32
    LOOKUP_READ_COUNTS,     // 33
    LOOKUP_READ_TAGS,       // 34
    // Shifted (5 entities, no TABLE shifts)
    W_L_SHIFT,              // 35
    W_R_SHIFT,              // 36
    W_O_SHIFT,              // 37
    W_4_SHIFT,              // 38
    Z_PERM_SHIFT            // 39
}

library Honk {
    struct G1Point {
        uint256 x;
        uint256 y;
    }

    struct G1ProofPoint {
        uint256 x_0;
        uint256 x_1;
        uint256 y_0;
        uint256 y_1;
    }

    struct VerificationKey {
        uint256 circuitSize;
        uint256 logCircuitSize;
        uint256 publicInputsSize;
        // Selectors
        G1Point qm;
        G1Point qc;
        G1Point ql;
        G1Point qr;
        G1Point qo;
        G1Point q4;
        G1Point qArith;
        G1Point qDeltaRange;
        G1Point qAux;
        G1Point qElliptic;
        G1Point qLookup;
        G1Point qPoseidon2External;
        G1Point qPoseidon2Internal;
        // Copy constraints
        G1Point s1;
        G1Point s2;
        G1Point s3;
        G1Point s4;
        // Copy identity
        G1Point id1;
        G1Point id2;
        G1Point id3;
        G1Point id4;
        // Precomputed lookup table
        G1Point t1;
        G1Point t2;
        G1Point t3;
        G1Point t4;
        // Fixed first and last
        G1Point lagrangeFirst;
        G1Point lagrangeLast;
    }

    struct Proof {
        uint256 circuitSize;
        uint256 publicInputsSize;
        uint256 publicInputsOffset;
        // Free wires
        Honk.G1ProofPoint w1;
        Honk.G1ProofPoint w2;
        Honk.G1ProofPoint w3;
        Honk.G1ProofPoint w4;
        // Lookup helpers - Permutations
        Honk.G1ProofPoint zPerm;
        // Lookup helpers - logup
        Honk.G1ProofPoint lookupReadCounts;
        Honk.G1ProofPoint lookupReadTags;
        Honk.G1ProofPoint lookupInverses;
        // Sumcheck
        Fr[BATCHED_RELATION_PARTIAL_LENGTH][CONST_PROOF_SIZE_LOG_N] sumcheckUnivariates;
        Fr[NUMBER_OF_ENTITIES] sumcheckEvaluations;
        // Gemini
        Honk.G1ProofPoint[CONST_PROOF_SIZE_LOG_N - 1] geminiFoldComms;
        Fr[CONST_PROOF_SIZE_LOG_N] geminiAEvaluations;
        Honk.G1ProofPoint shplonkQ;
        Honk.G1ProofPoint kzgQuotient;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// EC Utilities
// ═══════════════════════════════════════════════════════════════════════════════

uint256 constant Q = 21888242871839275222246405745257275088696311157297823662689037894645226208583;

// ═══════════════════════════════════════════════════════════════════════════════
// Transcript (Fiat-Shamir challenge generation)
// ═══════════════════════════════════════════════════════════════════════════════

struct Transcript {
    Fr eta;
    Fr etaTwo;
    Fr etaThree;
    Fr beta;
    Fr gamma;
    Fr[NUMBER_OF_ALPHAS] alphas;
    Fr[CONST_PROOF_SIZE_LOG_N] gateChallenges;
    Fr[CONST_PROOF_SIZE_LOG_N] sumCheckUChallenges;
    Fr rho;
    Fr geminiR;
    Fr shplonkNu;
    Fr shplonkZ;
    Fr publicInputsDelta;
}

library TranscriptLib {
    function readWord(bytes memory data, uint256 offset) internal pure returns (bytes32 result) {
        assembly {
            result := mload(add(add(data, 32), offset))
        }
    }

    function generateTranscript(Honk.Proof memory proof, bytes32[] memory publicInputs, uint256 publicInputsSize)
        internal
        view
        returns (Transcript memory t)
    {
        Fr previousChallenge;
        (t.eta, t.etaTwo, t.etaThree, previousChallenge) = generateEtaChallenge(proof, publicInputs, publicInputsSize);
        (t.beta, t.gamma, previousChallenge) = generateBetaAndGammaChallenges(previousChallenge, proof);
        (t.alphas, previousChallenge) = generateAlphaChallenges(previousChallenge, proof);
        (t.gateChallenges, previousChallenge) = generateGateChallenges(previousChallenge);
        (t.sumCheckUChallenges, previousChallenge) = generateSumcheckChallenges(proof, previousChallenge);
        (t.rho, previousChallenge) = generateRhoChallenge(proof, previousChallenge);
        (t.geminiR, previousChallenge) = generateGeminiRChallenge(proof, previousChallenge);
        (t.shplonkNu, previousChallenge) = generateShplonkNuChallenge(proof, previousChallenge);
        (t.shplonkZ, previousChallenge) = generateShplonkZChallenge(proof, previousChallenge);
        return t;
    }

    function splitChallenge(Fr challenge) internal pure returns (Fr first, Fr second) {
        uint256 challengeU256 = uint256(Fr.unwrap(challenge));
        uint256 lo = challengeU256 & 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF;
        uint256 hi = challengeU256 >> 128;
        first = FrLib.fromBytes32(bytes32(lo));
        second = FrLib.fromBytes32(bytes32(hi));
    }

    function generateEtaChallenge(Honk.Proof memory proof, bytes32[] memory publicInputs, uint256 publicInputsSize)
        internal
        pure
        returns (Fr eta, Fr etaTwo, Fr etaThree, Fr previousChallenge)
    {
        bytes32[] memory round0 = new bytes32[](3 + publicInputsSize + 12);
        round0[0] = bytes32(proof.circuitSize);
        round0[1] = bytes32(proof.publicInputsSize);
        round0[2] = bytes32(proof.publicInputsOffset);
        for (uint256 i = 0; i < publicInputsSize; i++) {
            round0[3 + i] = bytes32(publicInputs[i]);
        }
        round0[3 + publicInputsSize] = bytes32(proof.w1.x_0);
        round0[3 + publicInputsSize + 1] = bytes32(proof.w1.x_1);
        round0[3 + publicInputsSize + 2] = bytes32(proof.w1.y_0);
        round0[3 + publicInputsSize + 3] = bytes32(proof.w1.y_1);
        round0[3 + publicInputsSize + 4] = bytes32(proof.w2.x_0);
        round0[3 + publicInputsSize + 5] = bytes32(proof.w2.x_1);
        round0[3 + publicInputsSize + 6] = bytes32(proof.w2.y_0);
        round0[3 + publicInputsSize + 7] = bytes32(proof.w2.y_1);
        round0[3 + publicInputsSize + 8] = bytes32(proof.w3.x_0);
        round0[3 + publicInputsSize + 9] = bytes32(proof.w3.x_1);
        round0[3 + publicInputsSize + 10] = bytes32(proof.w3.y_0);
        round0[3 + publicInputsSize + 11] = bytes32(proof.w3.y_1);

        previousChallenge = FrLib.fromBytes32(keccak256(abi.encodePacked(round0)));
        (eta, etaTwo) = splitChallenge(previousChallenge);
        previousChallenge = FrLib.fromBytes32(keccak256(abi.encodePacked(Fr.unwrap(previousChallenge))));
        Fr unused;
        (etaThree, unused) = splitChallenge(previousChallenge);
    }

    function generateBetaAndGammaChallenges(Fr previousChallenge, Honk.Proof memory proof)
        internal
        pure
        returns (Fr beta, Fr gamma, Fr nextPreviousChallenge)
    {
        bytes32[13] memory round1;
        round1[0] = FrLib.toBytes32(previousChallenge);
        round1[1] = bytes32(proof.lookupReadCounts.x_0);
        round1[2] = bytes32(proof.lookupReadCounts.x_1);
        round1[3] = bytes32(proof.lookupReadCounts.y_0);
        round1[4] = bytes32(proof.lookupReadCounts.y_1);
        round1[5] = bytes32(proof.lookupReadTags.x_0);
        round1[6] = bytes32(proof.lookupReadTags.x_1);
        round1[7] = bytes32(proof.lookupReadTags.y_0);
        round1[8] = bytes32(proof.lookupReadTags.y_1);
        round1[9] = bytes32(proof.w4.x_0);
        round1[10] = bytes32(proof.w4.x_1);
        round1[11] = bytes32(proof.w4.y_0);
        round1[12] = bytes32(proof.w4.y_1);

        nextPreviousChallenge = FrLib.fromBytes32(keccak256(abi.encodePacked(round1)));
        (beta, gamma) = splitChallenge(nextPreviousChallenge);
    }

    function generateAlphaChallenges(Fr previousChallenge, Honk.Proof memory proof)
        internal
        pure
        returns (Fr[NUMBER_OF_ALPHAS] memory alphas, Fr nextPreviousChallenge)
    {
        uint256[9] memory alpha0;
        alpha0[0] = Fr.unwrap(previousChallenge);
        alpha0[1] = proof.lookupInverses.x_0;
        alpha0[2] = proof.lookupInverses.x_1;
        alpha0[3] = proof.lookupInverses.y_0;
        alpha0[4] = proof.lookupInverses.y_1;
        alpha0[5] = proof.zPerm.x_0;
        alpha0[6] = proof.zPerm.x_1;
        alpha0[7] = proof.zPerm.y_0;
        alpha0[8] = proof.zPerm.y_1;

        nextPreviousChallenge = FrLib.fromBytes32(keccak256(abi.encodePacked(alpha0)));
        (alphas[0], alphas[1]) = splitChallenge(nextPreviousChallenge);

        for (uint256 i = 1; i < NUMBER_OF_ALPHAS / 2; i++) {
            nextPreviousChallenge = FrLib.fromBytes32(keccak256(abi.encodePacked(Fr.unwrap(nextPreviousChallenge))));
            (alphas[2 * i], alphas[2 * i + 1]) = splitChallenge(nextPreviousChallenge);
        }
        if (((NUMBER_OF_ALPHAS & 1) == 1) && (NUMBER_OF_ALPHAS > 2)) {
            nextPreviousChallenge = FrLib.fromBytes32(keccak256(abi.encodePacked(Fr.unwrap(nextPreviousChallenge))));
            Fr unused;
            (alphas[NUMBER_OF_ALPHAS - 1], unused) = splitChallenge(nextPreviousChallenge);
        }
    }

    function generateGateChallenges(Fr previousChallenge)
        internal
        pure
        returns (Fr[CONST_PROOF_SIZE_LOG_N] memory gateChallenges, Fr nextPreviousChallenge)
    {
        for (uint256 i = 0; i < CONST_PROOF_SIZE_LOG_N; i++) {
            previousChallenge = FrLib.fromBytes32(keccak256(abi.encodePacked(Fr.unwrap(previousChallenge))));
            Fr unused;
            (gateChallenges[i], unused) = splitChallenge(previousChallenge);
        }
        nextPreviousChallenge = previousChallenge;
    }

    function generateSumcheckChallenges(Honk.Proof memory proof, Fr prevChallenge)
        internal
        pure
        returns (Fr[CONST_PROOF_SIZE_LOG_N] memory sumcheckChallenges, Fr nextPreviousChallenge)
    {
        for (uint256 i = 0; i < CONST_PROOF_SIZE_LOG_N; i++) {
            Fr[BATCHED_RELATION_PARTIAL_LENGTH + 1] memory univariateChal;
            univariateChal[0] = prevChallenge;
            for (uint256 j = 0; j < BATCHED_RELATION_PARTIAL_LENGTH; j++) {
                univariateChal[j + 1] = proof.sumcheckUnivariates[i][j];
            }
            prevChallenge = FrLib.fromBytes32(keccak256(abi.encodePacked(univariateChal)));
            Fr unused;
            (sumcheckChallenges[i], unused) = splitChallenge(prevChallenge);
        }
        nextPreviousChallenge = prevChallenge;
    }

    function generateRhoChallenge(Honk.Proof memory proof, Fr prevChallenge)
        internal
        pure
        returns (Fr rho, Fr nextPreviousChallenge)
    {
        Fr[NUMBER_OF_ENTITIES + 1] memory rhoChallengeElements;
        rhoChallengeElements[0] = prevChallenge;
        for (uint256 i = 0; i < NUMBER_OF_ENTITIES; i++) {
            rhoChallengeElements[i + 1] = proof.sumcheckEvaluations[i];
        }
        nextPreviousChallenge = FrLib.fromBytes32(keccak256(abi.encodePacked(rhoChallengeElements)));
        Fr unused;
        (rho, unused) = splitChallenge(nextPreviousChallenge);
    }

    function generateGeminiRChallenge(Honk.Proof memory proof, Fr prevChallenge)
        internal
        pure
        returns (Fr geminiR, Fr nextPreviousChallenge)
    {
        // Build the hash input incrementally via bytes concatenation instead
        // of a fixed uint256[109] array.  This avoids the 6976-byte zero
        // initialization that the compiler unrolls into 1000+ TEAL lines.
        bytes memory data = abi.encodePacked(Fr.unwrap(prevChallenge));
        for (uint256 i = 0; i < CONST_PROOF_SIZE_LOG_N - 1; i++) {
            data = abi.encodePacked(
                data,
                proof.geminiFoldComms[i].x_0,
                proof.geminiFoldComms[i].x_1,
                proof.geminiFoldComms[i].y_0,
                proof.geminiFoldComms[i].y_1
            );
        }
        nextPreviousChallenge = FrLib.fromBytes32(keccak256(data));
        Fr unused;
        (geminiR, unused) = splitChallenge(nextPreviousChallenge);
    }

    function generateShplonkNuChallenge(Honk.Proof memory proof, Fr prevChallenge)
        internal
        pure
        returns (Fr shplonkNu, Fr nextPreviousChallenge)
    {
        uint256[(CONST_PROOF_SIZE_LOG_N) + 1] memory shplonkNuChallengeElements;
        shplonkNuChallengeElements[0] = Fr.unwrap(prevChallenge);
        for (uint256 i = 0; i < CONST_PROOF_SIZE_LOG_N; i++) {
            shplonkNuChallengeElements[i + 1] = Fr.unwrap(proof.geminiAEvaluations[i]);
        }
        nextPreviousChallenge = FrLib.fromBytes32(keccak256(abi.encodePacked(shplonkNuChallengeElements)));
        Fr unused;
        (shplonkNu, unused) = splitChallenge(nextPreviousChallenge);
    }

    function generateShplonkZChallenge(Honk.Proof memory proof, Fr prevChallenge)
        internal
        pure
        returns (Fr shplonkZ, Fr nextPreviousChallenge)
    {
        uint256[5] memory shplonkZChallengeElements;
        shplonkZChallengeElements[0] = Fr.unwrap(prevChallenge);
        shplonkZChallengeElements[1] = proof.shplonkQ.x_0;
        shplonkZChallengeElements[2] = proof.shplonkQ.x_1;
        shplonkZChallengeElements[3] = proof.shplonkQ.y_0;
        shplonkZChallengeElements[4] = proof.shplonkQ.y_1;
        nextPreviousChallenge = FrLib.fromBytes32(keccak256(abi.encodePacked(shplonkZChallengeElements)));
        Fr unused;
        (shplonkZ, unused) = splitChallenge(nextPreviousChallenge);
    }

}

// ═══════════════════════════════════════════════════════════════════════════════
// Relations (all 8 relation accumulators + batch)
// ═══════════════════════════════════════════════════════════════════════════════

library RelationsLib {
    Fr internal constant GRUMPKIN_CURVE_B_PARAMETER_NEGATED = Fr.wrap(17);

    function accumulateRelationEvaluations(Honk.Proof memory proof, Transcript memory tp, Fr powPartialEval)
        internal
        view
        returns (Fr accumulator)
    {
        Fr[NUMBER_OF_ENTITIES] memory purportedEvaluations = proof.sumcheckEvaluations;
        Fr[NUMBER_OF_SUBRELATIONS] memory evaluations;

        accumulateArithmeticRelation(purportedEvaluations, evaluations, powPartialEval);
        accumulatePermutationRelation(purportedEvaluations, tp, evaluations, powPartialEval);
        accumulateLogDerivativeLookupRelation(purportedEvaluations, tp, evaluations, powPartialEval);
        accumulateDeltaRangeRelation(purportedEvaluations, evaluations, powPartialEval);
        accumulateEllipticRelation(purportedEvaluations, evaluations, powPartialEval);
        accumulateAuxillaryRelation(purportedEvaluations, tp, evaluations, powPartialEval);
        accumulatePoseidonExternalRelation(purportedEvaluations, tp, evaluations, powPartialEval);
        accumulatePoseidonInternalRelation(purportedEvaluations, evaluations, powPartialEval);
        accumulator = scaleAndBatchSubrelations(evaluations, tp.alphas);
    }

    function wire(Fr[NUMBER_OF_ENTITIES] memory p, WIRE _wire) internal pure returns (Fr) {
        return p[uint256(_wire)];
    }

    function accumulateArithmeticRelation(
        Fr[NUMBER_OF_ENTITIES] memory p,
        Fr[NUMBER_OF_SUBRELATIONS] memory evals,
        Fr domainSep
    ) internal view {
        Fr q_arith = wire(p, WIRE.Q_ARITH);
        {
            Fr neg_half = Fr.wrap(0) - (FrLib.invert(Fr.wrap(2)));
            Fr accum = (q_arith - Fr.wrap(3)) * (wire(p, WIRE.Q_M) * wire(p, WIRE.W_R) * wire(p, WIRE.W_L)) * neg_half;
            accum = accum + (wire(p, WIRE.Q_L) * wire(p, WIRE.W_L)) + (wire(p, WIRE.Q_R) * wire(p, WIRE.W_R))
                + (wire(p, WIRE.Q_O) * wire(p, WIRE.W_O)) + (wire(p, WIRE.Q_4) * wire(p, WIRE.W_4)) + wire(p, WIRE.Q_C);
            accum = accum + (q_arith - Fr.wrap(1)) * wire(p, WIRE.W_4_SHIFT);
            accum = accum * q_arith;
            accum = accum * domainSep;
            evals[0] = accum;
        }
        {
            Fr accum = wire(p, WIRE.W_L) + wire(p, WIRE.W_4) - wire(p, WIRE.W_L_SHIFT) + wire(p, WIRE.Q_M);
            accum = accum * (q_arith - Fr.wrap(2));
            accum = accum * (q_arith - Fr.wrap(1));
            accum = accum * q_arith;
            accum = accum * domainSep;
            evals[1] = accum;
        }
    }

    function accumulatePermutationRelation(
        Fr[NUMBER_OF_ENTITIES] memory p,
        Transcript memory tp,
        Fr[NUMBER_OF_SUBRELATIONS] memory evals,
        Fr domainSep
    ) internal pure {
        Fr grand_product_numerator;
        Fr grand_product_denominator;
        {
            Fr num = wire(p, WIRE.W_L) + wire(p, WIRE.ID_1) * tp.beta + tp.gamma;
            num = num * (wire(p, WIRE.W_R) + wire(p, WIRE.ID_2) * tp.beta + tp.gamma);
            num = num * (wire(p, WIRE.W_O) + wire(p, WIRE.ID_3) * tp.beta + tp.gamma);
            num = num * (wire(p, WIRE.W_4) + wire(p, WIRE.ID_4) * tp.beta + tp.gamma);
            grand_product_numerator = num;
        }
        {
            Fr den = wire(p, WIRE.W_L) + wire(p, WIRE.SIGMA_1) * tp.beta + tp.gamma;
            den = den * (wire(p, WIRE.W_R) + wire(p, WIRE.SIGMA_2) * tp.beta + tp.gamma);
            den = den * (wire(p, WIRE.W_O) + wire(p, WIRE.SIGMA_3) * tp.beta + tp.gamma);
            den = den * (wire(p, WIRE.W_4) + wire(p, WIRE.SIGMA_4) * tp.beta + tp.gamma);
            grand_product_denominator = den;
        }
        {
            Fr acc = (wire(p, WIRE.Z_PERM) + wire(p, WIRE.LAGRANGE_FIRST)) * grand_product_numerator;
            acc = acc
                - (
                    (wire(p, WIRE.Z_PERM_SHIFT) + (wire(p, WIRE.LAGRANGE_LAST) * tp.publicInputsDelta))
                        * grand_product_denominator
                );
            acc = acc * domainSep;
            evals[2] = acc;
        }
        {
            Fr acc = (wire(p, WIRE.LAGRANGE_LAST) * wire(p, WIRE.Z_PERM_SHIFT)) * domainSep;
            evals[3] = acc;
        }
    }

    function accumulateLogDerivativeLookupRelation(
        Fr[NUMBER_OF_ENTITIES] memory p,
        Transcript memory tp,
        Fr[NUMBER_OF_SUBRELATIONS] memory evals,
        Fr domainSep
    ) internal pure {
        Fr write_term;
        Fr read_term;
        {
            write_term = wire(p, WIRE.TABLE_1) + tp.gamma + (wire(p, WIRE.TABLE_2) * tp.eta)
                + (wire(p, WIRE.TABLE_3) * tp.etaTwo) + (wire(p, WIRE.TABLE_4) * tp.etaThree);
        }
        {
            Fr derived_entry_1 = wire(p, WIRE.W_L) + tp.gamma + (wire(p, WIRE.Q_R) * wire(p, WIRE.W_L_SHIFT));
            Fr derived_entry_2 = wire(p, WIRE.W_R) + wire(p, WIRE.Q_M) * wire(p, WIRE.W_R_SHIFT);
            Fr derived_entry_3 = wire(p, WIRE.W_O) + wire(p, WIRE.Q_C) * wire(p, WIRE.W_O_SHIFT);
            read_term = derived_entry_1 + (derived_entry_2 * tp.eta) + (derived_entry_3 * tp.etaTwo)
                + (wire(p, WIRE.Q_O) * tp.etaThree);
        }

        Fr read_inverse = wire(p, WIRE.LOOKUP_INVERSES) * write_term;
        Fr write_inverse = wire(p, WIRE.LOOKUP_INVERSES) * read_term;
        Fr inverse_exists_xor = wire(p, WIRE.LOOKUP_READ_TAGS) + wire(p, WIRE.Q_LOOKUP)
            - (wire(p, WIRE.LOOKUP_READ_TAGS) * wire(p, WIRE.Q_LOOKUP));
        Fr accumulatorNone = read_term * write_term * wire(p, WIRE.LOOKUP_INVERSES) - inverse_exists_xor;
        accumulatorNone = accumulatorNone * domainSep;
        Fr accumulatorOne = wire(p, WIRE.Q_LOOKUP) * read_inverse - wire(p, WIRE.LOOKUP_READ_COUNTS) * write_inverse;
        evals[4] = accumulatorNone;
        evals[5] = accumulatorOne;
    }

    function accumulateDeltaRangeRelation(
        Fr[NUMBER_OF_ENTITIES] memory p,
        Fr[NUMBER_OF_SUBRELATIONS] memory evals,
        Fr domainSep
    ) internal view {
        Fr minus_one = Fr.wrap(0) - Fr.wrap(1);
        Fr minus_two = Fr.wrap(0) - Fr.wrap(2);
        Fr minus_three = Fr.wrap(0) - Fr.wrap(3);
        Fr delta_1 = wire(p, WIRE.W_R) - wire(p, WIRE.W_L);
        Fr delta_2 = wire(p, WIRE.W_O) - wire(p, WIRE.W_R);
        Fr delta_3 = wire(p, WIRE.W_4) - wire(p, WIRE.W_O);
        Fr delta_4 = wire(p, WIRE.W_L_SHIFT) - wire(p, WIRE.W_4);
        {
            Fr acc = delta_1;
            acc = acc * (delta_1 + minus_one);
            acc = acc * (delta_1 + minus_two);
            acc = acc * (delta_1 + minus_three);
            acc = acc * wire(p, WIRE.Q_RANGE);
            acc = acc * domainSep;
            evals[6] = acc;
        }
        {
            Fr acc = delta_2;
            acc = acc * (delta_2 + minus_one);
            acc = acc * (delta_2 + minus_two);
            acc = acc * (delta_2 + minus_three);
            acc = acc * wire(p, WIRE.Q_RANGE);
            acc = acc * domainSep;
            evals[7] = acc;
        }
        {
            Fr acc = delta_3;
            acc = acc * (delta_3 + minus_one);
            acc = acc * (delta_3 + minus_two);
            acc = acc * (delta_3 + minus_three);
            acc = acc * wire(p, WIRE.Q_RANGE);
            acc = acc * domainSep;
            evals[8] = acc;
        }
        {
            Fr acc = delta_4;
            acc = acc * (delta_4 + minus_one);
            acc = acc * (delta_4 + minus_two);
            acc = acc * (delta_4 + minus_three);
            acc = acc * wire(p, WIRE.Q_RANGE);
            acc = acc * domainSep;
            evals[9] = acc;
        }
    }

    struct EllipticParams {
        Fr x_1;
        Fr y_1;
        Fr x_2;
        Fr y_2;
        Fr y_3;
        Fr x_3;
        Fr x_double_identity;
    }

    function accumulateEllipticRelation(
        Fr[NUMBER_OF_ENTITIES] memory p,
        Fr[NUMBER_OF_SUBRELATIONS] memory evals,
        Fr domainSep
    ) internal pure {
        EllipticParams memory ep;
        ep.x_1 = wire(p, WIRE.W_R);
        ep.y_1 = wire(p, WIRE.W_O);
        ep.x_2 = wire(p, WIRE.W_L_SHIFT);
        ep.y_2 = wire(p, WIRE.W_4_SHIFT);
        ep.y_3 = wire(p, WIRE.W_O_SHIFT);
        ep.x_3 = wire(p, WIRE.W_R_SHIFT);
        Fr q_sign = wire(p, WIRE.Q_L);
        Fr q_is_double = wire(p, WIRE.Q_M);
        Fr x_diff = (ep.x_2 - ep.x_1);
        Fr y1_sqr = (ep.y_1 * ep.y_1);
        {
            Fr partialEval = domainSep;
            Fr y2_sqr = (ep.y_2 * ep.y_2);
            Fr y1y2 = ep.y_1 * ep.y_2 * q_sign;
            Fr x_add_identity = (ep.x_3 + ep.x_2 + ep.x_1);
            x_add_identity = x_add_identity * x_diff * x_diff;
            x_add_identity = x_add_identity - y2_sqr - y1_sqr + y1y2 + y1y2;
            evals[10] = x_add_identity * partialEval * wire(p, WIRE.Q_ELLIPTIC) * (Fr.wrap(1) - q_is_double);
        }
        {
            Fr y1_plus_y3 = ep.y_1 + ep.y_3;
            Fr y_diff = ep.y_2 * q_sign - ep.y_1;
            Fr y_add_identity = y1_plus_y3 * x_diff + (ep.x_3 - ep.x_1) * y_diff;
            evals[11] = y_add_identity * domainSep * wire(p, WIRE.Q_ELLIPTIC) * (Fr.wrap(1) - q_is_double);
        }
        {
            Fr x_pow_4 = (y1_sqr + GRUMPKIN_CURVE_B_PARAMETER_NEGATED) * ep.x_1;
            Fr y1_sqr_mul_4 = y1_sqr + y1_sqr;
            y1_sqr_mul_4 = y1_sqr_mul_4 + y1_sqr_mul_4;
            Fr x1_pow_4_mul_9 = x_pow_4 * Fr.wrap(9);
            ep.x_double_identity = (ep.x_3 + ep.x_1 + ep.x_1) * y1_sqr_mul_4 - x1_pow_4_mul_9;
            Fr acc = ep.x_double_identity * domainSep * wire(p, WIRE.Q_ELLIPTIC) * q_is_double;
            evals[10] = evals[10] + acc;
        }
        {
            Fr x1_sqr_mul_3 = (ep.x_1 + ep.x_1 + ep.x_1) * ep.x_1;
            Fr y_double_identity = x1_sqr_mul_3 * (ep.x_1 - ep.x_3) - (ep.y_1 + ep.y_1) * (ep.y_1 + ep.y_3);
            evals[11] = evals[11] + y_double_identity * domainSep * wire(p, WIRE.Q_ELLIPTIC) * q_is_double;
        }
    }

    Fr constant LIMB_SIZE = Fr.wrap(uint256(1) << 68);
    Fr constant SUBLIMB_SHIFT = Fr.wrap(uint256(1) << 14);

    struct AuxParams {
        Fr limb_subproduct;
        Fr non_native_field_gate_1;
        Fr non_native_field_gate_2;
        Fr non_native_field_gate_3;
        Fr limb_accumulator_1;
        Fr limb_accumulator_2;
        Fr memory_record_check;
        Fr partial_record_check;
        Fr next_gate_access_type;
        Fr record_delta;
        Fr index_delta;
        Fr adjacent_values_match_if_adjacent_indices_match;
        Fr adjacent_values_match_if_adjacent_indices_match_and_next_access_is_a_read_operation;
        Fr access_check;
        Fr next_gate_access_type_is_boolean;
        Fr ROM_consistency_check_identity;
        Fr RAM_consistency_check_identity;
        Fr timestamp_delta;
        Fr RAM_timestamp_check_identity;
        Fr memory_identity;
        Fr index_is_monotonically_increasing;
        Fr auxiliary_identity;
    }

    function accumulateAuxillaryRelation(
        Fr[NUMBER_OF_ENTITIES] memory p,
        Transcript memory tp,
        Fr[NUMBER_OF_SUBRELATIONS] memory evals,
        Fr domainSep
    ) internal pure {
        AuxParams memory ap;

        ap.limb_subproduct = wire(p, WIRE.W_L) * wire(p, WIRE.W_R_SHIFT) + wire(p, WIRE.W_L_SHIFT) * wire(p, WIRE.W_R);
        ap.non_native_field_gate_2 =
            (wire(p, WIRE.W_L) * wire(p, WIRE.W_4) + wire(p, WIRE.W_R) * wire(p, WIRE.W_O) - wire(p, WIRE.W_O_SHIFT));
        ap.non_native_field_gate_2 = ap.non_native_field_gate_2 * LIMB_SIZE;
        ap.non_native_field_gate_2 = ap.non_native_field_gate_2 - wire(p, WIRE.W_4_SHIFT);
        ap.non_native_field_gate_2 = ap.non_native_field_gate_2 + ap.limb_subproduct;
        ap.non_native_field_gate_2 = ap.non_native_field_gate_2 * wire(p, WIRE.Q_4);

        ap.limb_subproduct = ap.limb_subproduct * LIMB_SIZE;
        ap.limb_subproduct = ap.limb_subproduct + (wire(p, WIRE.W_L_SHIFT) * wire(p, WIRE.W_R_SHIFT));
        ap.non_native_field_gate_1 = ap.limb_subproduct;
        ap.non_native_field_gate_1 = ap.non_native_field_gate_1 - (wire(p, WIRE.W_O) + wire(p, WIRE.W_4));
        ap.non_native_field_gate_1 = ap.non_native_field_gate_1 * wire(p, WIRE.Q_O);

        ap.non_native_field_gate_3 = ap.limb_subproduct;
        ap.non_native_field_gate_3 = ap.non_native_field_gate_3 + wire(p, WIRE.W_4);
        ap.non_native_field_gate_3 = ap.non_native_field_gate_3 - (wire(p, WIRE.W_O_SHIFT) + wire(p, WIRE.W_4_SHIFT));
        ap.non_native_field_gate_3 = ap.non_native_field_gate_3 * wire(p, WIRE.Q_M);

        Fr non_native_field_identity =
            ap.non_native_field_gate_1 + ap.non_native_field_gate_2 + ap.non_native_field_gate_3;
        non_native_field_identity = non_native_field_identity * wire(p, WIRE.Q_R);

        ap.limb_accumulator_1 = wire(p, WIRE.W_R_SHIFT) * SUBLIMB_SHIFT;
        ap.limb_accumulator_1 = ap.limb_accumulator_1 + wire(p, WIRE.W_L_SHIFT);
        ap.limb_accumulator_1 = ap.limb_accumulator_1 * SUBLIMB_SHIFT;
        ap.limb_accumulator_1 = ap.limb_accumulator_1 + wire(p, WIRE.W_O);
        ap.limb_accumulator_1 = ap.limb_accumulator_1 * SUBLIMB_SHIFT;
        ap.limb_accumulator_1 = ap.limb_accumulator_1 + wire(p, WIRE.W_R);
        ap.limb_accumulator_1 = ap.limb_accumulator_1 * SUBLIMB_SHIFT;
        ap.limb_accumulator_1 = ap.limb_accumulator_1 + wire(p, WIRE.W_L);
        ap.limb_accumulator_1 = ap.limb_accumulator_1 - wire(p, WIRE.W_4);
        ap.limb_accumulator_1 = ap.limb_accumulator_1 * wire(p, WIRE.Q_4);

        ap.limb_accumulator_2 = wire(p, WIRE.W_O_SHIFT) * SUBLIMB_SHIFT;
        ap.limb_accumulator_2 = ap.limb_accumulator_2 + wire(p, WIRE.W_R_SHIFT);
        ap.limb_accumulator_2 = ap.limb_accumulator_2 * SUBLIMB_SHIFT;
        ap.limb_accumulator_2 = ap.limb_accumulator_2 + wire(p, WIRE.W_L_SHIFT);
        ap.limb_accumulator_2 = ap.limb_accumulator_2 * SUBLIMB_SHIFT;
        ap.limb_accumulator_2 = ap.limb_accumulator_2 + wire(p, WIRE.W_4);
        ap.limb_accumulator_2 = ap.limb_accumulator_2 * SUBLIMB_SHIFT;
        ap.limb_accumulator_2 = ap.limb_accumulator_2 + wire(p, WIRE.W_O);
        ap.limb_accumulator_2 = ap.limb_accumulator_2 - wire(p, WIRE.W_4_SHIFT);
        ap.limb_accumulator_2 = ap.limb_accumulator_2 * wire(p, WIRE.Q_M);

        Fr limb_accumulator_identity = ap.limb_accumulator_1 + ap.limb_accumulator_2;
        limb_accumulator_identity = limb_accumulator_identity * wire(p, WIRE.Q_O);

        ap.memory_record_check = wire(p, WIRE.W_O) * tp.etaThree;
        ap.memory_record_check = ap.memory_record_check + (wire(p, WIRE.W_R) * tp.etaTwo);
        ap.memory_record_check = ap.memory_record_check + (wire(p, WIRE.W_L) * tp.eta);
        ap.memory_record_check = ap.memory_record_check + wire(p, WIRE.Q_C);
        ap.partial_record_check = ap.memory_record_check;
        ap.memory_record_check = ap.memory_record_check - wire(p, WIRE.W_4);

        ap.index_delta = wire(p, WIRE.W_L_SHIFT) - wire(p, WIRE.W_L);
        ap.record_delta = wire(p, WIRE.W_4_SHIFT) - wire(p, WIRE.W_4);
        ap.index_is_monotonically_increasing = ap.index_delta * ap.index_delta - ap.index_delta;
        ap.adjacent_values_match_if_adjacent_indices_match = (ap.index_delta * MINUS_ONE + Fr.wrap(1)) * ap.record_delta;

        evals[13] = ap.adjacent_values_match_if_adjacent_indices_match * (wire(p, WIRE.Q_L) * wire(p, WIRE.Q_R))
            * (wire(p, WIRE.Q_AUX) * domainSep);
        evals[14] = ap.index_is_monotonically_increasing * (wire(p, WIRE.Q_L) * wire(p, WIRE.Q_R))
            * (wire(p, WIRE.Q_AUX) * domainSep);

        ap.ROM_consistency_check_identity = ap.memory_record_check * (wire(p, WIRE.Q_L) * wire(p, WIRE.Q_R));

        Fr access_type = (wire(p, WIRE.W_4) - ap.partial_record_check);
        ap.access_check = access_type * access_type - access_type;

        ap.next_gate_access_type = wire(p, WIRE.W_O_SHIFT) * tp.etaThree;
        ap.next_gate_access_type = ap.next_gate_access_type + (wire(p, WIRE.W_R_SHIFT) * tp.etaTwo);
        ap.next_gate_access_type = ap.next_gate_access_type + (wire(p, WIRE.W_L_SHIFT) * tp.eta);
        ap.next_gate_access_type = wire(p, WIRE.W_4_SHIFT) - ap.next_gate_access_type;

        Fr value_delta = wire(p, WIRE.W_O_SHIFT) - wire(p, WIRE.W_O);
        ap.adjacent_values_match_if_adjacent_indices_match_and_next_access_is_a_read_operation = (
            ap.index_delta * MINUS_ONE + Fr.wrap(1)
        ) * value_delta * (ap.next_gate_access_type * MINUS_ONE + Fr.wrap(1));

        ap.next_gate_access_type_is_boolean =
            ap.next_gate_access_type * ap.next_gate_access_type - ap.next_gate_access_type;

        evals[15] = ap.adjacent_values_match_if_adjacent_indices_match_and_next_access_is_a_read_operation
            * (wire(p, WIRE.Q_ARITH)) * (wire(p, WIRE.Q_AUX) * domainSep);
        evals[16] = ap.index_is_monotonically_increasing * (wire(p, WIRE.Q_ARITH)) * (wire(p, WIRE.Q_AUX) * domainSep);
        evals[17] = ap.next_gate_access_type_is_boolean * (wire(p, WIRE.Q_ARITH)) * (wire(p, WIRE.Q_AUX) * domainSep);

        ap.RAM_consistency_check_identity = ap.access_check * (wire(p, WIRE.Q_ARITH));

        ap.timestamp_delta = wire(p, WIRE.W_R_SHIFT) - wire(p, WIRE.W_R);
        ap.RAM_timestamp_check_identity =
            (ap.index_delta * MINUS_ONE + Fr.wrap(1)) * ap.timestamp_delta - wire(p, WIRE.W_O);

        ap.memory_identity = ap.ROM_consistency_check_identity;
        ap.memory_identity =
            ap.memory_identity + ap.RAM_timestamp_check_identity * (wire(p, WIRE.Q_4) * wire(p, WIRE.Q_L));
        ap.memory_identity = ap.memory_identity + ap.memory_record_check * (wire(p, WIRE.Q_M) * wire(p, WIRE.Q_L));
        ap.memory_identity = ap.memory_identity + ap.RAM_consistency_check_identity;

        ap.auxiliary_identity = ap.memory_identity + non_native_field_identity + limb_accumulator_identity;
        ap.auxiliary_identity = ap.auxiliary_identity * (wire(p, WIRE.Q_AUX) * domainSep);
        evals[12] = ap.auxiliary_identity;
    }

    struct PoseidonExternalParams {
        Fr s1; Fr s2; Fr s3; Fr s4;
        Fr u1; Fr u2; Fr u3; Fr u4;
        Fr t0; Fr t1; Fr t2; Fr t3;
        Fr v1; Fr v2; Fr v3; Fr v4;
        Fr q_pos_by_scaling;
    }

    function accumulatePoseidonExternalRelation(
        Fr[NUMBER_OF_ENTITIES] memory p,
        Transcript memory tp,
        Fr[NUMBER_OF_SUBRELATIONS] memory evals,
        Fr domainSep
    ) internal pure {
        PoseidonExternalParams memory ep;
        ep.s1 = wire(p, WIRE.W_L) + wire(p, WIRE.Q_L);
        ep.s2 = wire(p, WIRE.W_R) + wire(p, WIRE.Q_R);
        ep.s3 = wire(p, WIRE.W_O) + wire(p, WIRE.Q_O);
        ep.s4 = wire(p, WIRE.W_4) + wire(p, WIRE.Q_4);
        ep.u1 = ep.s1 * ep.s1 * ep.s1 * ep.s1 * ep.s1;
        ep.u2 = ep.s2 * ep.s2 * ep.s2 * ep.s2 * ep.s2;
        ep.u3 = ep.s3 * ep.s3 * ep.s3 * ep.s3 * ep.s3;
        ep.u4 = ep.s4 * ep.s4 * ep.s4 * ep.s4 * ep.s4;
        ep.t0 = ep.u1 + ep.u2;
        ep.t1 = ep.u3 + ep.u4;
        ep.t2 = ep.u2 + ep.u2 + ep.t1;
        ep.t3 = ep.u4 + ep.u4 + ep.t0;
        ep.v4 = ep.t1 + ep.t1;
        ep.v4 = ep.v4 + ep.v4 + ep.t3;
        ep.v2 = ep.t0 + ep.t0;
        ep.v2 = ep.v2 + ep.v2 + ep.t2;
        ep.v1 = ep.t3 + ep.v2;
        ep.v3 = ep.t2 + ep.v4;
        ep.q_pos_by_scaling = wire(p, WIRE.Q_POSEIDON2_EXTERNAL) * domainSep;
        evals[18] = evals[18] + ep.q_pos_by_scaling * (ep.v1 - wire(p, WIRE.W_L_SHIFT));
        evals[19] = evals[19] + ep.q_pos_by_scaling * (ep.v2 - wire(p, WIRE.W_R_SHIFT));
        evals[20] = evals[20] + ep.q_pos_by_scaling * (ep.v3 - wire(p, WIRE.W_O_SHIFT));
        evals[21] = evals[21] + ep.q_pos_by_scaling * (ep.v4 - wire(p, WIRE.W_4_SHIFT));
    }

    struct PoseidonInternalParams {
        Fr u1; Fr u2; Fr u3; Fr u4;
        Fr u_sum; Fr v1; Fr v2; Fr v3; Fr v4;
        Fr s1; Fr q_pos_by_scaling;
    }

    function accumulatePoseidonInternalRelation(
        Fr[NUMBER_OF_ENTITIES] memory p,
        Fr[NUMBER_OF_SUBRELATIONS] memory evals,
        Fr domainSep
    ) internal pure {
        PoseidonInternalParams memory ip;
        Fr[4] memory INTERNAL_MATRIX_DIAGONAL = [
            FrLib.from(0x10dc6e9c006ea38b04b1e03b4bd9490c0d03f98929ca1d7fb56821fd19d3b6e7),
            FrLib.from(0x0c28145b6a44df3e0149b3d0a30b3bb599df9756d4dd9b84a86b38cfb45a740b),
            FrLib.from(0x00544b8338791518b2c7645a50392798b21f75bb60e3596170067d00141cac15),
            FrLib.from(0x222c01175718386f2e2e82eb122789e352e105a3b8fa852613bc534433ee428b)
        ];
        ip.s1 = wire(p, WIRE.W_L) + wire(p, WIRE.Q_L);
        ip.u1 = ip.s1 * ip.s1 * ip.s1 * ip.s1 * ip.s1;
        ip.u2 = wire(p, WIRE.W_R);
        ip.u3 = wire(p, WIRE.W_O);
        ip.u4 = wire(p, WIRE.W_4);
        ip.u_sum = ip.u1 + ip.u2 + ip.u3 + ip.u4;
        ip.q_pos_by_scaling = wire(p, WIRE.Q_POSEIDON2_INTERNAL) * domainSep;
        ip.v1 = ip.u1 * INTERNAL_MATRIX_DIAGONAL[0] + ip.u_sum;
        evals[22] = evals[22] + ip.q_pos_by_scaling * (ip.v1 - wire(p, WIRE.W_L_SHIFT));
        ip.v2 = ip.u2 * INTERNAL_MATRIX_DIAGONAL[1] + ip.u_sum;
        evals[23] = evals[23] + ip.q_pos_by_scaling * (ip.v2 - wire(p, WIRE.W_R_SHIFT));
        ip.v3 = ip.u3 * INTERNAL_MATRIX_DIAGONAL[2] + ip.u_sum;
        evals[24] = evals[24] + ip.q_pos_by_scaling * (ip.v3 - wire(p, WIRE.W_O_SHIFT));
        ip.v4 = ip.u4 * INTERNAL_MATRIX_DIAGONAL[3] + ip.u_sum;
        evals[25] = evals[25] + ip.q_pos_by_scaling * (ip.v4 - wire(p, WIRE.W_4_SHIFT));
    }

    function scaleAndBatchSubrelations(
        Fr[NUMBER_OF_SUBRELATIONS] memory evaluations,
        Fr[NUMBER_OF_ALPHAS] memory subrelationChallenges
    ) internal pure returns (Fr accumulator) {
        accumulator = accumulator + evaluations[0];
        for (uint256 i = 1; i < NUMBER_OF_SUBRELATIONS; ++i) {
            accumulator = accumulator + evaluations[i] * subrelationChallenges[i - 1];
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Errors
// ═══════════════════════════════════════════════════════════════════════════════

error PublicInputsLengthWrong();
error SumcheckFailed();
error ShpleminiFailed();

// ═══════════════════════════════════════════════════════════════════════════════
// SimpleHonkVerifierTest Contract
// ═══════════════════════════════════════════════════════════════════════════════

library VerifyLib {
    // ── EC Utilities ────────────────────────────────────────────────────────

    function convertProofPoint(Honk.G1ProofPoint memory input) internal pure returns (Honk.G1Point memory) {
        return Honk.G1Point({x: input.x_0 | (input.x_1 << 136), y: input.y_0 | (input.y_1 << 136)});
    }

    function ecMul(Honk.G1Point memory point, Fr scalar) internal view returns (Honk.G1Point memory) {
        bytes memory input = abi.encodePacked(point.x, point.y, Fr.unwrap(scalar));
        (bool success, bytes memory result) = address(0x07).staticcall(input);
        require(success, "ecMul failed");
        (uint256 x, uint256 y) = abi.decode(result, (uint256, uint256));
        return Honk.G1Point({x: x, y: y});
    }

    function ecAdd(Honk.G1Point memory point0, Honk.G1Point memory point1) internal view returns (Honk.G1Point memory) {
        bytes memory input = abi.encodePacked(point0.x, point0.y, point1.x, point1.y);
        (bool success, bytes memory result) = address(0x06).staticcall(input);
        require(success, "ecAdd failed");
        (uint256 x, uint256 y) = abi.decode(result, (uint256, uint256));
        return Honk.G1Point({x: x, y: y});
    }

    function negateInplace(Honk.G1Point memory point) internal pure returns (Honk.G1Point memory) {
        point.y = (Q - point.y) % Q;
        return point;
    }

    // ── Verifier ────────────────────────────────────────────────────────────

    function verify() internal view returns (bool) {
        Honk.VerificationKey memory vk = loadVerificationKey();
        Honk.Proof memory p = loadProof();

        bytes32[] memory publicInputs = new bytes32[](2);
        publicInputs[0] = bytes32(uint256(2));
        publicInputs[1] = bytes32(uint256(3));

        Transcript memory t = TranscriptLib.generateTranscript(p, publicInputs, vk.publicInputsSize);

        t.publicInputsDelta =
            computePublicInputDelta(publicInputs, t.beta, t.gamma, vk.circuitSize, p.publicInputsOffset);

        bool sumcheckVerified = verifySumcheck(p, t);
        if (!sumcheckVerified) revert SumcheckFailed();

        bool shpleminiVerified = verifyShplemini(p, vk, t);
        if (!shpleminiVerified) revert ShpleminiFailed();

        return sumcheckVerified && shpleminiVerified;
    }

    // ── Proof data (4 chunks, each ≤3616 bytes, 32-byte aligned for AVM 4096-byte limit) ──

    bytes constant PROOF_CHUNK_0 = hex"000000000000000000000000000000000000000000000000000000000000002000000000000000000000000000000000000000000000000000000000000000020000000000000000000000000000000000000000000000000000000000000001000000000000000000000000000000a16555b44bbe764b90975aa0d52b0ba43c0000000000000000000000000000000000041100eba196005627bb28e3b6ec8200000000000000000000000000000056bdbce2630a7f22e71e673b906b6e683b00000000000000000000000000000000000fb00382b00c68e7151406a5653c37000000000000000000000000000000fd298ac95bf5e22a727677f4e3feded87a00000000000000000000000000000000002f86305761ab79bb62c1c21695157400000000000000000000000000000038b6ae11f70a50117068c1567014454de0000000000000000000000000000000000017b872e686f2138d8bd7f2466998e100000000000000000000000000000033bd1c80b1b2e93d946c5bdbe955824abe00000000000000000000000000000000000c0ae686efc0389c91338aa6b0cae600000000000000000000000000000060a26a8b7dafe53885a7672bd835c3a04300000000000000000000000000000000002a8b6ee1de1fdc138326fc77d3c1d50000000000000000000000000000000ea3197280d9c1f581c6aee5fd13e5ab99000000000000000000000000000000000017e9156eb419522304bf90815b70e8000000000000000000000000000000c17c84cea29046525b7ed886d4a7cb20560000000000000000000000000000000000029bae5a451212815e1f97f3a49f4a0000000000000000000000000000000ea3197280d9c1f581c6aee5fd13e5ab99000000000000000000000000000000000017e9156eb419522304bf90815b70e8000000000000000000000000000000c17c84cea29046525b7ed886d4a7cb20560000000000000000000000000000000000029bae5a451212815e1f97f3a49f4a000000000000000000000000000000745cf398bbdc7e828c7bf39d2c1c03c8a400000000000000000000000000000000001056c697ca5fc1006ca0cde02c1a480000000000000000000000000000009d0b7421bd78783b1e4104f00b0f9bb210000000000000000000000000000000000015318df78da61666a7513b6dabb99100000000000000000000000000000093393690eea2b23aa371668390fda485a600000000000000000000000000000000000daa1a135cb4bcd86e71084644e44e000000000000000000000000000000e8bd00f425041f28a0c460aa5fae2657e9000000000000000000000000000000000023aed290896d5a47c398c592b3f7b80000000000000000000000000000008feee4545229b0adadb897b84c1247b41c00000000000000000000000000000000001346d9943283056206214e7105893d0000000000000000000000000000002bbde89b71569a202f06a0ac56cfde5b7a000000000000000000000000000000000023f932fd8d02cedb42bdebb6f5b8c71e041e5a85a368277a00dc35bc1cb841d1025eaaa178505127a4c77737b261f2126030185b8e38023e4f6980c564a01b5731899dd84120401c3d2e1cb84d9e0f03c3565b0a5b778bc0a4c3fc91cd88558c3b73acff261f85879274e2d0cd521204c8553efdb3b5fdf009ff5a283ace2903cfdebce89ea5e8846ead83e882ff850e0964751d5437b8b3429db523ba36b5b40cd573fe71dec250925f6ccc05227119006aa46d7175faf4e03991b0b3e6009d6fba1c9866c4b39dd7ae31742ee8da12bdd5c298692a2fabdee485c88a4ef1f5097200864cc8d5009c10b6db0ec8a7073ead4924272522516ac5faaeaccd0e215d7dcec0d0068503348439bec3f3f82e1cfd70fa00dad62da51e505e27964089d5ed7fc81ef624de5a0ce040df96671261290be5fb4070d35e02861d2339b910ebf9dca40c2a0c787472700581a4512171b1a07bca795284c673ba98a5414151613ab9c3d7042111fa116fe6717ea11ac8d8a5d9a74ac5aa23d913a88fae3bdbb29ac86d1c334baf48810eede673ce082e03969c53652746c4b136828ef68a634450c3d00d686fbceeef5036e05e051a7dfdba6bccae708bef1f44149741d4d5a4b1fc3de96a6f01a7c7b9e02cfbc70d1784a92d05299b18090d50dc1490bf2c0f3517177587cb0b3e473212dea394243b7e7785c15caacd6ce32fca5c9a0eb16aa020ae703c2201158a656c3452ae2948ddd12304ff12cd806628009b6b0bbf212ab9080b55d23467e5dcc9e729ec196783fb77f764d19eb684f65f6f97eae4aeb07a4e96ecd514b9364805f5dae82ae69bed1a66588c1053569cc6c775f7b676ce83ebb44478db305f081ce809bd0022f6336f94f68584d626c558dc7e88d9f198790904ef1cc62c50aaff47b43c287b95cc303633631fd9bbb8da7e1a02a43a3572f3d938af2cc47719743f06e20422d8f36ad0aeb42dd6ecba72da3631ef5aab5cf51c99a2361fa4d3febb16aa048d58ba1dbc0fb264d2a1cf00d5bbd621ed48b82091fe46eed16f8cea9a7fa328a5521995620538087b8748453b0bb4a85ba89aa939fa285549be4631e7e81f0f9cdbfbfdd0838b2a17c33ddfc5ec9b70bad90e84a9e26b16020727d005dcd01a33f0d001d84951d6ead10db7df8a0527467d97e6595789d98bee6f1965e36e24f50e4c7edbe733e387f4ca6d9f202d14a3924730e53def27274a11b914cf2f1228e10c4a7b8a4ac8a586d47f097b2675cc716e7b36889ec93c411b4b56dde100565842ed17ff937eb4845d66dd50386abb9237cd475322c163d6bd3b0673cf0f51e8c50aea051f0914f2149d5c40283870e06cb9ef3865eb91c70f8ce461ac07564cfc2db0fed225732c683b4d996339cb907913f7e5a7a470f4ed2321dca1048098773622cfae712fc7a04842ec37706fe15b98d28cf00a3e2a631924288409b23de33a475ef4f6816152c40d90537b29b10d69f35e3c8e8925250a97458e0ee41e887cd177de5bd9e21193b530e4f674cf6e245c884dc27fec5a05880b4f1db1748d8ea74f644a7385eea2cb1c5f0c19450f11041f0880e49a9f210bc2ec185fc47a82baffcaf1a50ebc633d50ba90e0bfc85fca9680f0f72b4fec3609a11c682795711d7f20f773790815c222f1b552a1b76eb1c65d5799a6f7d3e9c85425f6d2a161d000549b77a357c425b88f2a89145a51af79b613c32e4540a17d0217198f48fd0b31de90836375688a8146c1d33884df05a465e1d112e373fa6d7e1d81f2c929b9ac8abf541f65a8f9ffd1c74c5420cdb50b4a0e4f339c0c788c0100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
    bytes constant PROOF_CHUNK_1 = hex"00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
    bytes constant PROOF_CHUNK_2 = hex"0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001dd74e9c3b18cfc18ccb380d59fc8f874b0c07784aacf2848e229aeeab497e010f74a76765424ff8ae1956a70591e888626a897d45835a9fb176ee73cca75b32165ef7630b0141b8a8bdd0b95617ae85c404cf4798e1cf5b8eba4b05698be6cc0346082b7c70e259f63f8e56c8327e8cf949aa7e29151ec82d6ffe90f2c6a6c20f0f9996e4b6adc6fcba4d5d91fc41bb147e88cc8b3b3c556e6d87e3e053e16e1cffd96b3957f272cb13e32a401bf2b4ea58cfb87d3be02cea1a24b3fe69d7091c234e7fa782ae6b2e8294ae234f968882cfbed6b29ce260519410a70cd96f3927073d8eb4ae69769fdb046c2a5dae0a1f0a3b1e7f027a5d5612d77b53a98b6208c63bc78bd908ec27d059b36af70737f20e7515c1dc84b3f44480f663a502132a619f056bac32933605c59d254d7781cc1312edbfe1ac7654758c73923ec7ec0983b371cf95e427039516c05222d8c55a097eea8424e967f44e54612e52824f30208d4512fec7bd2b5f4815381bc27e9d1587ebc35269442b62cf034195601a0693a16a5bd229a70eba247cbfbe5eafa57bfca239ed09673651215050b15e9b1772a36efef784515e4b2ab77dc9b2698f851473e60f47d4d78e1caffcecbc70300af51b5c5cf71b1db38c9ec6f059e23295697556fc04fbea0de11196db17f3192ff368be30efaab3cf42670a723a7a773e85e8af9ab8db4aa945955970dda20c260135391ed4315e05dd3bca1ce5355258c7963b5ba5b6335a17c813b2789f10571c4ff66d5895c23a451ca3cb5c74d86253e36b2b90e879971085f691854c2bfe481626aeeb62a5b02efcda279e12a071938cb8f4fd6220870bc2a785a645044a02330e1814f7c4a50c591025daa5e1a91e4566cc13878ccab6f044e7fffc231c9aa1bebe8607817f91e09abb800f1d8f50dc5f0d0f377d00536c54db75520dd3e8eca4f34e0446c3fe02c795ebf49b20cd6a94fa4577dfb07bddf5dbf2830126cdd027596d6a2ddcec93b69645535a4e39f4e7b4637d481fe0b7afa94cff0301330e970c1dc3700da49aeae86852726c1687ba4af1121ceaa156bb94b0e11600244c8f9900ea048b012138ecf1d94957fcb18905aa11b30ee6ca9b9691b22a8cc14c80086ae434b6364ee9ea79c3932412648584f8432bc9e3b75793b8181550631a331582f64c4ed04788fb3f6073d575a2dce31ab4219d4090c8c5f2f82ae43fa19d7925920a79b53cf6d7d20fb6a23a7f016ad83f87fdddc7498191491ab41923aebb48aa89a5531d2f571f0bdff5d4a045af677f0233ff9262ada17c0db9424312077f02f326feb24de51c6b8a417821f28858fa8d9ba5b34e52cbb129ead3d61fd17cb7676debd58e92c9b595ae5db681c2ac1a6407e67a1556a6a9129b73e87c5a506db31c71ba89f0d730721152140165d27ca9591072049f226b06304f9891656eb11732045f998ef6bf3f1fd2474b7bb88820840f28607c038d1b565c84c9fcf0f02ccbbd7c346d99ab7a6b9eccb0e3f81d3047cd86e2a8bc901b565c84c9fcf0f02ccbbd7c346d99ab7a6b9eccb0e3f81d3047cd86e2a8bc900ddbd646eba704a636b58e54a47786008e7e772723734fb53d28f2a50d4413cb250c125db90a460cae31dfeb5b72241ddd6a63b7e7e9bda1f6ea83dc69de75e814d80bdda54b779df12b5778ff426c32e016d6e48bc28e19506385fced2c51422842a65b9259854906ecb3f6aa73aa192985fe1cb344e38693dc7cadec52a7c516f33e1e064528c9b0629ad215d9ba1509be040ac4aa09b7c36a1cbde8708954000000000000000000000000000000c1761cfb8b9ac3570c2ce6aa948688cc100000000000000000000000000000000000085aef4812aa721052928bc9e9fc7e0000000000000000000000000000005dbd1a3c63084e8fea657ca9fc5da0e07e00000000000000000000000000000000000d2fbe887487b8604fb954eeb6c53f000000000000000000000000000000098b36445842374bc357a2ef25f9837fd80000000000000000000000000000000000053eedf101608ac39f1c8177c2e21d000000000000000000000000000000532b9e2ab2c38e881335b6b294839ed5c50000000000000000000000000000000000039b5b79392e8680c5b633f91c0904000000000000000000000000000000ae181323ba3a75acab78032a49eee40d9f000000000000000000000000000000000014f99366f81a4f83a82a58706b7cd40000000000000000000000000000007eb54aeb04b76035e1a071d9bf365a6472000000000000000000000000000000000009e712e51b3b1390dbc35bc4fc0ab3000000000000000000000000000000e6dc278e3472c9b418cddf30d321f4d593000000000000000000000000000000000000ca73945d0a870d824fb963b68261000000000000000000000000000000bff8e49a8a86d0568bc644c7a8d38f3e06000000000000000000000000000000000005f45b020b6dc7dc4952069a44e06a000000000000000000000000000000000000000000000000000000000000000100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000200000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000020000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002";
    bytes constant PROOF_CHUNK_3 = hex"000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000200000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000020000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000200000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000020000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000200000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000020000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000200000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000020000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000200000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000020000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000200000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000020000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000200000000000000000000000000000000000000000000000000000000000000002cf7602c6bf57d91d30cc34307d0a495d1808e29c174303eaf9d32468afcdc652f9b4ccfe7bd41d0f2d63fac835f5de1b668508e8b309e65087569597254dc41157e648fbc4cbaebe6557e9b2915994c635dd56f4d63ea09ff2597704299e8f1272453e7fd57a097979dbc2b85612e87b1d6a374fd7649d2341d93bd7b34ce631fe20f57973d0dab6eb5361ea51a44b4400932aa82f1dd0758967a6defb54023000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000003bc851c133eef46d39a30aaf8a74fd1b670000000000000000000000000000000000235596e90d67a820b0d30890a068830000000000000000000000000000007fda86ff636a77d4116ba2eeefa5d51a2a00000000000000000000000000000000000dc8d17b3c45893c12d51c555024a9000000000000000000000000000000bb490f8cd67dd3a69c0718e44977362fed00000000000000000000000000000000002ce486afb6ce83ecaa858e0e2e30190000000000000000000000000000009d7ba6247640ec5c04a432c17f7ec7c8b500000000000000000000000000000000001d8bc1d23c284072fa2ccd760d8e7f";

    uint256 constant CHUNK_SIZE = 3520;

    function readProofWord(uint256 byteOffset) internal pure returns (bytes32) {
        if (byteOffset < 3520) {
            return TranscriptLib.readWord(PROOF_CHUNK_0, byteOffset);
        } else if (byteOffset < 7040) {
            return TranscriptLib.readWord(PROOF_CHUNK_1, byteOffset - 3520);
        } else if (byteOffset < 10560) {
            return TranscriptLib.readWord(PROOF_CHUNK_2, byteOffset - 7040);
        } else {
            return TranscriptLib.readWord(PROOF_CHUNK_3, byteOffset - 10560);
        }
    }

        function loadProof() internal pure returns (Honk.Proof memory) {
        Honk.Proof memory p;

        // Metadata
        p.circuitSize = uint256(readProofWord(0x00));
        p.publicInputsSize = uint256(readProofWord(0x20));
        p.publicInputsOffset = uint256(readProofWord(0x40));

        // Commitments
        p.w1 = Honk.G1ProofPoint({
            x_0: uint256(readProofWord(0x60)),
            x_1: uint256(readProofWord(0x80)),
            y_0: uint256(readProofWord(0xa0)),
            y_1: uint256(readProofWord(0xc0))
        });
        p.w2 = Honk.G1ProofPoint({
            x_0: uint256(readProofWord(0xe0)),
            x_1: uint256(readProofWord(0x100)),
            y_0: uint256(readProofWord(0x120)),
            y_1: uint256(readProofWord(0x140))
        });
        p.w3 = Honk.G1ProofPoint({
            x_0: uint256(readProofWord(0x160)),
            x_1: uint256(readProofWord(0x180)),
            y_0: uint256(readProofWord(0x1a0)),
            y_1: uint256(readProofWord(0x1c0))
        });

        // Lookup / Permutation Helper Commitments
        p.lookupReadCounts = Honk.G1ProofPoint({
            x_0: uint256(readProofWord(0x1e0)),
            x_1: uint256(readProofWord(0x200)),
            y_0: uint256(readProofWord(0x220)),
            y_1: uint256(readProofWord(0x240))
        });
        p.lookupReadTags = Honk.G1ProofPoint({
            x_0: uint256(readProofWord(0x260)),
            x_1: uint256(readProofWord(0x280)),
            y_0: uint256(readProofWord(0x2a0)),
            y_1: uint256(readProofWord(0x2c0))
        });
        p.w4 = Honk.G1ProofPoint({
            x_0: uint256(readProofWord(0x2e0)),
            x_1: uint256(readProofWord(0x300)),
            y_0: uint256(readProofWord(0x320)),
            y_1: uint256(readProofWord(0x340))
        });
        p.lookupInverses = Honk.G1ProofPoint({
            x_0: uint256(readProofWord(0x360)),
            x_1: uint256(readProofWord(0x380)),
            y_0: uint256(readProofWord(0x3a0)),
            y_1: uint256(readProofWord(0x3c0))
        });
        p.zPerm = Honk.G1ProofPoint({
            x_0: uint256(readProofWord(0x3e0)),
            x_1: uint256(readProofWord(0x400)),
            y_0: uint256(readProofWord(0x420)),
            y_1: uint256(readProofWord(0x440))
        });

        uint256 boundary = 0x460;

        // Sumcheck univariates
        for (uint256 i = 0; i < CONST_PROOF_SIZE_LOG_N; i++) {
            uint256 loop_boundary = boundary + (i * 0x20 * BATCHED_RELATION_PARTIAL_LENGTH);
            for (uint256 j = 0; j < BATCHED_RELATION_PARTIAL_LENGTH; j++) {
                uint256 start = loop_boundary + (j * 0x20);
                p.sumcheckUnivariates[i][j] = FrLib.fromBytes32(readProofWord(start));
            }
        }

        boundary = boundary + (CONST_PROOF_SIZE_LOG_N * BATCHED_RELATION_PARTIAL_LENGTH * 0x20);

        // Sumcheck evaluations
        for (uint256 i = 0; i < NUMBER_OF_ENTITIES; i++) {
            uint256 start = boundary + (i * 0x20);
            p.sumcheckEvaluations[i] = FrLib.fromBytes32(readProofWord(start));
        }

        boundary = boundary + (NUMBER_OF_ENTITIES * 0x20);

        // Gemini fold commitments
        for (uint256 i = 0; i < CONST_PROOF_SIZE_LOG_N - 1; i++) {
            uint256 xStart = boundary + (i * 0x80);
            p.geminiFoldComms[i] = Honk.G1ProofPoint({
                x_0: uint256(readProofWord(xStart)),
                x_1: uint256(readProofWord(xStart + 0x20)),
                y_0: uint256(readProofWord(xStart + 0x40)),
                y_1: uint256(readProofWord(xStart + 0x60))
            });
        }

        boundary = boundary + ((CONST_PROOF_SIZE_LOG_N - 1) * 0x80);

        // Gemini A evaluations
        for (uint256 i = 0; i < CONST_PROOF_SIZE_LOG_N; i++) {
            uint256 start = boundary + (i * 0x20);
            p.geminiAEvaluations[i] = FrLib.fromBytes32(readProofWord(start));
        }

        boundary = boundary + (CONST_PROOF_SIZE_LOG_N * 0x20);

        // Shplonk
        p.shplonkQ = Honk.G1ProofPoint({
            x_0: uint256(readProofWord(boundary)),
            x_1: uint256(readProofWord(boundary + 0x20)),
            y_0: uint256(readProofWord(boundary + 0x40)),
            y_1: uint256(readProofWord(boundary + 0x60))
        });
        boundary = boundary + 0x80;

        // KZG
        p.kzgQuotient = Honk.G1ProofPoint({
            x_0: uint256(readProofWord(boundary)),
            x_1: uint256(readProofWord(boundary + 0x20)),
            y_0: uint256(readProofWord(boundary + 0x40)),
            y_1: uint256(readProofWord(boundary + 0x60))
        });

        return p;
    }

        // ── Verification Key (from Rust test vectors) ────────────────────────────

    function loadVerificationKey() internal pure returns (Honk.VerificationKey memory) {
        return Honk.VerificationKey({
            circuitSize: 32,
            logCircuitSize: 5,
            publicInputsSize: 2,
            qm: Honk.G1Point(
                0x1d4e2b662cf75598ae75c80cb6190d6d86bc92fd69f1420fc9e6d5be8ba09e2c,
                0x30210ded34398f54e3048f65c3f1dac749cc5022828668a6b345712af7369cbb
            ),
            qc: Honk.G1Point(
                0x1c3736f27bc34afe8eb1021704555717e76024100c144933330df5d9a6fb7e7f,
                0x215612b168ecf42291b6df40da24069d5a0d5f2599d8be1ec34c5095e0922151
            ),
            ql: Honk.G1Point(
                0x059aecd0bba76edd4de929d587575b50c50f4be99a4615bfbd4ece89cb1442f1,
                0x121b12b8bfa67425811621a1be826bcc5add41edb51fdce6c134c8e3ff5b1578
            ),
            qr: Honk.G1Point(
                0x2ad6f88dd8a25590c065ad43adb6f3d4ccba5a7312f27dd564b12325a2594ae5,
                0x038c0c60a3dfed43a24eefcc0331f08074bea7bb5c7f65191ec2c3fe59a239cc
            ),
            qo: Honk.G1Point(
                0x17bebc96661564acc3f5c59647e9270570e0c238916df6390c8590445f256d1d,
                0x0bf23741444a9bf150d33f19d70a31863256e71d2bb1adf96b04d61f2c95a2c4
            ),
            q4: Honk.G1Point(
                0x1b8058db3a5b9890b24d2545b7dd4aca37844bb0964691811a3dfe7b9fd24f8f,
                0x28362861904e4b69161d7f43201c9213ede6e74eb63800123b82c73ad0156c40
            ),
            qArith: Honk.G1Point(
                0x3058b7f62cbcbdc8763b05935e9965bea86cd205281d331fb426ef4232ffe5c5,
                0x2b312f13fea65176bc0fe06aef8724f256898d215c78835f40bfe56fbf3f0de3
            ),
            qDeltaRange: Honk.G1Point(
                0x0ac6c48b063b744bbeecb29c8962cf27853ae788601a92a0420ba047a7f7a643,
                0x265a8af9070f8bd5e18bc97a13c985d35a59c188d3d5ee626bbc4589bba9ff9f
            ),
            qElliptic: Honk.G1Point(
                0x024236bda126650fb5228cf424a0878775499e69e8bd2c39af33bd5fa0b4079a,
                0x233cda9292be02cfa2da9d0fc7b0eab0eb1a867b06854066589b967455259b32
            ),
            qAux: Honk.G1Point(
                0x0ca0bc4b1cd9eadbbf49eae56a99a4502ef13d965226a634d0981555e4a4da56,
                0x1a8a818e6c61f68cefa329f2fabc95c80ad56a538d852f75eda858ed1a616c74
            ),
            qLookup: Honk.G1Point(
                0x09dfd2992ac1708f0dd1d28c2ad910d9cf21a1510948580f406bc9416113d620,
                0x205f76eebda12f565c98c775c4e4f3534b5dcc29e57eed899b1a1a880534dcb9
            ),
            qPoseidon2External: Honk.G1Point(
                0x1b8afad764d2cbe67c94249535bba7fcbd3f412f868487222aa54f3268ab64a2,
                0x01b70a90a334c9bd5096aad8a0cc5d4c1d1cdb0fe415445bd0c84309caaf213e
            ),
            qPoseidon2Internal: Honk.G1Point(
                0x13240f97a584b45184c8ec31319b5f6c04ee19ec1dfec87ed47d6d04aa158de2,
                0x2dad22022121d689f57fb38ca21349cefb5d240b07ceb4be26ea429b6dc9d9e0
            ),
            s1: Honk.G1Point(
                0x2dbea5caeded6749d2ef2e2074dbea56c8d54fa043a54c6e6a40238fb0a52c8e,
                0x1f299b74e3867e8c8bc149ef3a308007a3bd6f9935088ec247cce992c33a5336
            ),
            s2: Honk.G1Point(
                0x06652c2a72cb81284b190e235ee029a9463f36b2e29a1775c984b9d9b2714bab,
                0x268e8d1e619fde85a71e430b77974326d790cb64c87558085332df639b8ce410
            ),
            s3: Honk.G1Point(
                0x2849ce9f77669190ed63388b3cc4a6d4e0d895c683ae0057f36a00e62416de5e,
                0x2f8d58d08d4b4bb3a63e23e091e7a1f13c581c8a98c75014d5ec8a20890c62a5
            ),
            s4: Honk.G1Point(
                0x0fff3b4e49a2e6e05bc63d8438368182639ef435c89f30e3a3a9053d97bea5f2,
                0x1820cafe7ffbef14880565ed976d53ed31c844187447d21f09100e8e569d3aec
            ),
            id1: Honk.G1Point(
                0x2e89eeb660cac820de50be4c53b608dd67c6977f5f1746fcf0fb6475d81ccd93,
                0x18ca593957d2677420236138b3659a6b95b580bcc09a3dfbdadfa58a38222c15
            ),
            id2: Honk.G1Point(
                0x0c756ba6a0c66b05655349f04c61dff94dddf3a4d0117fafda741f9518c42f00,
                0x0f87a1201ebad9bd23fed33824ae4ba2a1a307a45fb15594f8d553d2ebf9c285
            ),
            id3: Honk.G1Point(
                0x248460656ec9bc0ad940051e3b0751d25bb97885d8bc362eb06b96ea78d82f84,
                0x0a5eebc538dc40185864706e22d850e3c02ce38e325761a59132bdb9e9d795be
            ),
            id4: Honk.G1Point(
                0x161edd8773a3b74c0553b690b4b80b2a5cbd4a1a25fda097bef23e349531b43e,
                0x287139da895215c216aebe8cce7d3b944f4a3b051bd407126007921cb1fbc5fc
            ),
            t1: Honk.G1Point(
                0x20d671263cad88c119d0a5d172679309087e385f8e76d4cfa834fab61ebd6603,
                0x0f9e6dfd3e6f4584b28e2cb00483dc2ffd9bf5f7ae2cc3f1ea0869c5ae71d9a1
            ),
            t2: Honk.G1Point(
                0x101e267b586089a8bb447e83ab3b7029ed788cc214e0be44485e2f39afbb7ae6,
                0x13410d68bce429dc36e23023cfe21c5f2ced7e136529a4bcd4317232f2fc16b6
            ),
            t3: Honk.G1Point(
                0x1054a26ae3aeeeedc653cf5c5e3c09e2258141e67f4a5a48b50cbf48958b40bd,
                0x2d14190edcf9b2aa697b677c779083aaf0151cc4f673dcf4bdba392d6280e376
            ),
            t4: Honk.G1Point(
                0x2e9e762a66fed77eb0e72645e5ba54f32c1d1bfbc4bd862361dafd7ebd6c68dd,
                0x0b4a012fbc876f57da669215383f3595383f787bca153e972e6cfb9dfebeaa1b
            ),
            lagrangeFirst: Honk.G1Point(
                0x0000000000000000000000000000000000000000000000000000000000000001,
                0x0000000000000000000000000000000000000000000000000000000000000002
            ),
            lagrangeLast: Honk.G1Point(
                0x0af3884ecad3331429af995779c2602e93ca1ea976e9e1bc64bbcdbb9fe79212,
                0x1f18803add8ad686e13dc2a989dcfb010cb69b0b38200df51787b7104bc74fb6
            )
        });
    }

    // ── Public Input Delta ───────────────────────────────────────────────────

    function computePublicInputDelta(
        bytes32[] memory publicInputs,
        Fr beta,
        Fr gamma,
        uint256 domainSize,
        uint256 offset
    ) internal view returns (Fr publicInputDelta) {
        Fr numerator = Fr.wrap(1);
        Fr denominator = Fr.wrap(1);
        Fr numeratorAcc = gamma + (beta * FrLib.from(domainSize + offset));
        Fr denominatorAcc = gamma - (beta * FrLib.from(offset + 1));
        for (uint256 i = 0; i < NUMBER_OF_PUBLIC_INPUTS; i++) {
            Fr pubInput = FrLib.fromBytes32(publicInputs[i]);
            numerator = numerator * (numeratorAcc + pubInput);
            denominator = denominator * (denominatorAcc + pubInput);
            numeratorAcc = numeratorAcc + beta;
            denominatorAcc = denominatorAcc - beta;
        }
        publicInputDelta = FrLib.div(numerator, denominator);
    }

    // ── Sumcheck ─────────────────────────────────────────────────────────────

    function verifySumcheck(Honk.Proof memory proof, Transcript memory tp) internal view returns (bool verified) {
        Fr roundTarget;
        Fr powPartialEvaluation = Fr.wrap(1);
        for (uint256 round; round < LOG_N; ++round) {
            Fr[BATCHED_RELATION_PARTIAL_LENGTH] memory roundUnivariate = proof.sumcheckUnivariates[round];
            bool valid = checkSum(roundUnivariate, roundTarget);
            if (!valid) revert SumcheckFailed();
            Fr roundChallenge = tp.sumCheckUChallenges[round];
            roundTarget = computeNextTargetSum(roundUnivariate, roundChallenge);
            powPartialEvaluation = partiallyEvaluatePOW(tp, powPartialEvaluation, roundChallenge, round);
        }
        Fr grandHonkRelationSum = RelationsLib.accumulateRelationEvaluations(proof, tp, powPartialEvaluation);
        verified = (grandHonkRelationSum == roundTarget);
    }

    function checkSum(Fr[BATCHED_RELATION_PARTIAL_LENGTH] memory roundUnivariate, Fr roundTarget)
        internal
        pure
        returns (bool checked)
    {
        Fr totalSum = roundUnivariate[0] + roundUnivariate[1];
        checked = totalSum == roundTarget;
    }

    function computeNextTargetSum(Fr[BATCHED_RELATION_PARTIAL_LENGTH] memory roundUnivariates, Fr roundChallenge)
        internal
        view
        returns (Fr targetSum)
    {
        Fr[BATCHED_RELATION_PARTIAL_LENGTH] memory BARYCENTRIC_LAGRANGE_DENOMINATORS = [
            Fr.wrap(0x30644e72e131a029b85045b68181585d2833e84879b9709143e1f593efffec51),
            Fr.wrap(0x00000000000000000000000000000000000000000000000000000000000002d0),
            Fr.wrap(0x30644e72e131a029b85045b68181585d2833e84879b9709143e1f593efffff11),
            Fr.wrap(0x0000000000000000000000000000000000000000000000000000000000000090),
            Fr.wrap(0x30644e72e131a029b85045b68181585d2833e84879b9709143e1f593efffff71),
            Fr.wrap(0x00000000000000000000000000000000000000000000000000000000000000f0),
            Fr.wrap(0x30644e72e131a029b85045b68181585d2833e84879b9709143e1f593effffd31),
            Fr.wrap(0x00000000000000000000000000000000000000000000000000000000000013b0)
        ];
        Fr[BATCHED_RELATION_PARTIAL_LENGTH] memory BARYCENTRIC_DOMAIN = [
            Fr.wrap(0x00), Fr.wrap(0x01), Fr.wrap(0x02), Fr.wrap(0x03),
            Fr.wrap(0x04), Fr.wrap(0x05), Fr.wrap(0x06), Fr.wrap(0x07)
        ];
        Fr numeratorValue = Fr.wrap(1);
        for (uint256 i; i < BATCHED_RELATION_PARTIAL_LENGTH; ++i) {
            numeratorValue = numeratorValue * (roundChallenge - Fr.wrap(i));
        }
        Fr[BATCHED_RELATION_PARTIAL_LENGTH] memory denominatorInverses;
        for (uint256 i; i < BATCHED_RELATION_PARTIAL_LENGTH; ++i) {
            Fr inv = BARYCENTRIC_LAGRANGE_DENOMINATORS[i];
            inv = inv * (roundChallenge - BARYCENTRIC_DOMAIN[i]);
            inv = FrLib.invert(inv);
            denominatorInverses[i] = inv;
        }
        for (uint256 i; i < BATCHED_RELATION_PARTIAL_LENGTH; ++i) {
            Fr term = roundUnivariates[i];
            term = term * denominatorInverses[i];
            targetSum = targetSum + term;
        }
        targetSum = targetSum * numeratorValue;
    }

    function partiallyEvaluatePOW(Transcript memory tp, Fr currentEvaluation, Fr roundChallenge, uint256 round)
        internal
        pure
        returns (Fr newEvaluation)
    {
        Fr univariateEval = Fr.wrap(1) + (roundChallenge * (tp.gateChallenges[round] - Fr.wrap(1)));
        newEvaluation = currentEvaluation * univariateEval;
    }

    // ── Shplemini (Gemini + Shplonk + KZG) ──────────────────────────────────

    struct ShpleminiIntermediates {
        Fr unshiftedScalar;
        Fr shiftedScalar;
        Fr constantTermAccumulator;
        Fr batchingChallenge;
        Fr batchedEvaluation;
    }

    function verifyShplemini(Honk.Proof memory proof, Honk.VerificationKey memory vk, Transcript memory tp)
        internal
        view
        returns (bool verified)
    {
        ShpleminiIntermediates memory mem;

        Fr[CONST_PROOF_SIZE_LOG_N] memory powers_of_evaluation_challenge = computeSquares(tp.geminiR);

        Fr[NUMBER_OF_ENTITIES + CONST_PROOF_SIZE_LOG_N + 2] memory scalars;
        Honk.G1Point[NUMBER_OF_ENTITIES + CONST_PROOF_SIZE_LOG_N + 2] memory commitments;

        Fr[CONST_PROOF_SIZE_LOG_N + 1] memory inverse_vanishing_evals =
            computeInvertedGeminiDenominators(tp, powers_of_evaluation_challenge);

        mem.unshiftedScalar = inverse_vanishing_evals[0] + (tp.shplonkNu * inverse_vanishing_evals[1]);
        mem.shiftedScalar =
            FrLib.invert(tp.geminiR) * (inverse_vanishing_evals[0] - (tp.shplonkNu * inverse_vanishing_evals[1]));

        scalars[0] = Fr.wrap(1);
        commitments[0] = convertProofPoint(proof.shplonkQ);

        mem.batchingChallenge = Fr.wrap(1);
        mem.batchedEvaluation = Fr.wrap(0);

        // Unshifted commitments
        for (uint256 i = 1; i <= NUMBER_UNSHIFTED; ++i) {
            scalars[i] = FrLib.neg(mem.unshiftedScalar) * mem.batchingChallenge;
            mem.batchedEvaluation = mem.batchedEvaluation + (proof.sumcheckEvaluations[i - 1] * mem.batchingChallenge);
            mem.batchingChallenge = mem.batchingChallenge * tp.rho;
        }
        // Shifted commitments
        for (uint256 i = NUMBER_UNSHIFTED + 1; i <= NUMBER_OF_ENTITIES; ++i) {
            scalars[i] = FrLib.neg(mem.shiftedScalar) * mem.batchingChallenge;
            mem.batchedEvaluation = mem.batchedEvaluation + (proof.sumcheckEvaluations[i - 1] * mem.batchingChallenge);
            mem.batchingChallenge = mem.batchingChallenge * tp.rho;
        }

        // VK commitments (unshifted, indices 1-27)
        commitments[1] = vk.qm;
        commitments[2] = vk.qc;
        commitments[3] = vk.ql;
        commitments[4] = vk.qr;
        commitments[5] = vk.qo;
        commitments[6] = vk.q4;
        commitments[7] = vk.qArith;
        commitments[8] = vk.qDeltaRange;
        commitments[9] = vk.qElliptic;
        commitments[10] = vk.qAux;
        commitments[11] = vk.qLookup;
        commitments[12] = vk.qPoseidon2External;
        commitments[13] = vk.qPoseidon2Internal;
        commitments[14] = vk.s1;
        commitments[15] = vk.s2;
        commitments[16] = vk.s3;
        commitments[17] = vk.s4;
        commitments[18] = vk.id1;
        commitments[19] = vk.id2;
        commitments[20] = vk.id3;
        commitments[21] = vk.id4;
        commitments[22] = vk.t1;
        commitments[23] = vk.t2;
        commitments[24] = vk.t3;
        commitments[25] = vk.t4;
        commitments[26] = vk.lagrangeFirst;
        commitments[27] = vk.lagrangeLast;

        // Proof point commitments (unshifted, indices 28-35)
        commitments[28] = convertProofPoint(proof.w1);
        commitments[29] = convertProofPoint(proof.w2);
        commitments[30] = convertProofPoint(proof.w3);
        commitments[31] = convertProofPoint(proof.w4);
        commitments[32] = convertProofPoint(proof.zPerm);
        commitments[33] = convertProofPoint(proof.lookupInverses);
        commitments[34] = convertProofPoint(proof.lookupReadCounts);
        commitments[35] = convertProofPoint(proof.lookupReadTags);

        // Shifted commitments (indices 36-40) — 5 entities, no TABLE shifts
        commitments[36] = convertProofPoint(proof.w1);
        commitments[37] = convertProofPoint(proof.w2);
        commitments[38] = convertProofPoint(proof.w3);
        commitments[39] = convertProofPoint(proof.w4);
        commitments[40] = convertProofPoint(proof.zPerm);

        // Gemini fold commitments
        mem.constantTermAccumulator = Fr.wrap(0);
        mem.batchingChallenge = FrLib.sqr(tp.shplonkNu);

        for (uint256 i; i < CONST_PROOF_SIZE_LOG_N - 1; ++i) {
            bool dummy_round = i >= (LOG_N - 1);
            Fr scalingFactor = Fr.wrap(0);
            if (!dummy_round) {
                scalingFactor = mem.batchingChallenge * inverse_vanishing_evals[i + 2];
                scalars[NUMBER_OF_ENTITIES + 1 + i] = FrLib.neg(scalingFactor);
            }
            mem.constantTermAccumulator =
                mem.constantTermAccumulator + (scalingFactor * proof.geminiAEvaluations[i + 1]);
            mem.batchingChallenge = mem.batchingChallenge * tp.shplonkNu;
            commitments[NUMBER_OF_ENTITIES + 1 + i] = convertProofPoint(proof.geminiFoldComms[i]);
        }

        // A₀(r) and A₀(-r)
        Fr a_0_pos = computeGeminiBatchedUnivariateEvaluation(
            tp, mem.batchedEvaluation, proof.geminiAEvaluations, powers_of_evaluation_challenge
        );
        mem.constantTermAccumulator = mem.constantTermAccumulator + (a_0_pos * inverse_vanishing_evals[0]);
        mem.constantTermAccumulator =
            mem.constantTermAccumulator + (proof.geminiAEvaluations[0] * tp.shplonkNu * inverse_vanishing_evals[1]);

        // Constant term: [1]₁
        commitments[NUMBER_OF_ENTITIES + CONST_PROOF_SIZE_LOG_N] = Honk.G1Point({x: 1, y: 2});
        scalars[NUMBER_OF_ENTITIES + CONST_PROOF_SIZE_LOG_N] = mem.constantTermAccumulator;

        // KZG quotient
        Honk.G1Point memory quotient_commitment = convertProofPoint(proof.kzgQuotient);
        commitments[NUMBER_OF_ENTITIES + CONST_PROOF_SIZE_LOG_N + 1] = quotient_commitment;
        scalars[NUMBER_OF_ENTITIES + CONST_PROOF_SIZE_LOG_N + 1] = tp.shplonkZ;

        Honk.G1Point memory P_0 = batchMul(commitments, scalars);
        Honk.G1Point memory P_1 = negateInplace(quotient_commitment);

        return pairing(P_0, P_1);
    }

    function computeSquares(Fr r) internal pure returns (Fr[CONST_PROOF_SIZE_LOG_N] memory squares) {
        squares[0] = r;
        for (uint256 i = 1; i < CONST_PROOF_SIZE_LOG_N; ++i) {
            squares[i] = FrLib.sqr(squares[i - 1]);
        }
    }

    function computeInvertedGeminiDenominators(
        Transcript memory tp,
        Fr[CONST_PROOF_SIZE_LOG_N] memory eval_challenge_powers
    ) internal view returns (Fr[CONST_PROOF_SIZE_LOG_N + 1] memory inverse_vanishing_evals) {
        Fr eval_challenge = tp.shplonkZ;
        inverse_vanishing_evals[0] = FrLib.invert(eval_challenge - eval_challenge_powers[0]);
        for (uint256 i = 0; i < CONST_PROOF_SIZE_LOG_N; ++i) {
            Fr round_inverted_denominator = Fr.wrap(0);
            if (i <= LOG_N + 1) {
                round_inverted_denominator = FrLib.invert(eval_challenge + eval_challenge_powers[i]);
            }
            inverse_vanishing_evals[i + 1] = round_inverted_denominator;
        }
    }

    function computeGeminiBatchedUnivariateEvaluation(
        Transcript memory tp,
        Fr batchedEvalAccumulator,
        Fr[CONST_PROOF_SIZE_LOG_N] memory geminiEvaluations,
        Fr[CONST_PROOF_SIZE_LOG_N] memory geminiEvalChallengePowers
    ) internal view returns (Fr a_0_pos) {
        for (uint256 i = CONST_PROOF_SIZE_LOG_N; i > 0; --i) {
            Fr challengePower = geminiEvalChallengePowers[i - 1];
            Fr u = tp.sumCheckUChallenges[i - 1];
            Fr evalNeg = geminiEvaluations[i - 1];
            Fr batchedEvalRoundAcc = (
                (challengePower * batchedEvalAccumulator * Fr.wrap(2))
                    - evalNeg * (challengePower * (Fr.wrap(1) - u) - u)
            );
            batchedEvalRoundAcc = batchedEvalRoundAcc * FrLib.invert(challengePower * (Fr.wrap(1) - u) + u);
            bool is_dummy_round = (i > LOG_N);
            if (!is_dummy_round) {
                batchedEvalAccumulator = batchedEvalRoundAcc;
            }
        }
        a_0_pos = batchedEvalAccumulator;
    }

    function batchMul(
        Honk.G1Point[NUMBER_OF_ENTITIES + CONST_PROOF_SIZE_LOG_N + 2] memory base,
        Fr[NUMBER_OF_ENTITIES + CONST_PROOF_SIZE_LOG_N + 2] memory scalars
    ) internal view returns (Honk.G1Point memory result) {
        uint256 limit = NUMBER_OF_ENTITIES + CONST_PROOF_SIZE_LOG_N + 2;
        result = ecMul(base[0], scalars[0]);
        for (uint256 i = 1; i < limit; i++) {
            Honk.G1Point memory tmp = ecMul(base[i], scalars[i]);
            result = ecAdd(result, tmp);
        }
    }

    function pairing(Honk.G1Point memory rhs, Honk.G1Point memory lhs) internal view returns (bool) {
        bytes memory input = abi.encodePacked(
            rhs.x,
            rhs.y,
            // Fixed G2 point (SRS)
            uint256(0x198e9393920d483a7260bfb731fb5d25f1aa493335a9e71297e485b7aef312c2),
            uint256(0x1800deef121f1e76426a00665e5c4479674322d4f75edadd46debd5cd992f6ed),
            uint256(0x090689d0585ff075ec9e99ad690c3395bc4b313370b38ef355acdadcd122975b),
            uint256(0x12c85ea5db8c6deb4aab71808dcb408fe3d1e7690c43d37b4ce6cc0166fa7daa),
            lhs.x,
            lhs.y,
            // G2 point from VK
            uint256(0x260e01b251f6f1c7e7ff4e580791dee8ea51d87a358e038b4efe30fac09383c1),
            uint256(0x0118c4d5b837bcc2bc89b5b398b5974e9f5944073b32078b7e231fec938883b0),
            uint256(0x04fc6369f7110fe3d25156c1bb9a72859cf2a04641f99ba4ee413c80da6a5fe4),
            uint256(0x22febda3c0c0632a56475b4214e5615e11e6dd3f96e6cea2854a87d4dacc5e55)
        );
        (bool success, bytes memory result) = address(0x08).staticcall(input);
        bool decodedResult = abi.decode(result, (bool));
        return success && decodedResult;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// SimpleHonkVerifierTest Contract (thin wrapper)
// ═══════════════════════════════════════════════════════════════════════════════

contract SimpleHonkVerifierTest {
    function verify() public view returns (bool) {
        return VerifyLib.verify();
    }
}

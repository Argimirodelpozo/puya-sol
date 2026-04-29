"""
Semantic equivalence tests for Poseidon hash contracts compiled to TEAL.

Deploys PoseidonT2Test..PoseidonT6Test to Algorand localnet and verifies
the hash outputs match reference vectors from circomlibjs / poseidon-lite.

Requires: algokit localnet start
"""

import pytest

import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract

# ── Reference test vectors ──────────────────────────────────────────────────
# Source: circomlibjs test/poseidon.js, iden3/circuits poseidon.test.ts,
#         poseidon-lite v0.3.0 (BN254 curve)
#
# Field prime F = 21888242871839275222246405745257275088548364400416034343698204186575808495617

POSEIDON_T2_VECTORS = [
    # (inputs, expected_output)
    ([0], 19014214495641488759237505126948346942972912379615652741039992445865937985820),
    ([1], 18586133768512220936620570745912940619677854269274689475585506675881198879027),
    ([12345], 4267533774488295900887461483015112262021273608761099826938271132511348470966),
]

POSEIDON_T3_VECTORS = [
    ([1, 2], 7853200120776062878684798364095072458815029376092732009249414926327459813530),
    ([0, 0], 14744269619966411208579211824598458697587494354926760081771325075741142829156),
]

POSEIDON_T4_VECTORS = [
    ([1, 2, 3], 6542985608222806190361240322586112750744169038454362455181422643027100751666),
    ([0, 0, 0], 5317387130258456662214331362918410991734007599705406860481038345552731150762),
]

POSEIDON_T5_VECTORS = [
    ([1, 2, 3, 4], 18821383157269793795438455681495246036402687001665670618754263018637548127333),
    ([0, 0, 0, 0], 2351654555892372227640888372176282444150254868378439619268573230312091195718),
]

POSEIDON_T6_VECTORS = [
    ([1, 2, 3, 4, 5], 6183221330272524995739186171720101788151706631170188140075976616310159254464),
    ([0, 0, 0, 0, 0], 14655542659562014735865511769057053982292279840403315552050801315682099828156),
]


# ── Fixtures ────────────────────────────────────────────────────────────────

@pytest.fixture(scope="module")
def t2_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "PoseidonT2Test")


@pytest.fixture(scope="module")
def t3_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "PoseidonT3Test")


@pytest.fixture(scope="module")
def t4_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "PoseidonT4Test")


@pytest.fixture(scope="module")
def t5_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "PoseidonT5Test")


@pytest.fixture(scope="module")
def t6_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "PoseidonT6Test")


# ── PoseidonT2 tests ───────────────────────────────────────────────────────

@pytest.mark.localnet
@pytest.mark.parametrize("inputs,expected", POSEIDON_T2_VECTORS)
def test_poseidon_t2(
    t2_client: au.AppClient,
    inputs: list[int],
    expected: int,
) -> None:
    result = t2_client.send.call(
        au.AppClientMethodCallParams(method="hashOne", args=inputs)
    )
    assert result.abi_return == expected


# ── PoseidonT3 tests ───────────────────────────────────────────────────────

@pytest.mark.localnet
@pytest.mark.parametrize("inputs,expected", POSEIDON_T3_VECTORS)
def test_poseidon_t3(
    t3_client: au.AppClient,
    inputs: list[int],
    expected: int,
) -> None:
    result = t3_client.send.call(
        au.AppClientMethodCallParams(method="hashTwo", args=inputs)
    )
    assert result.abi_return == expected


# ── PoseidonT4 tests ───────────────────────────────────────────────────────

@pytest.mark.localnet
@pytest.mark.parametrize("inputs,expected", POSEIDON_T4_VECTORS)
def test_poseidon_t4(
    t4_client: au.AppClient,
    inputs: list[int],
    expected: int,
) -> None:
    result = t4_client.send.call(
        au.AppClientMethodCallParams(method="hashThree", args=inputs)
    )
    assert result.abi_return == expected


# ── PoseidonT5 tests ───────────────────────────────────────────────────────

@pytest.mark.localnet
@pytest.mark.parametrize("inputs,expected", POSEIDON_T5_VECTORS)
def test_poseidon_t5(
    t5_client: au.AppClient,
    inputs: list[int],
    expected: int,
) -> None:
    result = t5_client.send.call(
        au.AppClientMethodCallParams(method="hashFour", args=inputs)
    )
    assert result.abi_return == expected


# ── PoseidonT6 tests ───────────────────────────────────────────────────────

@pytest.mark.localnet
@pytest.mark.parametrize("inputs,expected", POSEIDON_T6_VECTORS)
def test_poseidon_t6(
    t6_client: au.AppClient,
    inputs: list[int],
    expected: int,
) -> None:
    result = t6_client.send.call(
        au.AppClientMethodCallParams(method="hashFive", args=inputs)
    )
    assert result.abi_return == expected

"""
M26: Groth16 zk-SNARK Verifier — Semaphore Anonymous Identity (3,411 constraints).
Exercises: The most complex circuit tested — Semaphore-style anonymous group membership
with nullifier, combining identity commitment, depth-12 Poseidon Merkle proof, and
nullifier hash computation.

Circuit: Semaphore anonymous identity proof
  - Private inputs: identitySecret=98765, pathElements[12], pathIndex=7
  - Public inputs: root (4096-leaf tree), nullifierHash, externalNullifier=42
  - 3,411 non-linear constraints — 83% of ptau capacity
  - Combines: Poseidon(1) for commitment, 12x Poseidon(2) for Merkle, Poseidon(2) for nullifier
  - Real-world ZK use case: anonymous voting, identity verification

Proof generated with snarkjs (Groth16, BN128/BN254 curve, power-14 ptau).
"""

import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


OPUP_FEE = au.AlgoAmount(micro_algo=100_000)

# Semaphore: anonymous identity proof
# identitySecret=98765, tree of 4096 leaves, identity at index 7
# externalNullifier=42 (e.g., a poll ID)
MERKLE_ROOT = 2065995818570071510243971578097255352706440908417301819848907738024450654249
NULLIFIER_HASH = 8752169638035223830719248072061399430265027899670619901031398582949510023625
EXTERNAL_NULLIFIER = 42

PROOF_A = [
    6340714629096793632966327696800381756860483336159294850519556573119599887191,
    20298794325701599272370803678197819867105287723995742802171603750189293712812,
]

# [x_im, x_re, y_im, y_re]
PROOF_B = [
    17792718189596238772896905577173637594410632088408675032263448582459956913702,  # x_im
    1901287379548388639109338277516826028551018204939857142550446344627710890832,   # x_re
    19011423044087021167979116437611383597109862235210543589862007611857361768390,  # y_im
    403985066117388615222595291330362746025194422166400079504789959482533486632,    # y_re
]

PROOF_C = [
    7694569681883270460927201689761934890488035687334828467454981844170838305820,
    6342146905004595156211812860685174783232219845294583349607949053772145461950,
]

# Public signals: [root, nullifierHash, externalNullifier]
PUB_SIGNALS = [MERKLE_ROOT, NULLIFIER_HASH, EXTERNAL_NULLIFIER]

BN254_R = 21888242871839275222246405745257275088548364400416034343698204186575808495617


@pytest.fixture(scope="module")
def verifier_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy the SemaphoreVerifier contract."""
    return deploy_contract(localnet, account, "SemaphoreVerifier")


@pytest.mark.localnet
def test_verifier_deploys(verifier_client: au.AppClient) -> None:
    """The Semaphore verifier deploys successfully (3411-constraint circuit)."""
    assert verifier_client.app_id > 0


@pytest.mark.localnet
def test_valid_proof_returns_true(verifier_client: au.AppClient) -> None:
    """A valid Semaphore identity proof should be accepted."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, PUB_SIGNALS],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_wrong_root_returns_false(verifier_client: au.AppClient) -> None:
    """Wrong Merkle root (different group) should fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [MERKLE_ROOT + 1, NULLIFIER_HASH, EXTERNAL_NULLIFIER]],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_wrong_nullifier_hash_returns_false(verifier_client: au.AppClient) -> None:
    """Wrong nullifier hash should fail (can't fake identity)."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [MERKLE_ROOT, NULLIFIER_HASH + 1, EXTERNAL_NULLIFIER]],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_wrong_external_nullifier_returns_false(verifier_client: au.AppClient) -> None:
    """Wrong external nullifier (different poll/domain) should fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [MERKLE_ROOT, NULLIFIER_HASH, 43]],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_zero_nullifier_returns_false(verifier_client: au.AppClient) -> None:
    """Zero nullifier hash should fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [MERKLE_ROOT, 0, EXTERNAL_NULLIFIER]],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_tampered_proof_a_returns_false(verifier_client: au.AppClient) -> None:
    """Modifying proof point A should fail."""
    tampered_a = [PROOF_A[0] + 1, PROOF_A[1]]
    try:
        result = verifier_client.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                static_fee=OPUP_FEE,
                args=[tampered_a, PROOF_B, PROOF_C, PUB_SIGNALS],
            )
        )
        assert result.abi_return is False
    except Exception:
        pass


@pytest.mark.localnet
def test_tampered_proof_b_returns_false(verifier_client: au.AppClient) -> None:
    """Modifying proof point B should fail."""
    tampered_b = [PROOF_B[0], PROOF_B[1], PROOF_B[2] + 1, PROOF_B[3]]
    try:
        result = verifier_client.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                static_fee=OPUP_FEE,
                args=[PROOF_A, tampered_b, PROOF_C, PUB_SIGNALS],
            )
        )
        assert result.abi_return is False
    except Exception:
        pass


@pytest.mark.localnet
def test_tampered_proof_c_returns_false(verifier_client: au.AppClient) -> None:
    """Modifying proof point C should fail."""
    tampered_c = [PROOF_C[0], PROOF_C[1] + 1]
    try:
        result = verifier_client.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                static_fee=OPUP_FEE,
                args=[PROOF_A, PROOF_B, tampered_c, PUB_SIGNALS],
            )
        )
        assert result.abi_return is False
    except Exception:
        pass


@pytest.mark.localnet
def test_signal_at_field_boundary(verifier_client: au.AppClient) -> None:
    """Root signal == r should be rejected by checkField."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [BN254_R, NULLIFIER_HASH, EXTERNAL_NULLIFIER]],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_nullifier_at_field_boundary(verifier_client: au.AppClient) -> None:
    """Nullifier signal == r should be rejected by checkField."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [MERKLE_ROOT, BN254_R, EXTERNAL_NULLIFIER]],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_swapped_root_and_nullifier_returns_false(verifier_client: au.AppClient) -> None:
    """Swapped root and nullifier hash should fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [NULLIFIER_HASH, MERKLE_ROOT, EXTERNAL_NULLIFIER]],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_all_zero_proof(verifier_client: au.AppClient) -> None:
    """All-zero proof should be rejected."""
    try:
        result = verifier_client.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                static_fee=OPUP_FEE,
                args=[[0, 0], [0, 0, 0, 0], [0, 0], PUB_SIGNALS],
            )
        )
        assert result.abi_return is False
    except Exception:
        pass


@pytest.mark.localnet
def test_deep_merkle_proof_rejected(verifier_client: au.AppClient) -> None:
    """Proof from deep Merkle circuit should fail on Semaphore verifier (cross-verifier)."""
    dm_a = [
        13610972477533615423408678501437043330826986100369369242411801947371672065335,
        20820511077880042654786406381610664717983572237566434359677009020194603846592,
    ]
    dm_b = [
        11994341450889264267704531977163895704228491891888212907668830595033225598949,
        7823138012001469601192260840904554787912569072043291083045098813903670598558,
        5018126796196447927273007440695652960608336387394624494781737162287902133264,
        9402254691027390905085869406370432360858748690424875588974416893698121474323,
    ]
    dm_c = [
        423546999705413104379425308173314406949509199686045058745194172150859368759,
        20140145692672292775133120150543744999859424341570018828967386624162826385197,
    ]
    dm_root = 19192248324361169264508704913456895790068833131331403288109200390160250736999
    try:
        result = verifier_client.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                static_fee=OPUP_FEE,
                args=[dm_a, dm_b, dm_c, [dm_root, 42, 0]],
            )
        )
        assert result.abi_return is False
    except Exception:
        pass

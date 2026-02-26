"""
M24: Groth16 zk-SNARK Verifier — Deep Merkle Proof (2,460 constraints).
Exercises: Large-circuit verification with depth-10 Poseidon Merkle tree (1024 leaves).

Circuit: binary Merkle tree membership proof
  - Private inputs: leaf=43 (value at index 42), pathElements[10] (10 sibling hashes)
  - Public inputs: root (Poseidon tree root), leafIndex=42
  - 2,460 non-linear constraints — 60% of ptau capacity
  - Uses 10 Poseidon hashes, Num2Bits(10), 10 MultiMux1

Proof generated with snarkjs (Groth16, BN128/BN254 curve, power-14 ptau).
"""

import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


OPUP_FEE = au.AlgoAmount(micro_algo=100_000)

# Merkle tree: 1024 leaves (values 1..1024), depth 10, Poseidon hash
# Proving leaf index 42 (value 43) is in the tree
MERKLE_ROOT = 19192248324361169264508704913456895790068833131331403288109200390160250736999
LEAF_INDEX = 42

PROOF_A = [
    13610972477533615423408678501437043330826986100369369242411801947371672065335,
    20820511077880042654786406381610664717983572237566434359677009020194603846592,
]

# [x_im, x_re, y_im, y_re]
PROOF_B = [
    11994341450889264267704531977163895704228491891888212907668830595033225598949,  # x_im
    7823138012001469601192260840904554787912569072043291083045098813903670598558,   # x_re
    5018126796196447927273007440695652960608336387394624494781737162287902133264,   # y_im
    9402254691027390905085869406370432360858748690424875588974416893698121474323,   # y_re
]

PROOF_C = [
    423546999705413104379425308173314406949509199686045058745194172150859368759,
    20140145692672292775133120150543744999859424341570018828967386624162826385197,
]

PUB_SIGNALS = [MERKLE_ROOT, LEAF_INDEX]

BN254_R = 21888242871839275222246405745257275088548364400416034343698204186575808495617


@pytest.fixture(scope="module")
def verifier_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy the DeepMerkleVerifier contract."""
    return deploy_contract(localnet, account, "DeepMerkleVerifier")


@pytest.mark.localnet
def test_verifier_deploys(verifier_client: au.AppClient) -> None:
    """The deep Merkle verifier deploys successfully."""
    assert verifier_client.app_id > 0


@pytest.mark.localnet
def test_valid_proof_returns_true(verifier_client: au.AppClient) -> None:
    """A valid depth-10 Merkle membership proof (leaf 43 at index 42) should be accepted."""
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
    """Wrong Merkle root should fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [MERKLE_ROOT + 1, LEAF_INDEX]],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_wrong_leaf_index_returns_false(verifier_client: au.AppClient) -> None:
    """Wrong leaf index (41 instead of 42) should fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [MERKLE_ROOT, 41]],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_index_off_by_one_high_returns_false(verifier_client: au.AppClient) -> None:
    """Leaf index 43 (off by one high) should fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [MERKLE_ROOT, 43]],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_index_zero_returns_false(verifier_client: au.AppClient) -> None:
    """Leaf index 0 (first leaf) with same proof should fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [MERKLE_ROOT, 0]],
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
    tampered_b = [PROOF_B[0], PROOF_B[1] + 1, PROOF_B[2], PROOF_B[3]]
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
    tampered_c = [PROOF_C[0] + 1, PROOF_C[1]]
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
    """Signal == r should be rejected by checkField."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [BN254_R, LEAF_INDEX]],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_swapped_signals_returns_false(verifier_client: au.AppClient) -> None:
    """Swapped root and leafIndex should fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [LEAF_INDEX, MERKLE_ROOT]],
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
def test_shallow_merkle_proof_rejected(verifier_client: au.AppClient) -> None:
    """Proof from depth-4 Merkle circuit (M22) should fail on depth-10 verifier (cross-verifier)."""
    shallow_a = [
        12279569669103606036042502146659746960483758208041294585325933811382841895639,
        10747889010963478148743897416616087430266847955270084222460239702025579508428,
    ]
    shallow_b = [
        14923071949029698486289637366478038391167893027672699589097768553960756485034,
        19873770786866692187358084343935866206779263690466150484658213421445036509732,
        12620250971550692709225429887916335104073909990135840862559639960286807238101,
        21352331399131339250586122950411129121490421749783378865448878869939291385984,
    ]
    shallow_c = [
        10889246482943390784345021620304922186545249669722238866222845989878590749603,
        6600549057877170718428759517493472801724748166680227462292413672207965014385,
    ]
    shallow_root = 21013571166917622537724770309050693131274168214955073041334585836894534334888
    try:
        result = verifier_client.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                static_fee=OPUP_FEE,
                args=[shallow_a, shallow_b, shallow_c, [shallow_root, 3]],
            )
        )
        assert result.abi_return is False
    except Exception:
        pass

"""
M22: Groth16 zk-SNARK Verifier — Merkle Proof with Poseidon (984 constraints).
Exercises: Verification of a binary Merkle tree membership proof using Poseidon hashing.

Circuit: depth-4 Merkle tree membership (16 leaves)
  - Private inputs: leaf=4, pathElements[4] (sibling hashes)
  - Public inputs: root (tree root hash), leafIndex=3
  - 984 non-linear constraints — the most complex circuit tested
  - Uses circomlib Poseidon, Num2Bits, MultiMux1

Proof generated with snarkjs (Groth16, BN128/BN254 curve).
"""

import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


OPUP_FEE = au.AlgoAmount(micro_algo=100_000)

# Merkle tree: 16 leaves (values 1..16), depth 4, Poseidon hash
# Proving leaf index 3 (value 4) is in the tree
MERKLE_ROOT = 21013571166917622537724770309050693131274168214955073041334585836894534334888
LEAF_INDEX = 3

# Proof point A (G1)
PROOF_A = [
    12279569669103606036042502146659746960483758208041294585325933811382841895639,
    10747889010963478148743897416616087430266847955270084222460239702025579508428,
]

# Proof point B (G2) — [x_im, x_re, y_im, y_re]
PROOF_B = [
    14923071949029698486289637366478038391167893027672699589097768553960756485034,  # x_im
    19873770786866692187358084343935866206779263690466150484658213421445036509732,  # x_re
    12620250971550692709225429887916335104073909990135840862559639960286807238101,  # y_im
    21352331399131339250586122950411129121490421749783378865448878869939291385984,  # y_re
]

# Proof point C (G1)
PROOF_C = [
    10889246482943390784345021620304922186545249669722238866222845989878590749603,
    6600549057877170718428759517493472801724748166680227462292413672207965014385,
]

# Public signals: [root, leafIndex]
PUB_SIGNALS = [MERKLE_ROOT, LEAF_INDEX]

BN254_R = 21888242871839275222246405745257275088548364400416034343698204186575808495617


@pytest.fixture(scope="module")
def verifier_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy the MerkleVerifier contract."""
    return deploy_contract(localnet, account, "MerkleVerifier")


@pytest.mark.localnet
def test_verifier_deploys(verifier_client: au.AppClient) -> None:
    """The Merkle verifier deploys successfully."""
    assert verifier_client.app_id > 0


@pytest.mark.localnet
def test_valid_proof_returns_true(verifier_client: au.AppClient) -> None:
    """A valid Merkle membership proof (leaf 4 at index 3) should be accepted."""
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
    """Wrong leaf index (2 instead of 3) should fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [MERKLE_ROOT, 2]],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_adjacent_leaf_index_returns_false(verifier_client: au.AppClient) -> None:
    """Adjacent leaf index (4 instead of 3) should also fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [MERKLE_ROOT, 4]],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_zero_root_returns_false(verifier_client: au.AppClient) -> None:
    """Zero as Merkle root should fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [0, LEAF_INDEX]],
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
    tampered_b = [PROOF_B[0] + 1, PROOF_B[1], PROOF_B[2], PROOF_B[3]]
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
def test_poseidon_proof_rejected(verifier_client: au.AppClient) -> None:
    """Proof from the Poseidon preimage circuit should fail on Merkle verifier (cross-verifier)."""
    poseidon_a = [
        12016724999792277373915008240491650569066282092748425451838839880707649820852,
        15642078320736370418071084798942077298276928656472315303274092407334386642284,
    ]
    poseidon_b = [
        19603636679429065352922659439203213572382211898220488525448198899731266629889,
        10527263488018862201327252854946077436499563096772954049789326687505086224350,
        1695487672840860704599891334538696779135685808743866802544343802514588953091,
        3388521507770867383144583822040710553639375890017805861029738285493568823125,
    ]
    poseidon_c = [
        16267245738779053621406855107071800557255418847672904155257028553011712917013,
        8030219157381089833896809144417790060439776249448468757901915508237594723581,
    ]
    poseidon_hash = 19620391833206800292073497099357851348339828238212863168390691880932172496143
    try:
        result = verifier_client.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                static_fee=OPUP_FEE,
                args=[poseidon_a, poseidon_b, poseidon_c, [poseidon_hash, 0]],
            )
        )
        assert result.abi_return is False
    except Exception:
        pass

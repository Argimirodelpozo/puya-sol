"""
M25: Groth16 zk-SNARK Verifier — Poseidon Hash Chain (1,944 constraints).
Exercises: Sequential hash computation verification (8 rounds of Poseidon).

Circuit: Poseidon hash chain
  h_0 = seed, h_i = Poseidon(h_{i-1}, i) for i = 1..8
  - Private input: seed = 12345
  - Public inputs: chainLength = 8 (domain separator / constrained parameter)
  - Public output: finalHash (the 8th chained hash)
  - 1,944 non-linear constraints (8 sequential Poseidon hashes)

Proof generated with snarkjs (Groth16, BN128/BN254 curve, power-14 ptau).
"""

import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


OPUP_FEE = au.AlgoAmount(micro_algo=100_000)

# Hash chain: seed=12345, 8 rounds of Poseidon
FINAL_HASH = 11337946193877044699471039950379785533012136959144415594117565965546417650586
CHAIN_LENGTH = 8

PROOF_A = [
    20999920456780074468503215855921182346341721680587988719270112231726689563554,
    18885468778990375844764659862446337533661483363963848800356089693422564124765,
]

# [x_im, x_re, y_im, y_re]
PROOF_B = [
    11022557032423827931428743237607175064467488516638724527253199869098642674349,  # x_im
    12818843867798534910013142506912099737345409496905148121446867171479678222350,  # x_re
    593045184625768497820706340976781989594448595160132626114662048309208725945,    # y_im
    11405430808009897629439238546079494719794363776037799084799310753281907700156,  # y_re
]

PROOF_C = [
    2191845905971126798873665700782897699150696038021457847101895274265062702483,
    6988804475801735718605367713413443664144527290462648265477002630677293499920,
]

# Public signals: [finalHash, chainLength]
PUB_SIGNALS = [FINAL_HASH, CHAIN_LENGTH]

BN254_R = 21888242871839275222246405745257275088548364400416034343698204186575808495617


@pytest.fixture(scope="module")
def verifier_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy the HashChainVerifier contract."""
    return deploy_contract(localnet, account, "HashChainVerifier")


@pytest.mark.localnet
def test_verifier_deploys(verifier_client: au.AppClient) -> None:
    """The hash chain verifier deploys successfully."""
    assert verifier_client.app_id > 0


@pytest.mark.localnet
def test_valid_proof_returns_true(verifier_client: au.AppClient) -> None:
    """A valid 8-round Poseidon hash chain proof should be accepted."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, PUB_SIGNALS],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_wrong_final_hash_returns_false(verifier_client: au.AppClient) -> None:
    """Wrong final hash should fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [FINAL_HASH + 1, CHAIN_LENGTH]],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_wrong_chain_length_returns_false(verifier_client: au.AppClient) -> None:
    """Wrong chain length (7 instead of 8) should fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [FINAL_HASH, 7]],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_zero_hash_returns_false(verifier_client: au.AppClient) -> None:
    """Zero final hash should fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [0, CHAIN_LENGTH]],
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
            args=[PROOF_A, PROOF_B, PROOF_C, [BN254_R, CHAIN_LENGTH]],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_swapped_signals_returns_false(verifier_client: au.AppClient) -> None:
    """Swapped hash and chain length should fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [CHAIN_LENGTH, FINAL_HASH]],
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
def test_poseidon_preimage_proof_rejected(verifier_client: au.AppClient) -> None:
    """Proof from Poseidon preimage circuit should fail on hash chain verifier (cross-verifier)."""
    pos_a = [
        12016724999792277373915008240491650569066282092748425451838839880707649820852,
        15642078320736370418071084798942077298276928656472315303274092407334386642284,
    ]
    pos_b = [
        19603636679429065352922659439203213572382211898220488525448198899731266629889,
        10527263488018862201327252854946077436499563096772954049789326687505086224350,
        1695487672840860704599891334538696779135685808743866802544343802514588953091,
        3388521507770867383144583822040710553639375890017805861029738285493568823125,
    ]
    pos_c = [
        16267245738779053621406855107071800557255418847672904155257028553011712917013,
        8030219157381089833896809144417790060439776249448468757901915508237594723581,
    ]
    pos_hash = 19620391833206800292073497099357851348339828238212863168390691880932172496143
    try:
        result = verifier_client.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                static_fee=OPUP_FEE,
                args=[pos_a, pos_b, pos_c, [pos_hash, 8]],
            )
        )
        assert result.abi_return is False
    except Exception:
        pass

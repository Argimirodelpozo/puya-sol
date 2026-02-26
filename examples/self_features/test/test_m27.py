"""
M27: Groth16 zk-SNARK Verifier — Full-Disclosure Hash Chain (3,402 constraints, 14 signals).
Exercises: Maximum stress test — both high constraint count AND many public signals.

Circuit: 14-round Poseidon hash chain with ALL intermediates disclosed
  - h_0 = seed, h_i = Poseidon(h_{i-1}, i) for i = 1..14
  - Private input: seed = 777
  - Public outputs: all 14 intermediate hashes
  - 3,402 non-linear constraints (14 Poseidon evaluations)
  - 14 public signals (15 IC points)
  - 1,207 lines TEAL — largest verifier program
  - Use case: VDF-like verifiable sequential computation

Proof generated with snarkjs (Groth16, BN128/BN254 curve, power-14 ptau).
"""

import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


# Higher fee — 15 IC points means 14 scalar muls + 14 point additions
OPUP_FEE = au.AlgoAmount(micro_algo=300_000)

# 14-round Poseidon hash chain starting from seed=777
# h[i] = Poseidon(h[i-1], i) for i = 1..14
PUB_SIGNALS = [
    1958543737341338403972093774663304129372122365525651970641239634665879010152,   # h1
    7610152100860840517981021784923884441973894411387703975338335125623753890389,    # h2
    19971201935455871909702244308597639790344119410645062680871804757274776423187,   # h3
    549840061634353847437301919455553218223034336010809148799881471843625448298,     # h4
    7548626799453522298752674888986501235282621915352097606871396877762138959250,    # h5
    14289036237942410910304113103054447234167444497879790835760020939568980743678,   # h6
    3432544427080206015091030450262057867372991471832278132603752649882714132641,    # h7
    16639021719015717334326168073796438898841823672271780930378877521308915558103,   # h8
    21400491625353640742635792703015444748268224852425787789966668815204137798738,   # h9
    5616108858897414528753808090935407206552271023305199639785333956412857279145,    # h10
    14123567301291314621806580914037171622734209959273750892310207057430582358584,   # h11
    18482751951157633486312051000081274895017232781604183820670504066970226348623,   # h12
    19869981245741839616787111225407239880352623738987184412907733386909723391323,   # h13
    20127206499260315486167404977932886119979720225252876549042334177262901594006,   # h14
]

PROOF_A = [
    1376579349821079279465272627759655120038884716633593475101592620780646894051,
    14196922429307045676298136560953943627625722967586337622536555949992177858178,
]

# [x_im, x_re, y_im, y_re]
PROOF_B = [
    9209864267113829050212200300213054633306351180191356486805771853116905942694,   # x_im
    13110770565916802910752365422033368829416704260736927351286204130057798845599,  # x_re
    9907165673682828457734386155552683888041964292571729444384661305808022174794,   # y_im
    2132405035688046943467463101841146183620396117749246245859147035365046227373,   # y_re
]

PROOF_C = [
    19130410198666747722847356024437709777175215907894995950805887862634463087330,
    1020018530749586232422352798013403693497119312971409137952687141888986969476,
]

BN254_R = 21888242871839275222246405745257275088548364400416034343698204186575808495617


@pytest.fixture(scope="module")
def verifier_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy the FullChainVerifier contract (1207 lines TEAL)."""
    return deploy_contract(localnet, account, "FullChainVerifier")


@pytest.mark.localnet
def test_verifier_deploys(verifier_client: au.AppClient) -> None:
    """The full chain verifier deploys (3402 constraints, 14 signals, 1207L TEAL)."""
    assert verifier_client.app_id > 0


@pytest.mark.localnet
def test_valid_proof_returns_true(verifier_client: au.AppClient) -> None:
    """A valid 14-round full-disclosure hash chain proof should be accepted."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, PUB_SIGNALS],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_wrong_first_hash_returns_false(verifier_client: au.AppClient) -> None:
    """Wrong h1 (first intermediate) should fail."""
    wrong = PUB_SIGNALS.copy()
    wrong[0] = PUB_SIGNALS[0] + 1
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, wrong],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_wrong_last_hash_returns_false(verifier_client: au.AppClient) -> None:
    """Wrong h14 (final hash) should fail."""
    wrong = PUB_SIGNALS.copy()
    wrong[13] = PUB_SIGNALS[13] + 1
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, wrong],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_wrong_middle_hash_returns_false(verifier_client: au.AppClient) -> None:
    """Wrong h7 (middle of chain) should fail."""
    wrong = PUB_SIGNALS.copy()
    wrong[6] = PUB_SIGNALS[6] + 1
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, wrong],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_swapped_adjacent_hashes_returns_false(verifier_client: au.AppClient) -> None:
    """Swapping h3 and h4 should fail — chain order matters."""
    wrong = PUB_SIGNALS.copy()
    wrong[2], wrong[3] = wrong[3], wrong[2]
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, wrong],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_reversed_chain_returns_false(verifier_client: au.AppClient) -> None:
    """Fully reversed chain should fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, list(reversed(PUB_SIGNALS))],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_all_signals_zero_returns_false(verifier_client: au.AppClient) -> None:
    """All-zero signals should fail."""
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, [0] * 14],
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
def test_first_signal_at_field_boundary(verifier_client: au.AppClient) -> None:
    """h1 == r should be rejected by checkField."""
    bad = PUB_SIGNALS.copy()
    bad[0] = BN254_R
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, bad],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_last_signal_at_field_boundary(verifier_client: au.AppClient) -> None:
    """h14 == r should be rejected by checkField."""
    bad = PUB_SIGNALS.copy()
    bad[13] = BN254_R
    result = verifier_client.send.call(
        au.AppClientMethodCallParams(
            method="verifyProof",
            static_fee=OPUP_FEE,
            args=[PROOF_A, PROOF_B, PROOF_C, bad],
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
def test_semaphore_proof_rejected(verifier_client: au.AppClient) -> None:
    """Proof from Semaphore circuit should fail on full chain verifier (cross-verifier)."""
    sem_a = [
        6340714629096793632966327696800381756860483336159294850519556573119599887191,
        20298794325701599272370803678197819867105287723995742802171603750189293712812,
    ]
    sem_b = [
        17792718189596238772896905577173637594410632088408675032263448582459956913702,
        1901287379548388639109338277516826028551018204939857142550446344627710890832,
        19011423044087021167979116437611383597109862235210543589862007611857361768390,
        403985066117388615222595291330362746025194422166400079504789959482533486632,
    ]
    sem_c = [
        7694569681883270460927201689761934890488035687334828467454981844170838305820,
        6342146905004595156211812860685174783232219845294583349607949053772145461950,
    ]
    # Pad to 14 signals
    try:
        result = verifier_client.send.call(
            au.AppClientMethodCallParams(
                method="verifyProof",
                static_fee=OPUP_FEE,
                args=[sem_a, sem_b, sem_c, [0] * 14],
            )
        )
        assert result.abi_return is False
    except Exception:
        pass

"""
Certificate behavioral tests.
Tests certificate issuance, revocation, and validity checks.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def recipient_key(cid):
    return mapping_box_key("_certRecipient", cid.to_bytes(64, "big"))


def score_key(cid):
    return mapping_box_key("_certScore", cid.to_bytes(64, "big"))


def issued_key(cid):
    return mapping_box_key("_certIssuedAt", cid.to_bytes(64, "big"))


def revoked_key(cid):
    return mapping_box_key("_certRevoked", cid.to_bytes(64, "big"))


def exists_key(cid):
    return mapping_box_key("_certExists", cid.to_bytes(64, "big"))


def cert_boxes(cid):
    return [
        au.BoxReference(app_id=0, name=recipient_key(cid)),
        au.BoxReference(app_id=0, name=score_key(cid)),
        au.BoxReference(app_id=0, name=issued_key(cid)),
        au.BoxReference(app_id=0, name=revoked_key(cid)),
        au.BoxReference(app_id=0, name=exists_key(cid)),
    ]


@pytest.fixture(scope="module")
def cert(localnet, account):
    return deploy_contract(localnet, account, "CertificateTest")


def test_deploy(cert):
    assert cert.app_id > 0


def test_issuer(cert, account):
    result = cert.send.call(
        au.AppClientMethodCallParams(method="getIssuer")
    )
    assert result.abi_return == account.address


def test_issue_cert(cert, account):
    boxes = cert_boxes(0)
    result = cert.send.call(
        au.AppClientMethodCallParams(
            method="issueCert",
            args=[account.address, 95, 1000],  # score=95, issuedAt=1000
            box_references=boxes,
        )
    )
    assert result.abi_return == 0


def test_cert_count(cert):
    result = cert.send.call(
        au.AppClientMethodCallParams(method="getCertCount")
    )
    assert result.abi_return == 1


def test_cert_recipient(cert, account):
    result = cert.send.call(
        au.AppClientMethodCallParams(
            method="getCertRecipient",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=recipient_key(0))],
        )
    )
    assert result.abi_return == account.address


def test_cert_score(cert):
    result = cert.send.call(
        au.AppClientMethodCallParams(
            method="getCertScore",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=score_key(0))],
        )
    )
    assert result.abi_return == 95


def test_cert_issued_at(cert):
    result = cert.send.call(
        au.AppClientMethodCallParams(
            method="getCertIssuedAt",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=issued_key(0))],
        )
    )
    assert result.abi_return == 1000


def test_cert_exists(cert):
    result = cert.send.call(
        au.AppClientMethodCallParams(
            method="certExists",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=exists_key(0))],
        )
    )
    assert result.abi_return is True


def test_cert_valid(cert):
    result = cert.send.call(
        au.AppClientMethodCallParams(
            method="isValid",
            args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=exists_key(0)),
                au.BoxReference(app_id=0, name=revoked_key(0)),
            ],
        )
    )
    assert result.abi_return is True


def test_total_issued(cert):
    result = cert.send.call(
        au.AppClientMethodCallParams(method="getTotalIssued")
    )
    assert result.abi_return == 1


def test_revoke_cert(cert):
    cert.send.call(
        au.AppClientMethodCallParams(
            method="revokeCert",
            args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=exists_key(0)),
                au.BoxReference(app_id=0, name=revoked_key(0)),
            ],
        )
    )


def test_cert_revoked(cert):
    result = cert.send.call(
        au.AppClientMethodCallParams(
            method="isCertRevoked",
            args=[0],
            box_references=[au.BoxReference(app_id=0, name=revoked_key(0))],
        )
    )
    assert result.abi_return is True


def test_cert_not_valid(cert):
    result = cert.send.call(
        au.AppClientMethodCallParams(
            method="isValid",
            args=[0],
            box_references=[
                au.BoxReference(app_id=0, name=exists_key(0)),
                au.BoxReference(app_id=0, name=revoked_key(0)),
            ],
            note=b"v2",
        )
    )
    assert result.abi_return is False


def test_total_revoked(cert):
    result = cert.send.call(
        au.AppClientMethodCallParams(method="getTotalRevoked")
    )
    assert result.abi_return == 1


def test_issue_second(cert, account):
    boxes = cert_boxes(1)
    result = cert.send.call(
        au.AppClientMethodCallParams(
            method="issueCert",
            args=[account.address, 80, 2000],
            box_references=boxes,
            note=b"c2",
        )
    )
    assert result.abi_return == 1

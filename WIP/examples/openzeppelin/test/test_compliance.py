"""
Compliance behavioral tests.
Tests rule creation, compliance checks, and violation tracking.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def severity_key(rid):
    return mapping_box_key("_ruleSeverity", rid.to_bytes(64, "big"))
def rule_active_key(rid):
    return mapping_box_key("_ruleActive", rid.to_bytes(64, "big"))
def rule_violations_key(rid):
    return mapping_box_key("_ruleViolationCount", rid.to_bytes(64, "big"))
def entity_violations_key(addr):
    return mapping_box_key("_entityViolations", encoding.decode_address(addr))
def entity_checks_key(addr):
    return mapping_box_key("_entityChecks", encoding.decode_address(addr))

def rule_boxes(rid):
    return [
        au.BoxReference(app_id=0, name=severity_key(rid)),
        au.BoxReference(app_id=0, name=rule_active_key(rid)),
        au.BoxReference(app_id=0, name=rule_violations_key(rid)),
    ]

@pytest.fixture(scope="module")
def comp(localnet, account):
    return deploy_contract(localnet, account, "ComplianceTest")

def test_deploy(comp):
    assert comp.app_id > 0

def test_admin(comp, account):
    r = comp.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_create_rule(comp):
    boxes = rule_boxes(0)
    r = comp.send.call(au.AppClientMethodCallParams(
        method="createRule", args=[2], box_references=boxes))  # critical
    assert r.abi_return == 0

def test_rule_count(comp):
    r = comp.send.call(au.AppClientMethodCallParams(method="getRuleCount"))
    assert r.abi_return == 1

def test_rule_severity(comp):
    r = comp.send.call(au.AppClientMethodCallParams(
        method="getRuleSeverity", args=[0],
        box_references=[au.BoxReference(app_id=0, name=severity_key(0))]))
    assert r.abi_return == 2

def test_rule_active(comp):
    r = comp.send.call(au.AppClientMethodCallParams(
        method="isRuleActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=rule_active_key(0))]))
    assert r.abi_return is True

def test_init_entity_and_check_pass(comp, account):
    comp.send.call(au.AppClientMethodCallParams(
        method="initEntity", args=[account.address],
        box_references=[
            au.BoxReference(app_id=0, name=entity_violations_key(account.address)),
            au.BoxReference(app_id=0, name=entity_checks_key(account.address)),
        ]))
    comp.send.call(au.AppClientMethodCallParams(
        method="performCheck", args=[account.address, 0, True],
        box_references=[
            au.BoxReference(app_id=0, name=rule_active_key(0)),
            au.BoxReference(app_id=0, name=entity_checks_key(account.address)),
            au.BoxReference(app_id=0, name=entity_violations_key(account.address)),
            au.BoxReference(app_id=0, name=rule_violations_key(0)),
        ]))

def test_entity_compliant(comp, account):
    r = comp.send.call(au.AppClientMethodCallParams(
        method="isEntityCompliant", args=[account.address],
        box_references=[au.BoxReference(app_id=0, name=entity_violations_key(account.address))]))
    assert r.abi_return is True

def test_entity_checks(comp, account):
    r = comp.send.call(au.AppClientMethodCallParams(
        method="getEntityChecks", args=[account.address],
        box_references=[au.BoxReference(app_id=0, name=entity_checks_key(account.address))]))
    assert r.abi_return == 1

def test_check_fail(comp, account):
    comp.send.call(au.AppClientMethodCallParams(
        method="performCheck", args=[account.address, 0, False],
        box_references=[
            au.BoxReference(app_id=0, name=rule_active_key(0)),
            au.BoxReference(app_id=0, name=entity_checks_key(account.address)),
            au.BoxReference(app_id=0, name=entity_violations_key(account.address)),
            au.BoxReference(app_id=0, name=rule_violations_key(0)),
        ], note=b"c2"))

def test_entity_not_compliant(comp, account):
    r = comp.send.call(au.AppClientMethodCallParams(
        method="isEntityCompliant", args=[account.address],
        box_references=[au.BoxReference(app_id=0, name=entity_violations_key(account.address))],
        note=b"ec2"))
    assert r.abi_return is False

def test_entity_violations(comp, account):
    r = comp.send.call(au.AppClientMethodCallParams(
        method="getEntityViolations", args=[account.address],
        box_references=[au.BoxReference(app_id=0, name=entity_violations_key(account.address))]))
    assert r.abi_return == 1

def test_total_violations(comp):
    r = comp.send.call(au.AppClientMethodCallParams(method="getTotalViolations"))
    assert r.abi_return == 1

def test_check_count(comp):
    r = comp.send.call(au.AppClientMethodCallParams(method="getCheckCount"))
    assert r.abi_return == 2

def test_deactivate_rule(comp):
    comp.send.call(au.AppClientMethodCallParams(
        method="deactivateRule", args=[0],
        box_references=[au.BoxReference(app_id=0, name=rule_active_key(0))]))
    r = comp.send.call(au.AppClientMethodCallParams(
        method="isRuleActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=rule_active_key(0))], note=b"ra2"))
    assert r.abi_return is False

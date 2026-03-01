"""
SLA behavioral tests.
Tests SLA creation, report filing, breach detection, and activation.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key


def target_key(sid):
    return mapping_box_key("_slaTarget", sid.to_bytes(64, "big"))

def report_count_key(sid):
    return mapping_box_key("_slaReportCount", sid.to_bytes(64, "big"))

def breach_count_key(sid):
    return mapping_box_key("_slaBreachCount", sid.to_bytes(64, "big"))

def active_key(sid):
    return mapping_box_key("_slaActive", sid.to_bytes(64, "big"))

def uptime_key(sid):
    return mapping_box_key("_slaLastUptime", sid.to_bytes(64, "big"))

def sla_boxes(sid):
    return [
        au.BoxReference(app_id=0, name=target_key(sid)),
        au.BoxReference(app_id=0, name=report_count_key(sid)),
        au.BoxReference(app_id=0, name=breach_count_key(sid)),
        au.BoxReference(app_id=0, name=active_key(sid)),
        au.BoxReference(app_id=0, name=uptime_key(sid)),
    ]


@pytest.fixture(scope="module")
def sla(localnet, account):
    return deploy_contract(localnet, account, "SLATest")


def test_deploy(sla):
    assert sla.app_id > 0

def test_admin(sla, account):
    r = sla.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert r.abi_return == account.address

def test_create_sla(sla):
    boxes = sla_boxes(0)
    r = sla.send.call(au.AppClientMethodCallParams(
        method="createSLA", args=[9900], box_references=boxes))
    assert r.abi_return == 0

def test_sla_count(sla):
    r = sla.send.call(au.AppClientMethodCallParams(method="getSlaCount"))
    assert r.abi_return == 1

def test_sla_target(sla):
    r = sla.send.call(au.AppClientMethodCallParams(
        method="getSlaTarget", args=[0],
        box_references=[au.BoxReference(app_id=0, name=target_key(0))]))
    assert r.abi_return == 9900

def test_sla_active(sla):
    r = sla.send.call(au.AppClientMethodCallParams(
        method="isSlaActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))
    assert r.abi_return is True

def test_file_good_report(sla):
    sla.send.call(au.AppClientMethodCallParams(
        method="fileReport", args=[0, 9950],  # above target
        box_references=sla_boxes(0)))

def test_no_breach_after_good(sla):
    r = sla.send.call(au.AppClientMethodCallParams(
        method="isSLABreached", args=[0],
        box_references=[au.BoxReference(app_id=0, name=breach_count_key(0))]))
    assert r.abi_return is False

def test_last_uptime(sla):
    r = sla.send.call(au.AppClientMethodCallParams(
        method="getSlaLastUptime", args=[0],
        box_references=[au.BoxReference(app_id=0, name=uptime_key(0))]))
    assert r.abi_return == 9950

def test_report_count(sla):
    r = sla.send.call(au.AppClientMethodCallParams(
        method="getSlaReportCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=report_count_key(0))]))
    assert r.abi_return == 1

def test_file_bad_report(sla):
    sla.send.call(au.AppClientMethodCallParams(
        method="fileReport", args=[0, 9800],  # below target
        box_references=sla_boxes(0), note=b"r2"))

def test_breached_after_bad(sla):
    r = sla.send.call(au.AppClientMethodCallParams(
        method="isSLABreached", args=[0],
        box_references=[au.BoxReference(app_id=0, name=breach_count_key(0))],
        note=b"b2"))
    assert r.abi_return is True

def test_breach_count(sla):
    r = sla.send.call(au.AppClientMethodCallParams(
        method="getSlaBreachCount", args=[0],
        box_references=[au.BoxReference(app_id=0, name=breach_count_key(0))]))
    assert r.abi_return == 1

def test_total_breaches(sla):
    r = sla.send.call(au.AppClientMethodCallParams(method="getTotalBreaches"))
    assert r.abi_return == 1

def test_total_reports(sla):
    r = sla.send.call(au.AppClientMethodCallParams(method="getReportCount"))
    assert r.abi_return == 2

def test_deactivate(sla):
    sla.send.call(au.AppClientMethodCallParams(
        method="deactivateSLA", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))
    r = sla.send.call(au.AppClientMethodCallParams(
        method="isSlaActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        note=b"a2"))
    assert r.abi_return is False

def test_activate(sla):
    sla.send.call(au.AppClientMethodCallParams(
        method="activateSLA", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))]))
    r = sla.send.call(au.AppClientMethodCallParams(
        method="isSlaActive", args=[0],
        box_references=[au.BoxReference(app_id=0, name=active_key(0))],
        note=b"a3"))
    assert r.abi_return is True

"""Tests for the externalSource category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_multiple_equals_signs(harness):
    """externalSource/contracts/multiple_equals_signs.sol"""
    app = harness.compile_and_deploy("externalSource/contracts/multiple_equals_signs.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_multiple_external_source(harness):
    """externalSource/contracts/multiple_external_source.sol"""
    app = harness.compile_and_deploy("externalSource/contracts/multiple_external_source.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_multisource(harness):
    """externalSource/contracts/multisource.sol"""
    app = harness.compile_and_deploy("externalSource/contracts/multisource.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_non_normalized_paths(harness):
    """externalSource/contracts/non_normalized_paths.sol"""
    app = harness.compile_and_deploy("externalSource/contracts/non_normalized_paths.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_relative_imports(harness):
    """externalSource/contracts/relative_imports.sol"""
    app = harness.compile_and_deploy("externalSource/contracts/relative_imports.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_source(harness):
    """externalSource/contracts/source.sol"""
    app = harness.compile_and_deploy("externalSource/contracts/source.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_source_import(harness):
    """externalSource/contracts/source_import.sol"""
    app = harness.compile_and_deploy("externalSource/contracts/source_import.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_source_import_subdir(harness):
    """externalSource/contracts/source_import_subdir.sol"""
    app = harness.compile_and_deploy("externalSource/contracts/source_import_subdir.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_source_name_starting_with_dots(harness):
    """externalSource/contracts/source_name_starting_with_dots.sol"""
    app = harness.compile_and_deploy("externalSource/contracts/source_name_starting_with_dots.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_source_remapping(harness):
    """externalSource/contracts/source_remapping.sol"""
    pytest.fail("ExternalSource path-remap with leading-slash path collision. Multisource splitter doesn't handle remap `/foo=...`.")

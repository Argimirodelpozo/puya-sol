"""Auto-generated tests for the externalSource category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_multiple_equals_signs(harness):
    """externalSource/multiple_equals_signs.sol"""
    app = harness.compile_and_deploy("externalSource/multiple_equals_signs.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_multiple_external_source(harness):
    """externalSource/multiple_external_source.sol"""
    app = harness.compile_and_deploy("externalSource/multiple_external_source.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_multisource(harness):
    """externalSource/multisource.sol"""
    app = harness.compile_and_deploy("externalSource/multisource.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_non_normalized_paths(harness):
    """externalSource/non_normalized_paths.sol"""
    app = harness.compile_and_deploy("externalSource/non_normalized_paths.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_relative_imports(harness):
    """externalSource/relative_imports.sol"""
    app = harness.compile_and_deploy("externalSource/relative_imports.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_source(harness):
    """externalSource/source.sol"""
    app = harness.compile_and_deploy("externalSource/source.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_source_import(harness):
    """externalSource/source_import.sol"""
    app = harness.compile_and_deploy("externalSource/source_import.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_source_import_subdir(harness):
    """externalSource/source_import_subdir.sol"""
    app = harness.compile_and_deploy("externalSource/source_import_subdir.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_source_name_starting_with_dots(harness):
    """externalSource/source_name_starting_with_dots.sol"""
    app = harness.compile_and_deploy("externalSource/source_name_starting_with_dots.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_source_remapping(harness):
    """externalSource/source_remapping.sol"""
    app = harness.compile_and_deploy("externalSource/source_remapping.sol")
    # constructor-only test — deployment succeeding is the assertion

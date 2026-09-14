"""The EVM oracle must use verified solc facts before attempting deployment."""
from chd_common import replay_compiler_settings, verified_compiler_settings


def test_preserve_verified_ir_optimizer_details_and_target():
    source = {"compiler_settings": {
        "viaIR": True, "evmVersion": "cancun",
        "optimizer": {"enabled": True, "runs": 999999, "details": {"yul": True}},
        "metadata": {"bytecodeHash": "none"}, "libraries": {"L.sol": {"L": "0x12"}},
    }}
    case = {"solc_settings": verified_compiler_settings(source),
            "multifile": {"remappings": ["@lib/=lib/"]}}
    settings = replay_compiler_settings(case, ["abi"])
    assert settings["viaIR"] is True and settings["evmVersion"] == "cancun"
    assert settings["optimizer"] == source["compiler_settings"]["optimizer"]
    assert settings["metadata"] == {"bytecodeHash": "none"}
    assert settings["remappings"] == ["@lib/=lib/"]
    assert settings["outputSelection"] == {"*": {"*": ["abi"]}}
    assert "libraries" not in settings  # relocated through solc link references
    settings["optimizer"]["runs"] = 1
    assert source["compiler_settings"]["optimizer"]["runs"] == 999999


def test_blockscout_top_level_codegen_facts_are_not_lost():
    settings = verified_compiler_settings({
        "optimization_enabled": False, "optimization_runs": 0,
        "evm_version": "istanbul"})
    assert settings == {"optimizer": {"enabled": False, "runs": 0},
                        "evmVersion": "istanbul"}


def test_explicit_settings_take_precedence_over_summary_fields():
    settings = verified_compiler_settings({
        "compiler_settings": {"viaIR": False, "optimizer": {"enabled": False}},
        "optimization_enabled": True, "optimization_runs": 200})
    assert settings == {"viaIR": False, "optimizer": {"enabled": False}}


def test_missing_facts_use_solc_defaults_without_forcing_paris_or_ir():
    for case in ({}, {"solc_settings": {"viaIR": None, "optimizer": None,
                                        "evmVersion": "default"}}):
        assert replay_compiler_settings(case, ["abi"]) == {
            "outputSelection": {"*": {"*": ["abi"]}}}

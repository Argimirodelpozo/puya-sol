#include "json/OptionsWriter.h"

#include <nlohmann/json.hpp>

namespace puyasol::json
{

using njson = nlohmann::json;

static void addTemplateVarDefs(
	njson& opts,
	std::set<std::string> const& _children,
	std::map<std::string, int64_t> const& _intVars = {})
{
	if (_children.empty() && _intVars.empty()) return;
	auto& defs = opts["cli_template_definitions"];
	for (auto const& child : _children)
	{
		// Declare each template var as bytes type with a stub default.
		// The actual values are substituted at deployment time from
		// the .tmpl file, but puya needs the declarations to compile.
		// Keys WITHOUT the TMPL_ prefix — puya adds it from template_vars_prefix
		// Approval is split into two ≤4096-byte pages (ApprovalProgramPages);
		// see SolNewExpression. Page 1 is empty for small children.
		defs["APPROVAL_" + child + "_P0"] = "0x068101"; // stub: #pragma version 6; int 1
		defs["APPROVAL_" + child + "_P1"] = "0x068101";
		defs["CLEAR_" + child] = "0x068101";
	}
	// Integer template vars (e.g. UROS_ORCH_APP_ID for the splitter):
	// declared with a placeholder 0; deploy-time substitution writes the
	// real value into the bytecode.
	for (auto const& [name, value] : _intVars)
		defs[name] = value;
}

bool OptionsWriter::write(
	boost::filesystem::path const& _path,
	std::vector<std::string> const& _contractNames,
	std::string const& _outputDir,
	int _optimizationLevel,
	bool _outputIr,
	std::set<std::string> const& _templateVarChildren,
	std::map<std::string, int64_t> const& _intTemplateVars,
	artifact::Digest& _digest,
	std::string& _error
)
{
	if (_contractNames.empty())
	{
		_error = "options compilation set cannot be empty";
		return false;
	}
	njson opts;
	opts["compilation_set"] = njson::object();
	for (auto const& name: _contractNames)
	{
		if (name.empty())
		{
			_error = "options compilation target name cannot be empty";
			return false;
		}
		// Explicit/import aliases can repeat an AWST id; one compilation entry
		// is the backend's canonical representation.
		opts["compilation_set"][name] = _outputDir;
	}
	opts["output_teal"] = true;
	opts["output_source_map"] = false;
	opts["output_arc32"] = false;
	opts["output_arc56"] = true;
	opts["output_bytecode"] = true;
	opts["debug_level"] = 1;
	opts["optimization_level"] = _optimizationLevel;
	opts["target_avm_version"] = 12;
	opts["template_vars_prefix"] = "TMPL_";
	opts["cli_template_definitions"] = njson::object();
	addTemplateVarDefs(opts, _templateVarChildren, _intTemplateVars);
	if (_outputIr)
	{
		opts["output_ssa_ir"] = true;
		opts["output_optimization_ir"] = true;
		opts["output_destructured_ir"] = true;
		opts["output_memory_ir"] = true;
	}

	if (!opts["compilation_set"].is_object()
		|| !opts["cli_template_definitions"].is_object())
	{
		_error = "options JSON failed schema validation";
		return false;
	}
	return artifact::writeJsonAtomically(
		_path, opts.dump(2) + '\n', _digest, _error);
}

} // namespace puyasol::json

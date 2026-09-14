#include "json/OptionsWriter.h"
#include "builder/contract/ChildDeployment.h"

#include <nlohmann/json.hpp>

namespace puyasol::json
{

using njson = nlohmann::json;

static void addTemplateVarDefs(
	njson& opts,
	std::set<std::string> const& _children)
{
	if (_children.empty()) return;
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
		for (auto const& field: builder::childSchemaFields)
			defs["CHILD_" + child + "_" + field.transactionField] = 0;
	}
}

bool OptionsWriter::write(
	boost::filesystem::path const& _path,
	std::vector<std::string> const& _contractNames,
	std::string const& _outputDir,
	int _optimizationLevel,
	bool _outputIr,
	std::set<std::string> const& _templateVarChildren,
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
		if (opts["compilation_set"].contains(name))
		{
			_error = "duplicate compilation target: " + name;
			return false;
		}
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
	addTemplateVarDefs(opts, _templateVarChildren);
	if (_outputIr)
	{
		opts["output_ssa_ir"] = true;
		opts["output_optimization_ir"] = true;
		opts["output_destructured_ir"] = true;
		opts["output_memory_ir"] = true;
	}

	return artifact::writeJsonAtomically(
		_path, opts.dump(2) + '\n', _digest, _error);
}

} // namespace puyasol::json

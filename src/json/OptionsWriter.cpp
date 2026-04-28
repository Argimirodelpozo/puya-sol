#include "json/OptionsWriter.h"
#include "Logger.h"

#include <fstream>

namespace puyasol::json
{

using njson = nlohmann::json;

static void addTemplateVarDefs(njson& opts, std::set<std::string> const& _children)
{
	if (_children.empty()) return;
	auto& defs = opts["cli_template_definitions"];
	for (auto const& child : _children)
	{
		// Declare each template var as bytes type with a stub default.
		// The actual values are substituted at deployment time from
		// the .tmpl file, but puya needs the declarations to compile.
		// Keys WITHOUT the TMPL_ prefix — puya adds it from template_vars_prefix
		defs["APPROVAL_" + child] = "0x068101"; // stub: #pragma version 6; int 1
		defs["CLEAR_" + child] = "0x068101";
		// uint64 template var for the deployed helper's app id. Substituted
		// at deploy time once the helper is on chain.
		defs[child + "_APP_ID"] = 0;
	}
}

void OptionsWriter::write(
	std::string const& _path,
	std::string const& _contractName,
	std::string const& _outputDir,
	int _optimizationLevel,
	bool _outputIr,
	std::set<std::string> const& _templateVarChildren
)
{
	njson opts;
	opts["compilation_set"] = njson::object();
	opts["compilation_set"][_contractName] = _outputDir;
	opts["output_teal"] = true;
	opts["output_source_map"] = false;
	opts["output_arc32"] = false;
	opts["output_arc56"] = true;
	opts["output_bytecode"] = true;
	opts["debug_level"] = 1;
	opts["optimization_level"] = _optimizationLevel;
	opts["target_avm_version"] = 10;
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

	std::ofstream out(_path);
	if (!out.is_open())
	{
		Logger::instance().error("Cannot write options file: " + _path);
		return;
	}
	out << opts.dump(2) << std::endl;
}

void OptionsWriter::writeMultiple(
	std::string const& _path,
	std::vector<std::string> const& _contractNames,
	std::string const& _outputDir,
	int _optimizationLevel,
	bool _outputIr,
	std::set<std::string> const& _templateVarChildren
)
{
	njson opts;
	opts["compilation_set"] = njson::object();
	for (auto const& name: _contractNames)
		opts["compilation_set"][name] = _outputDir;
	opts["output_teal"] = true;
	opts["output_source_map"] = false;
	opts["output_arc32"] = false;
	opts["output_arc56"] = true;
	opts["output_bytecode"] = true;
	opts["debug_level"] = 1;
	opts["optimization_level"] = _optimizationLevel;
	opts["target_avm_version"] = 10;
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

	std::ofstream out(_path);
	if (!out.is_open())
	{
		Logger::instance().error("Cannot write options file: " + _path);
		return;
	}
	out << opts.dump(2) << std::endl;
}

} // namespace puyasol::json

#include "json/OptionsWriter.h"
#include "Logger.h"

#include <fstream>

namespace puyasol::json
{

using njson = nlohmann::json;

void OptionsWriter::write(
	std::string const& _path,
	std::string const& _contractName,
	std::string const& _outputDir
)
{
	njson opts;
	opts["compilation_set"] = njson::object();
	opts["compilation_set"][_contractName] = _outputDir;
	opts["output_teal"] = true;
	opts["output_source_map"] = false;
	opts["output_arc32"] = false;
	opts["output_arc56"] = true;
	opts["output_bytecode"] = false;
	opts["debug_level"] = 1;
	opts["optimization_level"] = 1;
	opts["target_avm_version"] = 10;
	opts["template_vars_prefix"] = "TMPL_";
	opts["cli_template_definitions"] = njson::object();

	std::ofstream out(_path);
	if (!out.is_open())
	{
		Logger::instance().error("Cannot write options file: " + _path);
		return;
	}
	out << opts.dump(2) << std::endl;
}

} // namespace puyasol::json

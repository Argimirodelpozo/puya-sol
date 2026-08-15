#pragma once

#include <libsolidity/ast/ASTForward.h>
#include <libyul/ASTForward.h>

#include <map>
#include <set>
#include <string>

namespace solidity::yul
{
class Block;
class Dialect;
class FunctionDefinition;
}

namespace puyasol::builder
{

/// Stable boundary around semantic facts supplied by the vendored solc.
///
/// Builder code should depend on these small, project-owned views instead of
/// reproducing solc AST walks or spreading optimiser-internal APIs throughout
/// the lowering pipeline. When the vendored solc changes, this is the one
/// integration point that should need adjustment.
class SolcFacts
{
public:
	struct YulAnalysis
	{
		std::map<std::string, solidity::yul::FunctionDefinition const*> functions;
		std::set<std::string> reachableFunctions;
		std::set<std::string> recursiveFunctions;
		std::set<std::string> assignedVariables;
		bool usesStorage = false;
	};

	/// Analyze a disambiguated inline-assembly block with solc's own Yul
	/// collectors, call graph, recursion detector, and side-effect propagator.
	static YulAnalysis analyzeYul(
		solidity::yul::Block const& _block,
		solidity::yul::Dialect const& _dialect);

	/// True when the inline assembly either references a Solidity `.slot` or
	/// reachable Yul code reads/writes EVM storage.
	static bool usesStorage(
		solidity::frontend::InlineAssembly const& _assembly);
};

} // namespace puyasol::builder

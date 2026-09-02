#pragma once

#include <libsolidity/ast/ASTForward.h>
#include <libyul/ASTForward.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace solidity::frontend
{
class FunctionType;
}

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
		/// Single-assignment locals whose defining expression is a NUMBER
		/// literal, as a full-width decimal string (solc's SSAValueTracker:
		/// a reassignment anywhere drops the entry).
		std::map<std::string, std::string> constantValues;
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

	/// Solidity's canonical four-byte function/error selector. The FunctionType
	/// overload delegates to solc's externalIdentifier(); the signature overload
	/// is for language constructs such as abi.encodeWithSignature.
	static std::vector<uint8_t> externalSelector(
		solidity::frontend::FunctionType const& _function);
	static std::vector<uint8_t> externalSelector(std::string const& _signature);

	/// Full keccak256 signature hash used by Solidity event selectors.
	static std::vector<uint8_t> signatureHash(std::string const& _signature);

	/// solc's EIP-165 interface ID for the exact interface declaration.
	static std::vector<uint8_t> interfaceId(
		solidity::frontend::ContractDefinition const& _contract);
};

} // namespace puyasol::builder

#pragma once

#include <libsolidity/ast/ASTForward.h>
#include <libyul/ASTForward.h>

#include <cstdint>
#include <map>
#include <memory>
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

struct PreparedAssembly;

/// Stable boundary around semantic facts supplied by the vendored solc.
///
/// Builder code should depend on these small, project-owned views instead of
/// reproducing solc AST walks or spreading optimiser-internal APIs throughout
/// the lowering pipeline. When the vendored solc changes, this is the one
/// integration point that should need adjustment.
class SolcFacts
{
public:
	/// Strip call options and parenthesized singleton expressions.
	static solidity::frontend::Expression const& functionExpression(
		solidity::frontend::Expression const& _expression);

	/// Concrete function denoted by a call/reference expression. Uses solc's
	/// requiredLookup and resolveVirtual; super's lexical owner comes from its
	/// annotated ContractType, not the function currently being translated.
	static solidity::frontend::FunctionDefinition const* resolveFunction(
		solidity::frontend::Expression const& _expression,
		solidity::frontend::ContractDefinition const* _mostDerived);

	/// Same lookup, restricted to reference-preserving calls (including the
	/// library delegate calls this backend internalizes). Null for dynamic
	/// function pointers and ordinary external ABI calls.
	static solidity::frontend::FunctionDefinition const* resolveInternalCall(
		solidity::frontend::FunctionCall const& _call,
		solidity::frontend::ContractDefinition const* _mostDerived);

	/// Match solc IRGenerator::generateModifier: declaration identity plus
	/// requiredLookup, followed by solc's own virtual override resolution.
	/// A constructor-base invocation is not a modifier and returns nullptr.
	static solidity::frontend::ModifierDefinition const* resolveModifier(
		solidity::frontend::ModifierInvocation const& _invocation,
		solidity::frontend::ContractDefinition const* _mostDerived);

	struct YulAnalysis
	{
		std::map<std::string, solidity::yul::FunctionDefinition const*> functions;
		std::set<std::string> reachableFunctions;
		std::set<std::string> recursiveFunctions;
		/// Transitive requirements of reachable functions, from solc's call
		/// graph/dialect effects. Calldata needs an explicit hidden argument;
		/// successful EVM termination still needs the enclosing return frame.
		std::set<std::string> calldataFunctions;
		std::set<std::string> terminatingFunctions;
		std::set<std::string> memoryWritingFunctions;
		std::set<std::string> assignedVariables;
		/// Single-assignment locals whose defining expression is a NUMBER
		/// literal, as a full-width decimal string (solc's SSAValueTracker:
		/// a reassignment anywhere drops the entry).
		std::map<std::string, std::string> constantValues;
		bool usesStorage = false;
	};

	/// Disambiguate once using solc's lexical scopes, remap external references,
	/// then run the Yul analyses against the exact tree lowering will consume.
	static std::shared_ptr<PreparedAssembly const> prepareAssembly(
		solidity::frontend::InlineAssembly const& _assembly);

	struct YulArgumentFacts
	{
		std::map<std::string, std::string> constants;
		std::map<std::string, unsigned> residuesMod32;
	};
	/// Facts true at EVERY call to an unmodified Yul parameter. solc owns
	/// scope, reachability, SSA definitions and constant/offset reasoning;
	/// residues are target-side arithmetic over those immutable definitions.
	static YulArgumentFacts yulArgumentFacts(
		PreparedAssembly const& _assembly,
		std::map<std::string, std::string> const& _externalConstants);

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

private:
	static YulAnalysis analyzeYul(
		solidity::yul::Block const& _block,
		solidity::yul::Dialect const& _dialect);
};

} // namespace puyasol::builder

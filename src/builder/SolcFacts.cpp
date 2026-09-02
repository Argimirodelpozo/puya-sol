#include "builder/SolcFacts.h"

#include <libsolidity/ast/AST.h>
#include <libsolutil/CommonData.h>
#include <libsolutil/Exceptions.h>
#include <libsolutil/Keccak256.h>
#include <libsolutil/Numeric.h>
#include <libyul/AST.h>
#include <libyul/Dialect.h>
#include <libyul/SideEffects.h>
#include <libyul/optimiser/CallGraphGenerator.h>
#include <libyul/optimiser/NameCollector.h>
#include <libyul/optimiser/Semantics.h>
#include <libyul/optimiser/SSAValueTracker.h>

#include <variant>
#include <vector>

namespace puyasol::builder
{

using namespace solidity::yul;

namespace
{

std::string nameString(YulName const& _name)
{
	return _name.str();
}

YulName const* userFunctionName(FunctionHandle const& _handle)
{
	return std::get_if<YulName>(&_handle);
}

} // namespace

SolcFacts::YulAnalysis SolcFacts::analyzeYul(
	Block const& _block,
	Dialect const& _dialect)
{
	YulAnalysis result;
	auto definitions = allFunctionDefinitions(_block);
	for (auto const& [name, definition]: definitions)
		result.functions.emplace(nameString(name), definition);

	for (auto const& name: assignedVariableNames(_block))
		result.assignedVariables.insert(nameString(name));

	// SSAValueTracker asserts each name is declared once ("Source needs to be
	// disambiguated") — sibling scopes reusing a `let` name are ordinary in
	// hand-written assembly. The values are an optimisation input, so an
	// ambiguous block yields none rather than failing the compile.
	try
	{
		SSAValueTracker ssaValues;
		ssaValues(_block);
		for (auto const& [name, value]: ssaValues.values())
		{
			if (!value)
				continue;
			auto const* literal = std::get_if<Literal>(value);
			if (!literal || literal->kind != LiteralKind::Number)
				continue;
			result.constantValues.emplace(
				nameString(name), literal->value.value().str());
		}
	}
	catch (solidity::util::Exception const&)
	{
		result.constantValues.clear();
	}

	auto graph = CallGraphGenerator::callGraph(_block);
	std::set<YulName> recursive;
	for (auto const& handle: graph.recursiveFunctions())
		if (auto const* name = userFunctionName(handle);
			name && definitions.count(*name))
			recursive.insert(*name);

	// Function definitions are declarations, not roots. Traverse only from the
	// empty-name outer context and retain user functions present in this block.
	std::set<YulName> reachable;
	std::vector<YulName> work;
	auto addCallees = [&](YulName const& _caller) {
		auto const found = graph.functionCalls.find(FunctionHandle{_caller});
		if (found == graph.functionCalls.end())
			return;
		for (auto const& callee: found->second)
			if (auto const* name = userFunctionName(callee);
				name && definitions.count(*name) && reachable.insert(*name).second)
				work.push_back(*name);
	};
	addCallees(YulName{});
	for (size_t i = 0; i < work.size(); ++i)
		addCallees(work[i]);
	for (auto const& name: reachable)
	{
		result.reachableFunctions.insert(nameString(name));
		if (recursive.count(name))
			result.recursiveFunctions.insert(nameString(name));
	}

	// The side-effect propagator deliberately treats an EVM `call` as capable
	// of touching storage. That is correct for optimizer reordering, but it is
	// too conservative for our question: only an explicit sload/sstore needs a
	// concrete host contract and its storage dispatcher. In particular, using
	// the propagated flag internalized Solady's SafeTransferLib (which only
	// performs external calls) while leaving its library callers as root
	// subroutines, producing unresolved cross-scope calls.
	//
	// Ask the dialect for the actual storage builtin handles and inspect only
	// the root plus reachable local Yul functions. This remains independent of
	// builtin spelling and preserves transitive Yul-function reachability.
	auto const storageLoad = _dialect.storageLoadFunctionHandle();
	auto const storageStore = _dialect.storageStoreFunctionHandle();
	auto callsStorageBuiltin = [&](FunctionHandle const& caller) {
		auto const found = graph.functionCalls.find(caller);
		if (found == graph.functionCalls.end())
			return false;
		for (auto const& callee: found->second)
			if (auto const* builtin = std::get_if<BuiltinHandle>(&callee))
				if ((storageLoad && *builtin == *storageLoad)
					|| (storageStore && *builtin == *storageStore))
					return true;
		return false;
	};
	result.usesStorage = callsStorageBuiltin(FunctionHandle{YulName{}});
	if (!result.usesStorage)
		for (auto const& name: reachable)
			if (callsStorageBuiltin(FunctionHandle{name}))
			{
				result.usesStorage = true;
				break;
			}
	return result;
}

bool SolcFacts::usesStorage(solidity::frontend::InlineAssembly const& _assembly)
{
	for (auto const& [_, reference]: _assembly.annotation().externalReferences)
		if (reference.suffix == "slot")
			return true;
	return analyzeYul(
		_assembly.operations().root(), _assembly.dialect()).usesStorage;
}

std::vector<uint8_t> SolcFacts::externalSelector(
	solidity::frontend::FunctionType const& _function)
{
	auto bytes = solidity::toBigEndian(_function.externalIdentifier());
	return {bytes.end() - 4, bytes.end()};
}

std::vector<uint8_t> SolcFacts::externalSelector(std::string const& _signature)
{
	auto hash = solidity::util::keccak256(_signature).asBytes();
	return {hash.begin(), hash.begin() + 4};
}

std::vector<uint8_t> SolcFacts::signatureHash(std::string const& _signature)
{
	return solidity::util::keccak256(_signature).asBytes();
}

std::vector<uint8_t> SolcFacts::interfaceId(
	solidity::frontend::ContractDefinition const& _contract)
{
	auto id = _contract.interfaceId();
	return {
		static_cast<uint8_t>((id >> 24) & 0xff),
		static_cast<uint8_t>((id >> 16) & 0xff),
		static_cast<uint8_t>((id >> 8) & 0xff),
		static_cast<uint8_t>(id & 0xff),
	};
}

} // namespace puyasol::builder

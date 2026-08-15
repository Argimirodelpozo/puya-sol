#include "builder/SolcFacts.h"

#include <libsolidity/ast/AST.h>
#include <libyul/AST.h>
#include <libyul/SideEffects.h>
#include <libyul/optimiser/CallGraphGenerator.h>
#include <libyul/optimiser/NameCollector.h>
#include <libyul/optimiser/Semantics.h>

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

	auto sideEffects = SideEffectsPropagator::sideEffects(_dialect, graph);
	auto const root = sideEffects.find(FunctionHandle{YulName{}});
	result.usesStorage = root != sideEffects.end()
		&& root->second.storage != SideEffects::None;
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

} // namespace puyasol::builder

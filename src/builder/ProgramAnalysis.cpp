#include "builder/ProgramAnalysis.h"
#include "builder/SolcFacts.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>
#include <libsolidity/ast/CallGraph.h>
#include <libsolidity/ast/Types.h>
#include <libsolidity/interface/CompilerStack.h>
#include <vector>

namespace puyasol::builder
{

using namespace solidity::frontend;

namespace
{

void collectMappingValueStructs(
	Type const* _type,
	std::set<int64_t>& _out,
	std::set<Type const*>& _seen)
{
	if (!_type || !_seen.insert(_type).second)
		return;
	if (auto const* mapping = dynamic_cast<MappingType const*>(_type))
	{
		Type const* value = mapping->valueType();
		while (auto const* array = dynamic_cast<ArrayType const*>(value))
			value = array->baseType();
		if (auto const* structure = dynamic_cast<StructType const*>(value))
			_out.insert(structure->structDefinition().id());
		collectMappingValueStructs(mapping->valueType(), _out, _seen);
	}
	else if (auto const* array = dynamic_cast<ArrayType const*>(_type))
		collectMappingValueStructs(array->baseType(), _out, _seen);
	else if (auto const* structure = dynamic_cast<StructType const*>(_type))
		for (auto const& member: structure->members(nullptr))
			collectMappingValueStructs(member.type, _out, _seen);
}

template <typename Fn>
void forEachFunction(CompilerStack& _compiler, Fn&& _fn)
{
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& unit = _compiler.ast(sourceName);
		for (auto const* contract:
			ASTNode::filteredNodes<ContractDefinition>(unit.nodes()))
			for (auto const* function: contract->definedFunctions())
				_fn(function, contract);
		for (auto const* function:
			ASTNode::filteredNodes<FunctionDefinition>(unit.nodes()))
			_fn(function, static_cast<ContractDefinition const*>(nullptr));
	}
}

CallableDeclaration const* graphCallable(CallGraph::Node const& _node)
{
	auto const* callable = std::get_if<CallableDeclaration const*>(&_node);
	return callable ? *callable : nullptr;
}

FunctionDefinition const* graphFunction(CallGraph::Node const& _node)
{
	return dynamic_cast<FunctionDefinition const*>(graphCallable(_node));
}

void collectContractCallGraphFacts(
	ContractDefinition const& _contract,
	CallGraph const* _graph,
	ProgramAnalysis& _out)
{
	if (!_graph)
		return;
	_out.contractsWithReachabilityGraphs.insert(_contract.id());
	auto& reachable = _out.reachableFunctionsByContract[_contract.id()];
	auto& internallyCalled = _out.internallyCalledFunctions[_contract.id()];
	for (auto const& [caller, callees]: _graph->edges)
	{
		// solc guarantees a key for every possible caller, including leaf
		// functions, so the graph's keys are the complete reachable set.
		if (auto const* function = graphFunction(caller))
			reachable.insert(function->id());
		// Entry edges are external router/constructor entry, not callsub.
		if (auto const* special = std::get_if<CallGraph::SpecialNode>(&caller);
			special && *special == CallGraph::SpecialNode::Entry)
			continue;
		for (auto const& callee: callees)
			if (auto const* function = graphFunction(callee))
				internallyCalled.insert(function->id());
	}
}

struct FunctionReferenceScanner: ASTConstVisitor
{
	std::set<FunctionDefinition const*> references;

	void add(Declaration const* _declaration)
	{
		if (auto const* function =
				dynamic_cast<FunctionDefinition const*>(_declaration))
			references.insert(function);
	}

	bool visit(Identifier const& _identifier) override
	{
		add(_identifier.annotation().referencedDeclaration);
		return true;
	}

	bool visit(MemberAccess const& _member) override
	{
		add(_member.annotation().referencedDeclaration);
		return true;
	}

	bool visit(IdentifierPath const& _path) override
	{
		add(_path.annotation().referencedDeclaration);
		return true;
	}
};

/// Whole-unit roots for early free/library-function pruning. solc's contract
/// graph may stop at a library entrypoint, so close it over AST-resolved
/// references before translation.
void collectReachableFunctions(CompilerStack& _compiler, ProgramAnalysis& _out)
{
	std::vector<FunctionDefinition const*> pending;
	auto addGraph = [&](CallGraph const* _graph) {
		if (!_graph)
			return;
		_out.hasReachabilityGraphs = true;
		for (auto const& edge: _graph->edges)
		{
			if (auto const* callable = graphCallable(edge.first))
				_out.reachableCallableIds.insert(callable->id());
			if (auto const* function = graphFunction(edge.first);
				function && _out.reachableFunctionIds.insert(function->id()).second)
				pending.push_back(function);
		}
	};

	for (auto const& sourceName: _compiler.sourceNames())
		for (auto const* contract: ASTNode::filteredNodes<ContractDefinition>(
			_compiler.ast(sourceName).nodes()))
		{
			if (!contract || contract->isLibrary())
				continue;
			if (contract->annotation().creationCallGraph.set())
				addGraph((*contract->annotation().creationCallGraph).get());
			if (contract->annotation().deployedCallGraph.set())
				addGraph((*contract->annotation().deployedCallGraph).get());
		}

	for (size_t i = 0; i < pending.size(); ++i)
	{
		FunctionReferenceScanner scanner;
		pending[i]->accept(scanner);
		for (auto const* referenced: scanner.references)
			if (_out.reachableFunctionIds.insert(referenced->id()).second)
			{
				_out.reachableCallableIds.insert(referenced->id());
				pending.push_back(referenced);
			}
	}
}

} // namespace

ProgramAnalysis ProgramAnalysis::analyze(
	CompilerStack& _compiler,
	bool _evmStorageLayout)
{
	ProgramAnalysis result;
	collectReachableFunctions(_compiler, result);

	std::set<Type const*> seen;
	for (auto const& sourceName: _compiler.sourceNames())
		for (auto const* contract:
			ASTNode::filteredNodes<ContractDefinition>(_compiler.ast(sourceName).nodes()))
		{
			for (auto const* stateVar: contract->stateVariables())
				collectMappingValueStructs(
					stateVar->type(), result.boxKeyedStructs, seen);
			if (contract->annotation().creationCallGraph.set())
				collectContractCallGraphFacts(
					*contract, (*contract->annotation().creationCallGraph).get(),
					result);
			if (contract->annotation().deployedCallGraph.set())
				collectContractCallGraphFacts(
					*contract, (*contract->annotation().deployedCallGraph).get(),
					result);
		}

	forEachFunction(_compiler, [&](FunctionDefinition const* function,
		ContractDefinition const* contract) {
		if (!function || !contract || contract->isLibrary())
			return;
		for (auto const& param: function->parameters())
			if (param->referenceLocation() == VariableDeclaration::Location::Storage)
				if (auto const* structure = dynamic_cast<StructType const*>(param->type()))
					result.refPassedStructs.insert(structure->structDefinition().id());
	});

	struct BodyFactsWalker: ASTConstVisitor
	{
		std::set<int64_t>& reassignedMemoryLocals;
		std::set<int64_t>& callablesWithInlineAssembly;
		std::set<int64_t>& callablesWithStorageAssembly;
		std::set<int64_t>& asmSlotReferenceDeclarations;
		std::set<int64_t>& structRefOffsetParams;
		bool collectOffsets;
		int64_t callableId = 0;
		BodyFactsWalker(
			std::set<int64_t>& _reassignedMemoryLocals,
			std::set<int64_t>& _callablesWithInlineAssembly,
			std::set<int64_t>& _callablesWithStorageAssembly,
			std::set<int64_t>& _asmSlotReferenceDeclarations,
			std::set<int64_t>& _structRefOffsetParams,
			bool _collectOffsets)
			: reassignedMemoryLocals(_reassignedMemoryLocals),
			  callablesWithInlineAssembly(_callablesWithInlineAssembly),
			  callablesWithStorageAssembly(_callablesWithStorageAssembly),
			  asmSlotReferenceDeclarations(_asmSlotReferenceDeclarations),
			  structRefOffsetParams(_structRefOffsetParams),
			  collectOffsets(_collectOffsets)
		{}

		static bool isArrayElementStructRef(Expression const* _expression)
		{
			auto const* index = dynamic_cast<IndexAccess const*>(_expression);
			if (!index)
				return false;
			auto const* array = dynamic_cast<ArrayType const*>(
				index->baseExpression().annotation().type);
			return array && !array->isByteArrayOrString() && array->baseType()
				&& array->baseType()->category() == Type::Category::Struct;
		}

		bool visit(Assignment const& _assignment) override
		{
			if (auto const* identifier =
				dynamic_cast<Identifier const*>(&_assignment.leftHandSide()))
				if (auto const* declaration = dynamic_cast<VariableDeclaration const*>(
						identifier->annotation().referencedDeclaration))
					if (declaration->referenceLocation()
						== VariableDeclaration::Location::Memory)
						reassignedMemoryLocals.insert(declaration->id());
			return true;
		}

		bool visit(InlineAssembly const& _assembly) override
		{
			callablesWithInlineAssembly.insert(callableId);
			if (SolcFacts::usesStorage(_assembly))
				callablesWithStorageAssembly.insert(callableId);
			for (auto const& [_, reference]: _assembly.annotation().externalReferences)
				if (reference.suffix == "slot" && reference.declaration)
					asmSlotReferenceDeclarations.insert(reference.declaration->id());
			return false;
		}

		bool visit(FunctionCall const& _call) override
		{
			if (!collectOffsets)
				return true;
			Declaration const* declaration = nullptr;
			if (auto const* identifier =
				dynamic_cast<Identifier const*>(&_call.expression()))
				declaration = identifier->annotation().referencedDeclaration;
			else if (auto const* member =
				dynamic_cast<MemberAccess const*>(&_call.expression()))
				declaration = member->annotation().referencedDeclaration;
			auto const* function =
				dynamic_cast<FunctionDefinition const*>(declaration);
			if (!function)
				return true;

			auto arguments = _call.sortedArguments();
			auto const& params = function->parameters();
			size_t shift = params.size() == arguments.size() ? 0 : 1;
			if (params.size() != arguments.size() + shift)
				return true;
			for (size_t i = 0; i < arguments.size(); ++i)
				if (isArrayElementStructRef(arguments[i].get())
					&& params[i + shift]->referenceLocation()
						== VariableDeclaration::Location::Storage)
					structRefOffsetParams.insert(params[i + shift]->id());
			return true;
		}
	} bodyFactsWalker(
		result.reassignedMemoryLocals, result.callablesWithInlineAssembly,
		result.callablesWithStorageAssembly,
		result.asmSlotReferenceDeclarations,
		result.structRefOffsetParams, !_evmStorageLayout);

	forEachFunction(_compiler, [&](FunctionDefinition const* function,
		ContractDefinition const*) {
		if (function && function->isImplemented())
		{
			bodyFactsWalker.callableId = function->id();
			function->body().accept(bodyFactsWalker);
		}
	});
	for (auto const& sourceName: _compiler.sourceNames())
		for (auto const* contract: ASTNode::filteredNodes<ContractDefinition>(
			_compiler.ast(sourceName).nodes()))
			for (auto const* modifier: contract->functionModifiers())
				if (modifier && modifier->isImplemented())
				{
					bodyFactsWalker.callableId = modifier->id();
					modifier->body().accept(bodyFactsWalker);
				}

	return result;
}

} // namespace puyasol::builder

#include "builder/ProgramAnalysis.h"
#include "builder/SolcFacts.h"
#include "builder/PreparedAssembly.h"
#include "builder/sol-ast/StorageRefPointer.h"

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

void closeOverEdges(std::set<int64_t>& ids,
	std::map<int64_t, std::set<int64_t>> const& edges)
{
	std::vector<int64_t> pending(ids.begin(), ids.end());
	for (size_t index = 0; index < pending.size(); ++index)
		if (auto found = edges.find(pending[index]); found != edges.end())
			for (auto target: found->second)
				if (ids.insert(target).second)
					pending.push_back(target);
}

IndexAccess const* indexedStorageReturn(FunctionDefinition const& function)
{
	if (!function.isImplemented() || function.returnParameters().size() != 1
		|| function.returnParameters()[0]->referenceLocation()
			!= VariableDeclaration::Location::Storage)
		return nullptr;
	struct Returns: ASTConstVisitor
	{
		VariableDeclaration const& parameter;
		std::vector<Return const*> returns;
		Expression const* assigned = nullptr;
		explicit Returns(VariableDeclaration const& p): parameter(p) {}
		bool visit(Return const& statement) override
		{
			returns.push_back(&statement);
			return true;
		}
		bool visit(Assignment const& assignment) override
		{
			if (auto const* lhs = dynamic_cast<Identifier const*>(&assignment.leftHandSide());
				lhs && lhs->annotation().referencedDeclaration == &parameter)
				assigned = &assignment.rightHandSide();
			return true;
		}
	} facts(*function.returnParameters()[0]);
	function.body().accept(facts);
	bool const named = facts.returns.empty() && !facts.parameter.name().empty();
	auto const* expression = named ? facts.assigned
		: facts.returns.size() == 1 ? facts.returns[0]->expression() : nullptr;
	auto const* access = dynamic_cast<IndexAccess const*>(expression);
	if (!access)
		return nullptr;
	bool const mapping = dynamic_cast<MappingType const*>(
		access->baseExpression().annotation().type) != nullptr;
	auto const* identifier = dynamic_cast<Identifier const*>(&access->baseExpression());
	auto const* holder = identifier ? dynamic_cast<VariableDeclaration const*>(
		identifier->annotation().referencedDeclaration) : nullptr;
	return holder && (mapping || (!named && holder->isStateVariable())) ? access : nullptr;
}

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
	if (!_contract.isLibrary())
		_out.hasReachabilityGraphs = true;
	auto& reachable = _out.reachableFunctionsByContract[_contract.id()];
	auto& internallyCalled = _out.internallyCalledFunctions[_contract.id()];
	for (auto const& [caller, callees]: _graph->edges)
	{
		// solc guarantees a key for every possible caller, including leaf
		// functions, so the graph's keys are the complete reachable set.
		if (auto const* callable = graphCallable(caller))
		{
			reachable.insert(callable->id());
			if (!_contract.isLibrary())
				_out.reachableCallableIds.insert(callable->id());
			else
				// Library calls have no host-dependent virtual resolution. Reuse
				// solc's exact edges (including overloaded operators) in the shared
				// graph; keep ordinary contract graphs context-specific.
				for (auto const& callee: callees)
					if (auto const* target = graphCallable(callee))
						_out.callableReferences[callable->id()].insert(target->id());
		}
		// Entry edges are external router/constructor entry, not callsub.
		if (auto const* special = std::get_if<CallGraph::SpecialNode>(&caller);
			special && *special == CallGraph::SpecialNode::Entry)
			continue;
		for (auto const& callee: callees)
			if (auto const* function = graphFunction(callee))
				internallyCalled.insert(function->id());
	}
}

struct CallableReferenceScanner: ASTConstVisitor
{
	std::set<int64_t> references;

	void add(Declaration const* _declaration)
	{
		if (dynamic_cast<FunctionDefinition const*>(_declaration)
			|| dynamic_cast<ModifierDefinition const*>(_declaration))
			references.insert(_declaration->id());
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

	bool visit(BinaryOperation const& _operation) override
	{
		add(*_operation.annotation().userDefinedFunction);
		return true;
	}

	bool visit(UnaryOperation const& _operation) override
	{
		add(*_operation.annotation().userDefinedFunction);
		return true;
	}
};

void indexCallable(CallableDeclaration const& _callable, ProgramAnalysis& _out)
{
	CallableReferenceScanner scanner;
	_callable.accept(scanner);
	auto& references = _out.callableReferences[_callable.id()];
	references.insert(scanner.references.begin(), scanner.references.end());
}

} // namespace

void ProgramAnalysis::closeCallableReferences(std::set<int64_t>& _ids) const
{
	closeOverEdges(_ids, callableReferences);
}

ProgramAnalysis ProgramAnalysis::analyze(
	CompilerStack& _compiler,
	bool _evmStorageLayout)
{
	ProgramAnalysis result;

	std::set<Type const*> seen;
	for (auto const& sourceName: _compiler.sourceNames())
	{
		for (auto const* contract:
			ASTNode::filteredNodes<ContractDefinition>(_compiler.ast(sourceName).nodes()))
		{
			for (auto const* modifier: contract->functionModifiers())
				indexCallable(*modifier, result);
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
	}

	forEachFunction(_compiler, [&](FunctionDefinition const* function,
		ContractDefinition const* contract) {
		if (function)
		{
			result.functionDeclarations[function->id()] = function;
			indexCallable(*function, result);
		}
		if (!function || !contract || contract->isLibrary())
			return;
		for (auto const& param: function->parameters())
			if (param->referenceLocation() == VariableDeclaration::Location::Storage)
				if (auto const* structure = dynamic_cast<StructType const*>(param->type()))
					result.refPassedStructs.insert(structure->structDefinition().id());
	});
	for (auto const& [caller, callees]: result.callableReferences)
		for (auto callee: callees)
			result.callableCallers[callee].insert(caller);
	result.closeCallableReferences(result.reachableCallableIds);
	for (auto id: result.reachableCallableIds)
		if (result.functionDeclarations.count(id))
			result.reachableFunctionIds.insert(id);
	for (auto& [_, reachable]: result.reachableFunctionsByContract)
	{
		result.closeCallableReferences(reachable);
		for (auto it = reachable.begin(); it != reachable.end();)
			if (!result.functionDeclarations.count(*it))
				it = reachable.erase(it);
			else
				++it;
	}

	struct BodyFactsWalker: ASTConstVisitor
	{
		ProgramAnalysis& analysis;
		std::set<int64_t>& reassignedMemoryLocals;
		std::set<int64_t>& callablesWithInlineAssembly;
		std::set<int64_t>& callablesWithStorageAssembly;
		std::set<int64_t>& asmSlotReferenceDeclarations;
		std::set<int64_t>& structRefOffsetParams;
		std::map<int64_t, std::set<int64_t>> offsetTransfers;
		std::map<int64_t, std::set<int64_t>> slotTransfers;
		bool collectOffsets;
		int64_t callableId = 0;
		BodyFactsWalker(
			ProgramAnalysis& _analysis,
			std::set<int64_t>& _reassignedMemoryLocals,
			std::set<int64_t>& _callablesWithInlineAssembly,
			std::set<int64_t>& _callablesWithStorageAssembly,
			std::set<int64_t>& _asmSlotReferenceDeclarations,
			std::set<int64_t>& _structRefOffsetParams,
			bool _collectOffsets)
			: analysis(_analysis), reassignedMemoryLocals(_reassignedMemoryLocals),
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

		void transferOffset(VariableDeclaration const& target, Expression const& argument)
		{
			if (!collectOffsets
				|| target.referenceLocation() != VariableDeclaration::Location::Storage
				|| !dynamic_cast<StructType const*>(target.type()))
				return;
			if (isArrayElementStructRef(&argument))
				structRefOffsetParams.insert(target.id());
			else if (auto const* identifier = dynamic_cast<Identifier const*>(&argument))
				if (auto const* declaration = identifier->annotation().referencedDeclaration)
					offsetTransfers[declaration->id()].insert(target.id());
		}

		void transferSlot(int64_t target, Expression const& expression)
		{
			if (!expression.annotation().type
				|| !expression.annotation().type->dataStoredIn(DataLocation::Storage))
				return;
			if (auto const* identifier = dynamic_cast<Identifier const*>(&expression))
			{
				if (auto const* source = identifier->annotation().referencedDeclaration)
					slotTransfers[source->id()].insert(target);
			}
			else if (auto const* call = dynamic_cast<FunctionCall const*>(&expression))
			{
				Declaration const* source = nullptr;
				if (auto const* id = dynamic_cast<Identifier const*>(&call->expression()))
					source = id->annotation().referencedDeclaration;
				else if (auto const* member = dynamic_cast<MemberAccess const*>(&call->expression()))
					source = member->annotation().referencedDeclaration;
				if (dynamic_cast<FunctionDefinition const*>(source))
					slotTransfers[source->id()].insert(target);
			}
			else if (auto const* index = dynamic_cast<IndexAccess const*>(&expression))
				transferSlot(target, index->baseExpression());
			else if (auto const* member = dynamic_cast<MemberAccess const*>(&expression))
				transferSlot(target, member->expression());
			else if (auto const* conditional = dynamic_cast<Conditional const*>(&expression))
			{
				transferSlot(target, conditional->trueExpression());
				transferSlot(target, conditional->falseExpression());
			}
		}

		bool visit(Return const& statement) override
		{
			if (statement.expression())
				transferSlot(callableId, *statement.expression());
			return true;
		}

		bool visit(VariableDeclarationStatement const& statement) override
		{
			if (statement.declarations().size() == 1 && statement.declarations()[0]
				&& statement.initialValue())
			{
				transferOffset(*statement.declarations()[0], *statement.initialValue());
				if (statement.declarations()[0]->referenceLocation()
					== VariableDeclaration::Location::Storage)
					transferSlot(statement.declarations()[0]->id(), *statement.initialValue());
			}
			return true;
		}

		bool visit(Assignment const& _assignment) override
		{
			if (auto const* identifier =
				dynamic_cast<Identifier const*>(&_assignment.leftHandSide()))
				if (auto const* declaration = dynamic_cast<VariableDeclaration const*>(
						identifier->annotation().referencedDeclaration))
				{
					if (declaration->referenceLocation()
						== VariableDeclaration::Location::Memory)
						reassignedMemoryLocals.insert(declaration->id());
					transferOffset(*declaration, _assignment.rightHandSide());
					if (declaration->referenceLocation() == VariableDeclaration::Location::Storage)
						transferSlot(declaration->id(), _assignment.rightHandSide());
				}
			return true;
		}

		bool visit(InlineAssembly const& _assembly) override
		{
			callablesWithInlineAssembly.insert(callableId);
			auto prepared = SolcFacts::prepareAssembly(_assembly);
			if (prepared->facts.usesStorage)
				callablesWithStorageAssembly.insert(callableId);
			analysis.asmAssignedSlotDeclarations.insert(
				prepared->assignedSlotDeclarations.begin(), prepared->assignedSlotDeclarations.end());
			analysis.preparedAssemblies.emplace(_assembly.id(), std::move(prepared));
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
			auto const* type = dynamic_cast<FunctionType const*>(
				_call.expression().annotation().type);
			size_t const shift = type && type->hasBoundFirstArgument() ? 1 : 0;
			if (params.size() != arguments.size() + shift)
				return true;
			if (shift)
				if (auto const* member = dynamic_cast<MemberAccess const*>(&_call.expression()))
					transferOffset(*params.front(), member->expression());
			for (size_t i = 0; i < arguments.size(); ++i)
				transferOffset(*params[i + shift], *arguments[i]);
			return true;
		}
	} bodyFactsWalker(
		result, result.reassignedMemoryLocals, result.callablesWithInlineAssembly,
		result.callablesWithStorageAssembly,
		result.asmSlotReferenceDeclarations,
		result.structRefOffsetParams, !_evmStorageLayout);

	// Body/Yul facts are invariant: collect them once, then close the finite,
	// monotone parameter-transfer graph without an arbitrary depth cutoff.
	forEachFunction(_compiler, [&](FunctionDefinition const* function,
		ContractDefinition const*) {
		if (function && function->isImplemented())
		{
			bodyFactsWalker.callableId = function->id();
			for (auto const& parameter: function->returnParameters())
				if (parameter->referenceLocation() == VariableDeclaration::Location::Storage)
					bodyFactsWalker.slotTransfers[parameter->id()].insert(function->id());
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
	closeOverEdges(result.structRefOffsetParams, bodyFactsWalker.offsetTransfers);
	auto slotSources = result.asmAssignedSlotDeclarations;
	closeOverEdges(slotSources, bodyFactsWalker.slotTransfers);
	for (auto const& [id, function]: result.functionDeclarations)
	{
		auto& facts = result.storageReferenceReturns[id];
		facts.slotHandle = slotSources.count(id) != 0;
		if (!facts.slotHandle)
			facts.indexedReturn = indexedStorageReturn(*function);
		if (facts.indexedReturn)
		{
			result.storageRefPointerReturnAccesses.insert(facts.indexedReturn->id());
			facts.bytesKeyed = dynamic_cast<MappingType const*>(
				facts.indexedReturn->baseExpression().annotation().type)
				|| function->returnParameters()[0]->type()->containsNestedMapping();
		}
	}

	return result;
}

} // namespace puyasol::builder

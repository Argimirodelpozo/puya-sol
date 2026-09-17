#include "builder/context/ProgramAnalysis.h"
#include "builder/solc/SolcFacts.h"
#include "builder/solc/PreparedAssembly.h"
#include "builder/lowering/intrinsics/AsaIntrinsics.h"
#include "builder/lowering/calls/CallResolver.h"
#include "builder/solc/StorageRefPointer.h"
#include "builder/solc/AsmScan.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>
#include <libsolidity/ast/CallGraph.h>
#include <libsolidity/ast/Types.h>
#include <libsolidity/ast/TypeProvider.h>
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
			if (auto const* lhs = SolcFacts::expressionAs<Identifier>(&assignment.leftHandSide());
				lhs && lhs->annotation().referencedDeclaration == &parameter)
				assigned = &assignment.rightHandSide();
			return true;
		}
	} facts(*function.returnParameters()[0]);
	function.body().accept(facts);
	bool const named = facts.returns.empty() && !facts.parameter.name().empty();
	auto const* expression = named ? facts.assigned
		: facts.returns.size() == 1 ? facts.returns[0]->expression() : nullptr;
	auto const* access = expression ? SolcFacts::expressionAs<IndexAccess>(expression) : nullptr;
	if (!access)
		return nullptr;
	bool const mapping = dynamic_cast<MappingType const*>(
		access->baseExpression().annotation().type) != nullptr;
	auto const* identifier = SolcFacts::expressionAs<Identifier>(&access->baseExpression());
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
	if (!_contract.isLibrary())
		_out.hasReachabilityGraphs = true;
	auto& reachable = _out.reachableCallablesByContract[_contract.id()];
	auto& internallyCalled = _out.internallyCalledFunctions[_contract.id()];
	for (auto const& [caller, callees]: _graph->edges)
	{
		// solc guarantees a key for every possible caller, including leaf
		// functions, so the graph's keys are the complete reachable set.
		if (auto const* callable = graphCallable(caller))
		{
			reachable.insert(callable->id());
			_out.reachableCallableIds.insert(callable->id());
			if (_contract.isLibrary())
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
	bool selfCall = false;

	bool visit(FunctionCall const& call) override
	{
		auto const* type = dynamic_cast<FunctionType const*>(call.expression().annotation().type);
		auto const* member = SolcFacts::expressionAs<MemberAccess>(&SolcFacts::functionExpression(call.expression()));
		if (type && member && (type->kind() == FunctionType::Kind::BareCall
			|| type->kind() == FunctionType::Kind::BareStaticCall)
			&& SolcFacts::isThis(member->expression())) selfCall = true;
		return true;
	}

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
	if (scanner.selfCall) _out.selfCallFunctions.insert(_callable.id());
	auto& references = _out.callableReferences[_callable.id()];
	references.insert(scanner.references.begin(), scanner.references.end());
}

} // namespace

void ProgramAnalysis::closeCallableReferences(std::set<int64_t>& _ids) const
{
	closeOverEdges(_ids, callableReferences);
}

namespace
{

/// Per-contract facts: modifier reference edges, mapping-valued structs, and
/// the creation/deployed call-graph reachability.
void collectContractFacts(CompilerStack& _compiler, ProgramAnalysis& _out)
{
	struct PackedAddresses: ASTConstVisitor
	{
		std::set<unsigned>& offsets;
		explicit PackedAddresses(std::set<unsigned>& value): offsets(value) {}
		static bool isAddress(Type const* type)
		{
			if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(type))
				type = &udvt->underlyingType();
			return dynamic_cast<AddressType const*>(type) || dynamic_cast<ContractType const*>(type);
		}
		bool visit(StructDefinition const& definition) override
		{
			auto const* type = TypeProvider::structType(definition, DataLocation::Storage);
			for (auto const& member: type->members(nullptr))
				if (isAddress(member.type))
					for (auto const& other: type->members(nullptr))
						if (member.name != other.name && type->storageOffsetsOfMember(member.name).first
							== type->storageOffsetsOfMember(other.name).first)
							offsets.insert(type->storageOffsetsOfMember(member.name).second);
			return true;
		}
		bool visit(ContractDefinition const& definition) override
		{
			auto const variables = TypeProvider::contract(definition)->linearizedStateVariables(DataLocation::Storage);
			for (auto const& [variable, slot, offset]: variables)
				if (isAddress(variable->type()))
					for (auto const& [other, otherSlot, _]: variables)
						if (variable != other && slot == otherSlot) offsets.insert(offset);
			return true;
		}
	} addresses(_out.packedAddressOffsets);
	std::set<Type const*> seen;
	for (auto const& sourceName: _compiler.sourceNames())
	{
		_compiler.ast(sourceName).accept(addresses);
		for (auto const* contract:
			ASTNode::filteredNodes<ContractDefinition>(_compiler.ast(sourceName).nodes()))
		{
			_out.contracts.push_back(contract);
			for (auto const* modifier: contract->functionModifiers())
				indexCallable(*modifier, _out);
			for (auto const* stateVar: contract->stateVariables())
				collectMappingValueStructs(
					stateVar->type(), _out.boxKeyedStructs, seen);
			if (contract->annotation().creationCallGraph.set())
				collectContractCallGraphFacts(
					*contract, (*contract->annotation().creationCallGraph).get(),
					_out);
			if (contract->annotation().deployedCallGraph.set())
				collectContractCallGraphFacts(
					*contract, (*contract->annotation().deployedCallGraph).get(),
					_out);
		}
	}
}

/// Effect aggregation follows solc graph edges. Only the self/public-library
/// calls that this target internalizes need extra edges into deployed bodies.
class CreationEffectScanner: public ASTConstVisitor
{
public:
	CreationEffectScanner(ContractDefinition const& contract, ProgramAnalysis const& analysis,
		CreationEffects& effects): contract(contract), analysis(analysis), effects(effects)
	{}

	void run(CallGraph const& creation)
	{
		for (auto const& [node, _]: creation.edges) pending.emplace_back(node, &creation);
		// Direct effects in these roots are not callable nodes in solc's graph.
		for (auto const* base: contract.annotation().linearizedBaseContracts)
		{
			for (auto const* variable: base->stateVariables())
				if (!variable->isConstant() && variable->value()) variable->value()->accept(*this);
			for (auto const& spec: base->baseContracts())
				if (auto const* args = spec->arguments())
					for (auto const& arg: *args) arg->accept(*this);
		}
		std::map<CallGraph const*, std::set<CallGraph::Node, CallGraph::CompareByID>> seen;
		for (size_t i = 0; i < pending.size(); ++i)
		{
			auto [node, graph] = pending[i];
			if (!seen[graph].insert(node).second) continue;
			if (auto const* callable = graphCallable(node))
			{
				if (effects.reachableCallables.insert(callable->id()).second
					&& !analysis.avmIntrinsics.count(callable->id())) callable->accept(*this);
			}
			if (auto found = graph->edges.find(node); found != graph->edges.end())
				for (auto const& callee: found->second) pending.emplace_back(callee, graph);
		}
	}

	bool visit(Identifier const& expression) override
	{
		if (auto const* variable = dynamic_cast<VariableDeclaration const*>(expression.annotation().referencedDeclaration))
		{
			if (variable->isStateVariable()) effects.stateReferences.insert(variable->id());
			if (variable->isConstant() && variable->value() && constants.insert(variable->id()).second)
				variable->value()->accept(*this);
		}
		return true;
	}
	bool visit(MemberAccess const& expression) override
	{
		if (auto const* variable = dynamic_cast<VariableDeclaration const*>(expression.annotation().referencedDeclaration);
			variable && variable->isStateVariable()) effects.stateReferences.insert(variable->id());
		if (auto const* id = SolcFacts::expressionAs<Identifier>(&expression.expression()))
			if (auto const* magic = dynamic_cast<MagicVariableDeclaration const*>(id->annotation().referencedDeclaration);
				magic && magic->name() == "msg")
				effects.messageContext |= expression.memberName() == "value"
					|| expression.memberName() == "sender" || expression.memberName() == "data";
		return true;
	}
	bool visit(NewExpression const& expression) override
	{
		effects.createsContract |= dynamic_cast<ContractType const*>(expression.typeName().annotation().type) != nullptr;
		return true;
	}
	bool visit(InlineAssembly const&) override { effects.assembly = true; return false; }
	bool visit(FunctionCall const& call) override
	{
		auto plan = eb::CallResolver::plan(call);
		if (!plan.functionType) return true;
		using K = FunctionType::Kind;
		auto kind = plan.functionType->kind();
		if (plan.declaration && analysis.avmIntrinsics.count(plan.declaration->id()))
		{
			// Pure crypto/bit operations need no post-create context. Native
			// transaction, application and asset operations conservatively do.
			effects.nativeContext |= plan.declaration->stateMutability() != StateMutability::Pure;
			return true;
		}
		if (kind == K::External && plan.isSelfCall && plan.declaration)
			adaptedCall(contract, plan.declaration->resolveVirtual(contract));
		else if (kind == K::DelegateCall && plan.declaration
			&& plan.declaration->annotation().contract && plan.declaration->annotation().contract->isLibrary())
			adaptedCall(*plan.declaration->annotation().contract, *plan.declaration);
		else if (kind == K::External || kind == K::BareCall || kind == K::BareStaticCall
			|| kind == K::BareDelegateCall || kind == K::Send || kind == K::Transfer)
			effects.externalCall = true;
		return true;
	}

private:
	ContractDefinition const& contract;
	ProgramAnalysis const& analysis;
	CreationEffects& effects;
	std::vector<std::pair<CallGraph::Node, CallGraph const*>> pending;
	std::set<int64_t> constants;
	void adaptedCall(ContractDefinition const& owner, FunctionDefinition const& function)
	{
		auto const& graph = owner.annotation().deployedCallGraph;
		solAssert(graph.set() && *graph, "missing solc graph for adapted creation call");
		// Keep graph identity on every edge: creation and deployed internal
		// dispatch have different possible pointer targets. Never union them.
		pending.emplace_back(&function, (*graph).get());
	}
};

void collectCreationEffects(ProgramAnalysis& analysis)
{
	for (auto const* contract: analysis.contracts)
		if (auto const& graph = contract->annotation().creationCallGraph; graph.set() && *graph)
		{
			auto& effects = analysis.creationEffects[contract->id()];
			CreationEffectScanner(*contract, analysis, effects).run(**graph);
		}
}

/// Index every function declaration and its reference edges; storage struct
/// params of non-library functions are ref-passed structs.
void indexFunctionDeclarations(CompilerStack& _compiler, ProgramAnalysis& _out)
{
	forEachFunction(_compiler, [&](FunctionDefinition const* function,
		ContractDefinition const* contract) {
		if (function)
		{
			_out.functionDeclarations[function->id()] = function;
			if (auto const* intrinsic = eb::AsaIntrinsics::descriptor(*function))
				_out.avmIntrinsics.emplace(function->id(), intrinsic);
			indexCallable(*function, _out);
		}
		if (!function || !contract || contract->isLibrary())
			return;
		for (auto const& param: function->parameters())
			if (param->referenceLocation() == VariableDeclaration::Location::Storage)
				if (auto const* structure = dynamic_cast<StructType const*>(param->type()))
					_out.refPassedStructs.insert(structure->structDefinition().id());
	});
}

/// Reverse the reference edges and close the reachable sets over them.
/// Per-contract sets retain modifiers too: they can require runtime support.
void closeReachability(ProgramAnalysis& _out)
{
	for (auto const& [caller, callees]: _out.callableReferences)
		for (auto callee: callees)
			_out.callableCallers[callee].insert(caller);
	_out.closeCallableReferences(_out.reachableCallableIds);
	for (auto& [_, reachable]: _out.reachableCallablesByContract)
		_out.closeCallableReferences(reachable);
}

/// Body/Yul facts of one callable at a time (`callableId`): memory-local
/// reassignment, inline assembly and its storage use, `.slot` references, and
/// the storage-ref parameter/slot transfer edges closed after the walk.
struct BodyFactsWalker: ASTConstVisitor
{
	ProgramAnalysis& analysis;
	std::set<int64_t> writtenDeclarations;
	std::map<int64_t, std::set<int64_t>> offsetTransfers;
	std::map<int64_t, std::set<int64_t>> slotTransfers;
	std::vector<std::pair<int64_t, FunctionCall const*>> calls;
	std::vector<std::pair<int64_t, FunctionType const*>> indirectCalls;
	bool collectOffsets;
	int64_t callableId = 0;
	BodyFactsWalker(ProgramAnalysis& _analysis, bool _collectOffsets)
		: analysis(_analysis), collectOffsets(_collectOffsets)
	{}

	static bool isArrayElementStructRef(Expression const* _expression)
	{
		auto const* index = SolcFacts::expressionAs<IndexAccess>(_expression);
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
		auto const& value = SolcFacts::functionExpression(argument);
		if (auto const* conditional = SolcFacts::expressionAs<Conditional>(&value))
		{
			transferOffset(target, conditional->trueExpression());
			transferOffset(target, conditional->falseExpression());
		}
		else if (isArrayElementStructRef(&value))
			analysis.structRefOffsetParams.insert(target.id());
		else if (auto const* identifier = SolcFacts::expressionAs<Identifier>(&value))
			if (auto const* declaration = identifier->annotation().referencedDeclaration)
				offsetTransfers[declaration->id()].insert(target.id());
	}

	void transferSlot(int64_t target, Expression const& source, std::optional<size_t> component = {})
	{
		auto const& expression = SolcFacts::unparenthesized(source);
		if (auto const* tuple = SolcFacts::expressionAs<TupleExpression>(&expression))
		{
			if (component && tuple->components().at(*component))
				transferSlot(target, *tuple->components()[*component]);
			return;
		}
		auto const* type = expression.annotation().type;
		if (auto const* tuple = dynamic_cast<TupleType const*>(type); tuple && component)
			type = tuple->components().at(*component);
		if (!type || !type->dataStoredIn(DataLocation::Storage))
			return;
		if (auto const* identifier = SolcFacts::expressionAs<Identifier>(&expression))
		{
			if (auto const* source = identifier->annotation().referencedDeclaration)
				slotTransfers[source->id()].insert(target);
		}
		else if (auto const* call = SolcFacts::expressionAs<FunctionCall>(&expression))
		{
			auto const* source = ASTNode::referencedDeclaration(
				SolcFacts::functionExpression(call->expression()));
			if (dynamic_cast<FunctionDefinition const*>(source))
				slotTransfers[source->id()].insert(target);
		}
		else if (auto const* index = SolcFacts::expressionAs<IndexAccess>(&expression))
			transferSlot(target, index->baseExpression());
		else if (auto const* member = SolcFacts::expressionAs<MemberAccess>(&expression))
			transferSlot(target, member->expression());
		else if (auto const* conditional = SolcFacts::expressionAs<Conditional>(&expression))
		{
			transferSlot(target, conditional->trueExpression(), component);
			transferSlot(target, conditional->falseExpression(), component);
		}
	}

	bool visit(Return const& statement) override
	{
		if (statement.expression())
		{
			auto const& returns = statement.annotation().functionReturnParameters->parameters();
			for (size_t i = 0; i < returns.size(); ++i)
				if (returns[i]->referenceLocation() == VariableDeclaration::Location::Storage)
					transferSlot(callableId, *statement.expression(), returns.size() > 1 ? std::optional<size_t>{i} : std::nullopt);
		}
		return true;
	}

	bool visit(VariableDeclarationStatement const& statement) override
	{
		if (statement.initialValue())
			for (size_t i = 0; i < statement.declarations().size(); ++i)
				if (auto const& declaration = statement.declarations()[i])
					transfer(*declaration, *statement.initialValue(),
						statement.declarations().size() > 1 ? std::optional<size_t>{i} : std::nullopt);
		if (statement.declarations().size() == 1 && statement.declarations()[0]
			&& statement.initialValue())
		{
			analysis.localInitializers.emplace(statement.declarations()[0]->id(), statement.initialValue());
			if (dynamic_cast<FunctionType const*>(statement.declarations()[0]->type())
				&& dynamic_cast<FunctionDefinition const*>(
					ASTNode::referencedDeclaration(SolcFacts::unparenthesized(*statement.initialValue()))))
				analysis.stableFunctionPointers.emplace(
					statement.declarations()[0]->id(), statement.initialValue());
		}
		return true;
	}

	bool visit(Identifier const& identifier) override
	{
		// Includes tuple assignments and delete: solc propagates lvalue facts
		// to the actual written identifiers. Writes anywhere disqualify a local,
		// including branches/loops that happen to lower after its call site.
		if (identifier.annotation().willBeWrittenTo)
			if (auto const* declaration = identifier.annotation().referencedDeclaration)
			{
				writtenDeclarations.insert(declaration->id());
				if (auto const* variable = dynamic_cast<VariableDeclaration const*>(declaration);
					variable && variable->referenceLocation() == VariableDeclaration::Location::Memory)
					analysis.reassignedMemoryLocals.insert(variable->id());
			}
		return true;
	}

	void transferReference(VariableDeclaration const& target, Expression const& value)
	{
		auto const* type = value.annotation().type;
		if (!type || type->isValueType() || !target.type()
			|| target.type()->isValueType()) return;
		for (auto const* source: SolcFacts::referenceSources(value))
			if (auto const* identifier = SolcFacts::expressionAs<Identifier>(source))
				if (auto const* variable = dynamic_cast<VariableDeclaration const*>(
					identifier->annotation().referencedDeclaration);
					variable && variable->referenceLocation() == target.referenceLocation()
					&& (target.referenceLocation() == VariableDeclaration::Location::Memory
						|| target.referenceLocation() == VariableDeclaration::Location::Storage))
					analysis.referenceAssignments[target.id()].insert(variable->id());
	}

	void transfer(VariableDeclaration const& target, Expression const& source,
		std::optional<size_t> component = {})
	{
		auto const& value = SolcFacts::unparenthesized(source);
		if (auto const* tuple = SolcFacts::expressionAs<TupleExpression>(&value); tuple && !tuple->isInlineArray())
		{
			if (component && tuple->components().at(*component))
				transfer(target, *tuple->components()[*component]);
			return;
		}
		if (auto const* conditional = SolcFacts::expressionAs<Conditional>(&value))
		{
			transfer(target, conditional->trueExpression(), component);
			transfer(target, conditional->falseExpression(), component);
			return;
		}
		if (!component)
		{
			transferReference(target, value);
			transferOffset(target, value);
		}
		if (target.referenceLocation() == VariableDeclaration::Location::Storage)
			transferSlot(target.id(), value, component);
	}

	void transferAssignment(Expression const& lhs, Expression const& rhs,
		std::optional<size_t> component = {})
	{
		if (auto const* tuple = SolcFacts::expressionAs<TupleExpression>(&lhs))
		{
			for (size_t i = 0; i < tuple->components().size(); ++i)
				if (tuple->components()[i]) transferAssignment(*tuple->components()[i], rhs, i);
		}
		else if (auto const* identifier = SolcFacts::expressionAs<Identifier>(&lhs))
			if (auto const* declaration = dynamic_cast<VariableDeclaration const*>(
					identifier->annotation().referencedDeclaration))
				transfer(*declaration, rhs, component);
		if (!lhs.annotation().type->isValueType() && lhs.annotation().type->dataStoredIn(DataLocation::Memory))
			for (auto const* root: SolcFacts::referenceSources(lhs))
				if (auto const* id = SolcFacts::expressionAs<Identifier>(root))
					if (auto const* declaration = dynamic_cast<VariableDeclaration const*>(id->annotation().referencedDeclaration))
					{
						transfer(*declaration, rhs, component);
						if (!SolcFacts::expressionAs<Identifier>(&lhs)
							&& analysis.referenceAssignments.contains(declaration->id()))
							analysis.memoryIdentityDeclarations.insert(declaration->id());
					}
	}

	bool visit(Assignment const& assignment) override
	{
		transferAssignment(assignment.leftHandSide(), assignment.rightHandSide());
		return true;
	}

	bool visit(InlineAssembly const& _assembly) override
	{
		analysis.callablesWithInlineAssembly.insert(callableId);
		auto prepared = SolcFacts::prepareAssembly(_assembly);
		if (prepared->facts.usesCalldata)
			analysis.callablesWithCalldata.insert(callableId);
		if (prepared->facts.usesStorage)
			analysis.callablesWithStorageSlotAccess.insert(callableId);
		analysis.slotHandleDeclarations.insert(
			prepared->assignedSlotDeclarations.begin(), prepared->assignedSlotDeclarations.end());
		analysis.preparedAssemblies.emplace(_assembly.id(), std::move(prepared));
		for (auto const& [_, reference]: _assembly.annotation().externalReferences)
			if (reference.declaration)
			{
				if (auto const* declaration = dynamic_cast<VariableDeclaration const*>(reference.declaration);
					declaration && declaration->referenceLocation() == VariableDeclaration::Location::CallData)
					analysis.callablesWithCalldata.insert(callableId);
				writtenDeclarations.insert(reference.declaration->id());
				if (reference.suffix == "slot")
					analysis.asmSlotReferenceDeclarations.insert(reference.declaration->id());
			}
		return false;
	}

	bool visit(FunctionCall const& _call) override
	{
		calls.emplace_back(callableId, &_call);
		if (auto const* type = dynamic_cast<FunctionType const*>(_call.expression().annotation().type);
			type && type->kind() == FunctionType::Kind::Internal
			&& !dynamic_cast<FunctionDefinition const*>(ASTNode::referencedDeclaration(
				SolcFacts::functionExpression(_call.expression()))))
			indirectCalls.emplace_back(callableId, type);
		return true;
	}

	void transferCallOffsets()
	{
		if (!collectOffsets) return;
		// The source declaration's parameter IDs are not the override's IDs.
		// Resolve each reachable body's calls in its concrete solc host, then
		// map actual arguments to the exact implementation by formal position.
		for (auto const& [caller, call]: calls)
			for (auto const* host: analysis.contracts)
				if (analysis.isCallableReachable(host->id(), caller))
					if (auto const* target = SolcFacts::resolveInternalCall(*call, host))
					{
						auto arguments = SolcFacts::callArguments(*call);
						for (size_t i = 0; i < arguments.size(); ++i)
							transferOffset(*target->parameters().at(i), *arguments[i]);
					}
	}
};

/// Walk every implemented function body, then every implemented modifier
/// body, once with `_walker`.
void collectBodyFacts(BodyFactsWalker& _walker)
{
	for (auto const& [_, function]: _walker.analysis.functionDeclarations)
		if (function->isImplemented())
		{
			_walker.callableId = function->id();
			for (auto const& parameter: function->returnParameters())
				if (parameter->referenceLocation() == VariableDeclaration::Location::Storage)
					_walker.slotTransfers[parameter->id()].insert(function->id());
			function->body().accept(_walker);
			for (auto const& modifier: function->modifiers())
				if (auto const* arguments = modifier->arguments())
					for (auto const& argument: *arguments) argument->accept(_walker);
		}
	for (auto const* contract: _walker.analysis.contracts)
		for (auto const* modifier: contract->functionModifiers())
			if (modifier && modifier->isImplemented())
			{
				_walker.callableId = modifier->id();
				modifier->body().accept(_walker);
			}
}

/// Per-function storage-reference return facts; `_slotSources` are the
/// declarations an assembly `.slot` assignment reaches.
void deriveStorageReferenceReturns(
	ProgramAnalysis& _out, std::set<int64_t> const& _slotSources)
{
	for (auto const& [id, function]: _out.functionDeclarations)
	{
		auto& facts = _out.storageReferenceReturns[id];
		facts.pointerAlias = storagePointerAliasParam(*function);
		auto const& returns = function->returnParameters();
		facts.slotHandle = _slotSources.count(id) != 0;
		if (facts.slotHandle)
			_out.callablesWithStorageSlotAccess.insert(id);
		if (!facts.slotHandle)
		{
			facts.indexedReturn = indexedStorageReturn(*function);
			facts.bytesKeyed = returns.size() == 1
				&& returns[0]->referenceLocation() == VariableDeclaration::Location::Storage
				&& containsMappingType(returns[0]->type());
		}
		if (facts.indexedReturn)
		{
			_out.storageRefPointerReturnAccesses.insert(facts.indexedReturn->id());
			facts.bytesKeyed = facts.bytesKeyed || dynamic_cast<MappingType const*>(
				facts.indexedReturn->baseExpression().annotation().type)
				|| function->returnParameters()[0]->type()->containsNestedMapping();
		}
	}
}

} // namespace

StorageReferenceReturnFacts const& ProgramAnalysis::storageReturnFacts(
	FunctionDefinition const* function) const
{
	static StorageReferenceReturnFacts const empty;
	auto found = function ? storageReferenceReturns.find(function->id()) : storageReferenceReturns.end();
	return found == storageReferenceReturns.end() ? empty : found->second;
}

bool ProgramAnalysis::pointerNeedsCalldata(FunctionType const& type) const
{
	if (type.kind() != FunctionType::Kind::Internal) return false;
	for (auto id: callablesWithCalldata)
		if (auto function = functionDeclarations.find(id); function != functionDeclarations.end())
			if (auto const* candidate = function->second->functionType(true);
				candidate && candidate->hasEqualParameterTypes(type)
				&& candidate->hasEqualReturnTypes(type)) return true;
	return false;
}

ProgramAnalysis ProgramAnalysis::analyze(
	CompilerStack& _compiler,
	bool _evmStorageLayout)
{
	ProgramAnalysis result;

	collectContractFacts(_compiler, result);
	indexFunctionDeclarations(_compiler, result);
	collectCreationEffects(result);
	closeReachability(result);

	BodyFactsWalker bodyFactsWalker(result, !_evmStorageLayout);

	// Body/Yul facts are invariant: collect them once, then close the finite,
	// monotone parameter-transfer graph without an arbitrary depth cutoff.
	collectBodyFacts(bodyFactsWalker);
	// Virtual calls can reach a calldata consumer only in a derived host.
	// Reuse solc's resolution, keeping the actual caller/body edge.
	for (auto const& [caller, call]: bodyFactsWalker.calls)
		for (auto const* host: result.contracts)
			if (result.isCallableReachable(host->id(), caller))
				if (auto const* target = SolcFacts::resolveInternalCall(*call, host))
					result.callableCallers[target->id()].insert(caller);
	for (size_t previous = size_t(-1); previous != result.callablesWithCalldata.size();)
	{
		previous = result.callablesWithCalldata.size();
		closeOverEdges(result.callablesWithCalldata, result.callableCallers);
		for (auto const& [caller, type]: bodyFactsWalker.indirectCalls)
			if (result.pointerNeedsCalldata(*type)) result.callablesWithCalldata.insert(caller);
	}
	bodyFactsWalker.transferCallOffsets();
	auto aliasComponents = result.referenceAssignments;
	for (auto const& [target, sources]: result.referenceAssignments)
		for (auto source: sources) aliasComponents[source].insert(target);
	// A standalone local's rebind needs no identity carrier. Promote only
	// connected aliases or input parameters whose caller retains the entry
	// referent, then close those finite declaration-ID components.
	for (auto id: result.reassignedMemoryLocals)
		if (aliasComponents.contains(id)) result.memoryIdentityDeclarations.insert(id);
	for (auto const& [_, function]: result.functionDeclarations)
		for (auto const& parameter: function->parameters())
			if (result.reassignedMemoryLocals.contains(parameter->id()))
				result.memoryIdentityDeclarations.insert(parameter->id());
	closeOverEdges(result.memoryIdentityDeclarations, aliasComponents);
	for (auto id: bodyFactsWalker.writtenDeclarations)
		result.stableFunctionPointers.erase(id);
	closeOverEdges(result.structRefOffsetParams, bodyFactsWalker.offsetTransfers);
	auto slotSources = result.slotHandleDeclarations;
	// Opaque tuple components need runtime identity. Seed their function IDs
	// before closing the same solc-declaration transfer graph used for .slot,
	// so a single-reference wrapper around a tuple result retains its handle.
	for (auto const& [id, function]: result.functionDeclarations)
		if (function->returnParameters().size() > 1)
			for (auto const& parameter: function->returnParameters())
				if (parameter->referenceLocation() == VariableDeclaration::Location::Storage)
					slotSources.insert(id);
	closeOverEdges(slotSources, bodyFactsWalker.slotTransfers);
	for (auto id: slotSources)
		if (!result.functionDeclarations.contains(id))
			result.slotHandleDeclarations.insert(id);
	deriveStorageReferenceReturns(result, slotSources);

	return result;
}

} // namespace puyasol::builder

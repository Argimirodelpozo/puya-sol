/// @file MemorySharing.cpp
/// Scratch prototype: conservatively choose pointer-backed memory aggregates.
/// Representation is keyed by solc declaration identity, so all host-resolved
/// edges participate in one fixed point, not independent per-host solutions.

#include "builder/context/ProgramAnalysis.h"
#include "builder/solc/PreparedAssembly.h"
#include "builder/solc/SolcFacts.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>
#include <libsolidity/ast/Types.h>

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace puyasol::builder
{
using namespace solidity::frontend;

namespace
{

bool isMemoryReference(Type const* type)
{
	auto const* reference = dynamic_cast<ReferenceType const*>(type);
	return reference && reference->location() == DataLocation::Memory;
}

bool isMemoryAggregate(VariableDeclaration const* declaration)
{
	if (!declaration || declaration->isStateVariable() || !isMemoryReference(declaration->type()))
		return false;
	if (auto const* array = dynamic_cast<ArrayType const*>(declaration->type()))
		return !array->isByteArrayOrString();
	return dynamic_cast<StructType const*>(declaration->type()) != nullptr;
}

VariableDeclaration const* memoryRoot(Expression const& expression)
{
	auto const* identifier = dynamic_cast<Identifier const*>(&SolcFacts::functionExpression(expression));
	auto const* declaration = identifier ? dynamic_cast<VariableDeclaration const*>(
		identifier->annotation().referencedDeclaration) : nullptr;
	return isMemoryAggregate(declaration) ? declaration : nullptr;
}

bool internalCall(FunctionCall const& call)
{
	auto const* type = dynamic_cast<FunctionType const*>(call.expression().annotation().type);
	return call.annotation().kind.set() && *call.annotation().kind == FunctionCallKind::FunctionCall
		&& type && (type->kind() == FunctionType::Kind::Internal
			|| type->kind() == FunctionType::Kind::DelegateCall);
}

// A constructor allocates its own head, but memory-reference children still
// alias their arguments. An internal result may likewise retain an argument.
// External ABI calls copy; their arguments are not provenance of the result.
void referenceSources(Expression const& value, std::function<void(Expression const&)> const& use)
{
	if (!isMemoryReference(value.annotation().type)) return;
	for (auto const* source: SolcFacts::referenceSources(value))
	{
		use(*source);
		if (auto const* call = dynamic_cast<FunctionCall const*>(source); call
			&& (internalCall(*call) || (call->annotation().kind.set()
				&& *call->annotation().kind == FunctionCallKind::StructConstructorCall)))
			for (auto const* argument: SolcFacts::callArguments(*call))
				referenceSources(*argument, use);
	}
}

struct CallEdge
{
	FunctionDefinition const* target;
	std::vector<std::pair<size_t, int64_t>> arguments;
};

struct FunctionFacts
{
	std::set<int64_t> returnRoots;
	std::vector<CallEdge> calls;
	std::vector<std::pair<int64_t, int64_t>> binds;
	std::set<int64_t> returnCalls;
	std::vector<int64_t> parameters;
	std::vector<int64_t> memoryReturns;
	std::set<size_t> mutatedParameters;
};

struct SharingState
{
	MemorySharingFacts result;
	std::set<int64_t> aggregates;
	bool rawMemory = false;
	bool unknownInternalTarget = false;
};

class SharingScanner: public ASTConstVisitor
{
public:
	SharingScanner(ProgramAnalysis const& analysis, ContractDefinition const* host,
		FunctionFacts& facts, SharingState& state)
		: m_analysis(analysis), m_host(host), m_facts(facts), m_state(state) {}

	bool visit(VariableDeclaration const& declaration) override
	{
		if (isMemoryAggregate(&declaration)) m_state.aggregates.insert(declaration.id());
		return true;
	}

	bool visit(VariableDeclarationStatement const& statement) override
	{
		if (auto const* value = statement.initialValue())
			for (size_t i = 0; i < statement.declarations().size(); ++i)
				if (auto const* declaration = statement.declarations()[i].get(); isMemoryAggregate(declaration))
					bindReference(declaration->id(), *value,
						statement.declarations().size() > 1 ? std::optional<size_t>{i} : std::nullopt);
		return true;
	}

	bool visit(Assignment const& assignment) override
	{
		bindAssignment(assignment.leftHandSide(), assignment.rightHandSide());
		return true;
	}

	bool visit(Return const& statement) override
	{
		auto const* value = statement.expression();
		if (!value) return true;
		auto const& returns = statement.annotation().functionReturnParameters->parameters();
		if (returns.size() != 1 || !isMemoryAggregate(returns[0].get())) return true;
		referenceSources(*value, [&](Expression const& source) {
			if (auto const* root = memoryRoot(source)) m_facts.returnRoots.insert(root->id());
			callResult(source, [&](int64_t target) { m_facts.returnCalls.insert(target); }, [&] {
				m_state.result.pointerReturnFunctions.insert(m_functionId);
			});
		});
		return true;
	}

	bool visit(FunctionCall const& call) override
	{
		if (!internalCall(call)) return true;
		auto const* target = SolcFacts::resolveInternalCall(call, m_host);
		auto arguments = SolcFacts::callArguments(call);
		if (!target)
		{
			// A runtime target must agree with the callers' representation too.
			// Without exact target facts, promote all possible memory signatures.
			m_state.unknownInternalTarget = true;
			for (auto const* argument: arguments) shareRoots(*argument);
			return true;
		}
		CallEdge edge{target, {}};
		for (size_t i = 0; i < arguments.size(); ++i)
			if (isMemoryAggregate(target->parameters().at(i).get()))
				referenceSources(*arguments[i], [&](Expression const& source) {
					if (auto const* root = memoryRoot(source)) edge.arguments.emplace_back(i, root->id());
					callResult(source, [&](int64_t producer) {
						m_facts.binds.emplace_back(producer, target->parameters()[i]->id());
					}, [&] { m_state.result.sharedDeclarations.insert(target->parameters()[i]->id()); });
				});
		m_facts.calls.push_back(std::move(edge));
		return true;
	}

	bool visit(InlineAssembly const& assembly) override
	{
		m_state.rawMemory |= m_analysis.preparedAssemblies.at(assembly.id())->facts.rootEffects.memory
			!= solidity::yul::SideEffects::None;
		for (auto const& [_, reference]: assembly.annotation().externalReferences)
			m_state.rawMemory |= isMemoryAggregate(dynamic_cast<VariableDeclaration const*>(reference.declaration));
		return true;
	}

	void scan(FunctionDefinition const& function)
	{
		m_functionId = function.id();
		for (auto const& parameter: function.parameters()) visit(*parameter);
		for (auto const& result: function.returnParameters()) visit(*result);
		if (function.isImplemented()) function.body().accept(*this);
		for (auto const& invocation: function.modifiers())
		{
			auto const* modifier = SolcFacts::resolveModifier(*invocation, m_host);
			if (auto const* arguments = invocation->arguments())
				for (size_t i = 0; i < arguments->size(); ++i)
				{
					auto const& argument = *(*arguments)[i];
					argument.accept(*this);
					if (modifier && isMemoryAggregate(modifier->parameters().at(i).get()))
						bindReference(modifier->parameters()[i]->id(), argument);
				}
			if (modifier)
			{
				for (auto const& parameter: modifier->parameters()) visit(*parameter);
				if (modifier->isImplemented()) modifier->body().accept(*this);
			}
		}
		if (function.isImplemented())
		{
			auto const& mutations = m_analysis.parameterMutations(m_host, function);
			for (size_t i = 0; i < function.parameters().size(); ++i)
				if (mutations.mutates(i) && isMemoryAggregate(function.parameters()[i].get()))
					m_facts.mutatedParameters.insert(i);
		}
	}

private:
	ProgramAnalysis const& m_analysis;
	ContractDefinition const* m_host;
	FunctionFacts& m_facts;
	SharingState& m_state;
	int64_t m_functionId = 0;

	void shareRoots(Expression const& value)
	{
		referenceSources(value, [&](Expression const& source) {
			if (auto const* root = memoryRoot(source)) m_state.result.sharedDeclarations.insert(root->id());
		});
	}

	void bindAssignment(Expression const& expression, Expression const& rhs,
		std::optional<size_t> component = {})
	{
		auto const& lhs = SolcFacts::functionExpression(expression);
		if (auto const* tuple = dynamic_cast<TupleExpression const*>(&lhs))
		{
			for (size_t i = 0; i < tuple->components().size(); ++i)
				if (tuple->components()[i]) bindAssignment(*tuple->components()[i], rhs, i);
		}
		else if (auto const* root = memoryRoot(lhs)) bindReference(root->id(), rhs, component);
		else if (isMemoryReference(lhs.annotation().type))
			referenceSources(lhs, [&](Expression const& source) {
				if (auto const* root = memoryRoot(source)) bindReference(root->id(), rhs, component);
			});
	}

	void bindReference(int64_t destination, Expression const& expression, std::optional<size_t> component = {})
	{
		auto const& value = SolcFacts::functionExpression(expression);
		if (auto const* tuple = dynamic_cast<TupleExpression const*>(&value); tuple && component)
		{
			if (tuple->components().at(*component)) bindReference(destination, *tuple->components()[*component]);
			return;
		}
		referenceSources(value, [&](Expression const& source) {
			if (auto const* root = memoryRoot(source))
			{
				m_state.result.sharedDeclarations.insert(root->id());
				m_state.result.sharedDeclarations.insert(destination);
			}
			callResult(source, [&](int64_t target) { m_facts.binds.emplace_back(target, destination); },
				[&] { m_state.result.sharedDeclarations.insert(destination); });
		});
	}

	template <typename Resolved, typename Unknown>
	void callResult(Expression const& source, Resolved&& resolved, Unknown&& unknown)
	{
		if (auto const* call = dynamic_cast<FunctionCall const*>(&source); call && internalCall(*call))
		{
			if (auto const* target = SolcFacts::resolveInternalCall(*call, m_host)) resolved(target->id());
			else unknown();
		}
	}
};

} // namespace

MemorySharingFacts analyzeMemorySharing(ProgramAnalysis const& analysis)
{
	SharingState state;
	std::map<int64_t, FunctionFacts> facts;
	for (auto const& [id, function]: analysis.functionDeclarations)
	{
		auto& node = facts[id];
		for (auto const& parameter: function->parameters()) node.parameters.push_back(parameter->id());
		for (auto const& result: function->returnParameters())
			if (isMemoryAggregate(result.get()))
			{
				node.memoryReturns.push_back(result->id());
				// Includes implicit fallthrough and bare returns, not just Return expressions.
				if (function->returnParameters().size() == 1 && !result->name().empty())
					node.returnRoots.insert(result->id());
			}
		auto const* owner = function->annotation().contract;
		bool scanned = false;
		for (auto const* host: analysis.contracts)
		{
			auto const& bases = host->annotation().linearizedBaseContracts;
			if (owner && !owner->isLibrary() && std::find(bases.begin(), bases.end(), owner) == bases.end()) continue;
			if (analysis.hasContractReachability(host->id()) && !analysis.isCallableReachable(host->id(), id)) continue;
			SharingScanner(analysis, host, node, state).scan(*function);
			scanned = true;
		}
		if (!scanned) SharingScanner(analysis, owner, node, state).scan(*function);
	}
	auto& shared = state.result.sharedDeclarations;
	auto& pointerReturns = state.result.pointerReturnFunctions;
	if (state.rawMemory)
		// Raw Yul can inspect an object without mentioning its declaration. Merely
		// reserving its address while keeping its bytes elsewhere is not sound.
		shared.insert(state.aggregates.begin(), state.aggregates.end());
	if (state.unknownInternalTarget)
		for (auto const& [id, node]: facts)
		{
			for (auto parameter: node.parameters) if (state.aggregates.contains(parameter)) shared.insert(parameter);
			shared.insert(node.memoryReturns.begin(), node.memoryReturns.end());
			if (!node.memoryReturns.empty()) pointerReturns.insert(id);
		}
	bool changed;
	do
	{
		changed = false;
		auto promote = [&](int64_t id) { changed = shared.insert(id).second || changed; };
		for (auto const& [id, node]: facts)
		{
			for (auto const& edge: node.calls)
			{
				auto const& callee = facts.at(edge.target->id());
				auto pointer = [&](size_t index) { return shared.contains(callee.parameters.at(index)); };
				for (auto const& [index, root]: edge.arguments)
				{
					if (pointer(index)) promote(root);
					bool alias = shared.contains(root) && callee.mutatedParameters.contains(index);
					for (auto const& [other, otherRoot]: edge.arguments)
						if (other != index && (pointer(other) || callee.mutatedParameters.contains(other))
							&& (root == otherRoot || (shared.contains(root) && shared.contains(otherRoot)))) alias = true;
					if (alias) promote(callee.parameters.at(index));
				}
			}
			for (auto const& [target, destination]: node.binds)
				if (pointerReturns.contains(target)) promote(destination);
			bool returnsShared = pointerReturns.contains(id);
			// Always visit roots: an earlier opaque return must not suppress promotion
			// of a parameter returned by another branch.
			for (auto root: node.returnRoots)
			{
				if (std::find(node.parameters.begin(), node.parameters.end(), root) != node.parameters.end()) promote(root);
				returnsShared |= shared.contains(root);
			}
			for (auto target: node.returnCalls) returnsShared |= pointerReturns.contains(target);
			if (returnsShared)
			{
				changed = pointerReturns.insert(id).second || changed;
				for (auto result: node.memoryReturns) promote(result);
			}
		}
	}
	while (changed);
	return std::move(state.result);
}

} // namespace puyasol::builder

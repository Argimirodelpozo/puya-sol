/// @file MemorySharing.cpp
/// Scratch memory model: which memory aggregates need the pointer
/// representation. An object is unique when one variable owns it for its whole
/// lifetime; everything else (aliased, stored into another object, returned
/// from a callee's parameter, forwarded to a pointer parameter, mutated through
/// a parameter, captured by a modifier, touched by assembly) is shared and gets a
/// pointer. Unknown internal callees share whatever they receive.

#include "builder/context/ProgramAnalysis.h"
#include "builder/solc/SolcFacts.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>
#include <libsolidity/ast/Types.h>

#include <algorithm>
#include <exception>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace puyasol::builder
{
using namespace solidity::frontend;

namespace
{

bool isMemoryAggregate(VariableDeclaration const* declaration)
{
	if (!declaration || declaration->referenceLocation() != VariableDeclaration::Location::Memory)
		return false;
	auto const* type = declaration->type();
	if (!type || type->isValueType())
		return false;
	if (auto const* array = dynamic_cast<ArrayType const*>(type))
		return !array->isByteArrayOrString();
	return dynamic_cast<StructType const*>(type) != nullptr;
}

VariableDeclaration const* memoryAggregateRoot(Expression const* expression)
{
	if (auto const* identifier = dynamic_cast<Identifier const*>(expression))
	{
		auto const* declaration = dynamic_cast<VariableDeclaration const*>(
			identifier->annotation().referencedDeclaration);
		return isMemoryAggregate(declaration) && !declaration->isStateVariable() ? declaration : nullptr;
	}
	return nullptr;
}

bool isMemoryReferenceType(Type const* type)
{
	auto const* reference = dynamic_cast<ReferenceType const*>(type);
	return reference && reference->location() == DataLocation::Memory;
}

struct CallEdge
{
	FunctionDefinition const* target = nullptr;
	/// `(callee parameter index, caller root declaration)` for every memory
	/// aggregate argument of this call site.
	std::vector<std::pair<size_t, int64_t>> arguments;
};

struct BindEdge
{
	FunctionDefinition const* target = nullptr;
	int64_t declaration = 0;
};

struct FunctionFacts
{
	std::set<int64_t> shared;
	std::set<int64_t> returnRoots;
	std::vector<CallEdge> calls;
	std::vector<BindEdge> binds;
	std::vector<FunctionDefinition const*> returnCalls;
	bool returnsShared = false;
	std::vector<int64_t> parameters;
	std::vector<int64_t> namedReturns;
	std::set<size_t> mutatedParameters;
};

class SharingScanner: public ASTConstVisitor
{
public:
	SharingScanner(ContractDefinition const* mostDerived, FunctionFacts& facts)
		: m_mostDerived(mostDerived), m_facts(facts) {}

	bool visit(VariableDeclarationStatement const& statement) override
	{
		auto const* value = statement.initialValue();
		if (!value) return true;
		auto const& declarations = statement.declarations();
		if (declarations.size() == 1)
		{
			if (isMemoryAggregate(declarations[0].get()))
				bindReference(*declarations[0], *value);
		}
		else if (auto const* tuple = dynamic_cast<TupleExpression const*>(value))
			for (size_t i = 0; i < declarations.size() && i < tuple->components().size(); ++i)
				if (isMemoryAggregate(declarations[i].get()) && tuple->components()[i])
					bindReference(*declarations[i], *tuple->components()[i]);
		return true;
	}

	bool visit(Assignment const& assignment) override
	{
		auto const& lhs = assignment.leftHandSide();
		auto const& rhs = assignment.rightHandSide();
		if (auto const* declaration = memoryAggregateRoot(&lhs))
		{
			bindReference(*declaration, rhs);
			return true;
		}
		if (auto const* tuple = dynamic_cast<TupleExpression const*>(&lhs))
		{
			auto const* values = dynamic_cast<TupleExpression const*>(&rhs);
			for (size_t i = 0; i < tuple->components().size(); ++i)
				if (auto const* declaration = memoryAggregateRoot(tuple->components()[i].get()))
				{
					if (values && i < values->components().size() && values->components()[i])
						bindReference(*declaration, *values->components()[i]);
				}
			return true;
		}
		// Storing a reference into another object's slot: both sides share.
		if ((dynamic_cast<MemberAccess const*>(&lhs) || dynamic_cast<IndexAccess const*>(&lhs))
			&& isMemoryReferenceType(lhs.annotation().type))
		{
			bool aliased = false;
			for (auto const* source: SolcFacts::referenceSources(rhs))
				if (auto const* root = memoryAggregateRoot(source))
				{
					m_facts.shared.insert(root->id());
					aliased = true;
				}
			if (aliased)
				for (auto const* source: SolcFacts::referenceSources(lhs))
					if (auto const* root = memoryAggregateRoot(source))
						m_facts.shared.insert(root->id());
		}
		return true;
	}

	bool visit(Return const& statement) override
	{
		auto const* value = statement.expression();
		if (!value) return true;
		auto const& returns = statement.annotation().functionReturnParameters->parameters();
		if (returns.size() != 1 || !isMemoryAggregate(returns[0].get()))
			return true;
		for (auto const* source: SolcFacts::referenceSources(*value))
		{
			if (auto const* root = memoryAggregateRoot(source))
			{
				m_facts.returnRoots.insert(root->id());
				continue;
			}
			if (auto const* call = dynamic_cast<FunctionCall const*>(source))
				recordCallResult(*call, [&](FunctionDefinition const* target) {
					m_facts.returnCalls.push_back(target); }, [&] { m_facts.returnsShared = true; });
		}
		return true;
	}

	bool visit(FunctionCall const& call) override
	{
		auto const* functionType = dynamic_cast<FunctionType const*>(
			call.expression().annotation().type);
		if (!functionType || !call.annotation().kind.set() || *call.annotation().kind != FunctionCallKind::FunctionCall)
			return true;
		auto const* target = SolcFacts::resolveInternalCall(call, m_mostDerived);
		auto arguments = SolcFacts::callArguments(call);
		if (!target)
		{
			// Dynamic internal targets may alias anything they receive.
			if (functionType->kind() == FunctionType::Kind::Internal)
				for (auto const* argument: arguments)
					if (argument)
						for (auto const* source: SolcFacts::referenceSources(*argument))
							if (auto const* root = memoryAggregateRoot(source))
								m_facts.shared.insert(root->id());
			return true;
		}
		CallEdge edge{target, {}};
		for (size_t i = 0; i < arguments.size() && i < target->parameters().size(); ++i)
		{
			if (!arguments[i] || !isMemoryAggregate(target->parameters()[i].get()))
				continue;
			for (auto const* source: SolcFacts::referenceSources(*arguments[i]))
				if (auto const* root = memoryAggregateRoot(source))
					edge.arguments.emplace_back(i, root->id());
		}
		if (!edge.arguments.empty())
			m_facts.calls.push_back(std::move(edge));
		return true;
	}

	bool visit(InlineAssembly const& assembly) override
	{
		for (auto const& [_, reference]: assembly.annotation().externalReferences)
			if (auto const* declaration = dynamic_cast<VariableDeclaration const*>(reference.declaration);
				isMemoryAggregate(declaration))
				m_facts.shared.insert(declaration->id());
		return true;
	}

	void recordModifierArgument(Expression const& argument)
	{
		for (auto const* source: SolcFacts::referenceSources(argument))
			if (auto const* root = memoryAggregateRoot(source))
				m_facts.shared.insert(root->id());
	}

private:
	ContractDefinition const* m_mostDerived;
	FunctionFacts& m_facts;

	/// `declaration` now refers to whatever `value` denotes: an existing object
	/// (both share), a callee result (shared if the callee returns a shared
	/// object), or a fresh value (unique).
	void bindReference(VariableDeclaration const& declaration, Expression const& value)
	{
		for (auto const* source: SolcFacts::referenceSources(value))
		{
			if (auto const* root = memoryAggregateRoot(source))
			{
				m_facts.shared.insert(root->id());
				m_facts.shared.insert(declaration.id());
				continue;
			}
			if (auto const* call = dynamic_cast<FunctionCall const*>(source))
				recordCallResult(*call, [&](FunctionDefinition const* target) {
					m_facts.binds.push_back({target, declaration.id()}); },
					[&] { m_facts.shared.insert(declaration.id()); });
		}
	}

	void recordCallResult(FunctionCall const& call,
		std::function<void(FunctionDefinition const*)> const& resolved,
		std::function<void()> const& unknown)
	{
		auto const* functionType = dynamic_cast<FunctionType const*>(
			call.expression().annotation().type);
		if (!functionType || !call.annotation().kind.set() || *call.annotation().kind != FunctionCallKind::FunctionCall)
			return; // struct constructors and conversions are fresh values
		if (auto const* target = SolcFacts::resolveInternalCall(call, m_mostDerived))
			resolved(target);
		else if (functionType->kind() == FunctionType::Kind::Internal)
			unknown();
		// External and builtin calls return fresh copies.
	}
};

void scanFunction(
	ProgramAnalysis const& analysis,
	ContractDefinition const* mostDerived,
	FunctionDefinition const& function,
	FunctionFacts& facts)
{
	for (auto const& parameter: function.parameters())
		facts.parameters.push_back(parameter->id());
	for (auto const& result: function.returnParameters())
		if (isMemoryAggregate(result.get()))
			facts.namedReturns.push_back(result->id());
	SharingScanner scanner(mostDerived, facts);
	if (function.isImplemented())
		function.body().accept(scanner);
	for (auto const& invocation: function.modifiers())
	{
		auto const* modifier = SolcFacts::resolveModifier(*invocation, mostDerived);
		auto const* arguments = invocation->arguments();
		if (!modifier || !arguments) continue;
		auto const& parameters = modifier->parameters();
		for (size_t i = 0; i < arguments->size() && i < parameters.size(); ++i)
			if (isMemoryAggregate(parameters[i].get()))
				scanner.recordModifierArgument(*(*arguments)[i]);
		if (modifier->isImplemented())
			modifier->body().accept(scanner);
	}
	// A mutated parameter stays a value with write-back while every caller
	// passes a unique object; the call-site rules below promote it otherwise.
	if (function.isImplemented())
	{
		auto const& mutations = analysis.parameterMutations(mostDerived, function);
		for (size_t i = 0; i < facts.parameters.size(); ++i)
			if (mutations.mutates(i) && isMemoryAggregate(function.parameters()[i].get()))
				facts.mutatedParameters.insert(i);
	}
}

} // namespace

MemorySharingFacts analyzeMemorySharing(ProgramAnalysis const& analysis)
{
	MemorySharingFacts result;
	for (auto const* contract: analysis.contracts)
	{
		std::map<int64_t, FunctionFacts> facts;
		std::map<int64_t, FunctionDefinition const*> functions;
		for (auto const& [id, function]: analysis.functionDeclarations)
		{
			if (analysis.hasContractReachability(contract->id()) && !analysis.isCallableReachable(contract->id(), id))
				continue;
			functions.emplace(id, function);
			try
			{
				scanFunction(analysis, contract, *function, facts[id]);
			}
			catch (std::exception const&)
			{
				// Facts unavailable (the body is not lowerable): share everything.
				auto& node = facts[id];
				node.shared.insert(node.parameters.begin(), node.parameters.end());
				node.shared.insert(node.namedReturns.begin(), node.namedReturns.end());
				for (size_t i = 0; i < node.parameters.size(); ++i)
					node.mutatedParameters.insert(i);
				node.returnsShared = true;
			}
		}
		auto pointerParameter = [&](FunctionDefinition const* target, size_t index) {
			auto found = facts.find(target->id());
			if (found == facts.end() || index >= found->second.parameters.size())
				return true; // outside the analysed set: share
			return found->second.shared.contains(found->second.parameters[index]) != 0;
		};
		auto returnsShared = [&](FunctionDefinition const* target) {
			auto found = facts.find(target->id());
			return found == facts.end() || found->second.returnsShared;
		};
		bool changed;
		do
		{
			changed = false;
			for (auto& [id, node]: facts)
			{
				for (auto const& edge: node.calls)
				{
					auto callee = facts.find(edge.target->id());
					auto mutatedOrPointer = [&](size_t index) {
						return pointerParameter(edge.target, index)
							|| (callee != facts.end() && callee->second.mutatedParameters.contains(index));
					};
					for (auto const& [index, root]: edge.arguments)
						if (pointerParameter(edge.target, index))
							changed = node.shared.insert(root).second || changed;
					if (callee == facts.end())
						continue;
					// A value parameter is unsound when its object can be written through
					// another path during the call: a shared object given to a mutated
					// parameter, the same root in two slots (`f(a, a)`), or two shared
					// roots where one slot is written.
					for (auto const& [index, root]: edge.arguments)
					{
						if (pointerParameter(edge.target, index) || index >= callee->second.parameters.size())
							continue;
						bool const shared = node.shared.contains(root);
						bool promote = shared && callee->second.mutatedParameters.contains(index);
						for (auto const& [other, otherRoot]: edge.arguments)
							if (other != index && mutatedOrPointer(other)
								&& (otherRoot == root || (shared && node.shared.contains(otherRoot))))
								promote = true;
						if (promote)
							changed = callee->second.shared.insert(callee->second.parameters[index]).second || changed;
					}
				}
				for (auto const& edge: node.binds)
					if (returnsShared(edge.target))
						changed = node.shared.insert(edge.declaration).second || changed;
				if (!node.returnsShared)
				{
					bool shared = false;
					for (auto root: node.returnRoots)
					{
						// A returned parameter IS the caller's object: it must travel and
						// come back as a pointer for the caller's binding to alias it.
						if (std::find(node.parameters.begin(), node.parameters.end(), root)
							!= node.parameters.end())
						{
							changed = node.shared.insert(root).second || changed;
							shared = true;
						}
						else if (node.shared.contains(root))
							shared = true;
					}
					for (auto const* target: node.returnCalls)
						if (returnsShared(target)) shared = true;
					if (shared)
					{
						node.returnsShared = true;
						changed = true;
					}
				}
				if (node.returnsShared)
					for (auto id2: node.namedReturns)
						changed = node.shared.insert(id2).second || changed;
			}
		}
		while (changed);
		for (auto const& [id, node]: facts)
		{
			result.sharedDeclarations.insert(node.shared.begin(), node.shared.end());
			if (node.returnsShared)
				result.pointerReturnFunctions.insert(id);
		}
	}
	return result;
}

} // namespace puyasol::builder

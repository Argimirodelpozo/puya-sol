#include "builder/ProgramAnalysis.h"
#include "builder/SolcFacts.h"

#include "builder/PreparedAssembly.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>
#include <libsolidity/ast/Types.h>

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace puyasol::builder
{

using namespace solidity::frontend;

namespace
{

using MutationKey = std::pair<int64_t, int64_t>;

int64_t contractContextId(ContractDefinition const* _contract)
{
	return _contract ? _contract->id() : 0;
}

bool isReferenceParameter(VariableDeclaration const& _parameter)
{
	if (_parameter.referenceLocation() == VariableDeclaration::Location::Storage)
		return true;
	return _parameter.referenceLocation() == VariableDeclaration::Location::Memory
		&& _parameter.type() && !_parameter.type()->isValueType();
}

struct MutationEdge
{
	FunctionDefinition const* target = nullptr;
	/// `(target parameter index, caller parameter index)`.
	std::vector<std::pair<size_t, size_t>> parameterMap;
};

struct NodeFacts
{
	ParameterMutationSummary direct;
	std::vector<MutationEdge> edges;
};

class DirectMutationScanner: public ASTConstVisitor
{
public:
	DirectMutationScanner(
		ProgramAnalysis const& _analysis,
		ContractDefinition const* _mostDerived,
		FunctionDefinition const& _caller,
		NodeFacts& _facts)
		: m_analysis(_analysis), m_mostDerived(_mostDerived), m_facts(_facts)
	{
		for (size_t i = 0; i < _caller.parameters().size(); ++i)
			m_parameterIndexById[_caller.parameters()[i]->id()] = i;
	}

	bool visit(MemberAccess const& _expression) override
	{
		recordIfWritten(_expression);
		return true;
	}

	bool visit(IndexAccess const& _expression) override
	{
		recordIfWritten(_expression);
		return true;
	}

	bool visit(IndexRangeAccess const& _expression) override
	{
		recordIfWritten(_expression);
		return true;
	}

	bool visit(InlineAssembly const& assembly) override
	{
		auto const& prepared = *m_analysis.preparedAssemblies.at(assembly.id());
		m_facts.direct.assemblyEffects += prepared.facts.rootEffects;
		if (prepared.facts.rootEffects.memory == solidity::yul::SideEffects::Write)
			for (auto const& [_, reference]: prepared.externalReferences)
				if (auto const* variable = dynamic_cast<VariableDeclaration const*>(reference.declaration);
					variable && !variable->type()->isValueType()
					&& variable->type()->dataStoredIn(DataLocation::Memory))
					recordDeclarationRoots(variable->id(), m_facts.direct.mutatedParameterIndices);
		return false;
	}

	// Only referent writes count: bare names (including tuple components)
	// rebind. Delete, unlike assignment, clears the referenced object.
	bool visit(UnaryOperation const& expression) override
	{
		if (expression.getOperator() == Token::Delete)
			recordRoots(&expression.subExpression(), m_facts.direct.mutatedParameterIndices);
		return true;
	}

	bool visit(FunctionCall const& _call) override
	{
		auto const* functionType = dynamic_cast<FunctionType const*>(
			_call.expression().annotation().type);
		if (functionType
			&& (functionType->kind() == FunctionType::Kind::ArrayPush
				|| functionType->kind() == FunctionType::Kind::ArrayPop))
			if (auto const* member = dynamic_cast<MemberAccess const*>(
				&SolcFacts::functionExpression(_call.expression())))
				recordRoots(&member->expression(),
					m_facts.direct.mutatedParameterIndices);

		auto const* target = SolcFacts::resolveInternalCall(_call, m_mostDerived);
		if (!target)
		{
			// Indirect internal calls may write any reference argument. Unknown
			// targets must not turn a mutating wrapper into a read-only summary.
			if (functionType && functionType->kind() == FunctionType::Kind::Internal)
			{
				m_facts.direct.assemblyEffects = solidity::yul::SideEffects::worst();
				for (auto const& argument: _call.arguments())
					recordRoots(argument.get(), m_facts.direct.mutatedParameterIndices);
			}
			return true;
		}

		MutationEdge edge;
		edge.target = target;
		auto arguments = SolcFacts::callArguments(_call);
		for (size_t i = 0; i < arguments.size(); ++i)
			if (arguments[i]) mapArgument(edge, i, *arguments[i]);

		m_facts.edges.push_back(std::move(edge));
		return true;
	}

	void recordModifierMemoryAlias(Expression const& _argument)
	{
		recordRoots(&_argument, m_facts.direct.mutatedParameterIndices);
	}

private:
	ProgramAnalysis const& m_analysis;
	ContractDefinition const* m_mostDerived;
	NodeFacts& m_facts;
	std::map<int64_t, size_t> m_parameterIndexById;

	void recordIfWritten(Expression const& _expression)
	{
		if (_expression.annotation().willBeWrittenTo)
			recordRoots(&_expression, m_facts.direct.mutatedParameterIndices);
	}

	void mapArgument(
		MutationEdge& _edge,
		size_t _targetParameterIndex,
		Expression const& _argument)
	{
		if (_targetParameterIndex >= _edge.target->parameters().size()
			|| !isReferenceParameter(
				*_edge.target->parameters()[_targetParameterIndex]))
			return;
		std::set<size_t> roots;
		recordRoots(&_argument, roots);
		for (size_t root: roots)
			_edge.parameterMap.emplace_back(_targetParameterIndex, root);
	}

	void recordRoots(Expression const* expression, std::set<size_t>& out)
	{
		if (!expression) return;
		for (auto const* source: SolcFacts::referenceSources(*expression))
		{
			if (auto const* call = dynamic_cast<FunctionCall const*>(source))
				if (auto const* function = SolcFacts::resolveInternalCall(*call, m_mostDerived))
					if (auto const& alias = m_analysis.storageReturnFacts(function).pointerAlias)
					{
						auto arguments = SolcFacts::callArguments(*call);
						if (alias->parameter < arguments.size())
							recordRoots(arguments[alias->parameter], out);
					}
			if (auto const* identifier = dynamic_cast<Identifier const*>(source))
				if (auto const* declaration = identifier->annotation().referencedDeclaration)
					recordDeclarationRoots(declaration->id(), out);
		}
	}

	void recordDeclarationRoots(int64_t declaration, std::set<size_t>& out)
	{
		std::set<int64_t> seen;
		std::vector<int64_t> pending{declaration};
		for (size_t i = 0; i < pending.size(); ++i)
		{
			auto id = pending[i];
			if (!seen.insert(id).second) continue;
			if (auto found = m_parameterIndexById.find(id); found != m_parameterIndexById.end())
				out.insert(found->second);
			if (auto aliases = m_analysis.referenceAssignments.find(id);
				aliases != m_analysis.referenceAssignments.end())
				pending.insert(pending.end(), aliases->second.begin(), aliases->second.end());
		}
	}

};

ParameterMutationSummary const& analyzeFrom(
	ProgramAnalysis const& _analysis,
	ContractDefinition const* _mostDerived,
	FunctionDefinition const& _root)
{
	auto const context = contractContextId(_mostDerived);
	MutationKey const rootKey{context, _root.id()};
	if (auto found = _analysis.parameterMutationSummaries.find(rootKey);
		found != _analysis.parameterMutationSummaries.end())
		return found->second;

	std::map<int64_t, NodeFacts> facts;
	std::function<void(FunctionDefinition const&)> discover;
	discover = [&](FunctionDefinition const& function) {
		if (facts.count(function.id())
			|| _analysis.parameterMutationSummaries.contains({context, function.id()}))
			return;
		auto [it, _] = facts.emplace(function.id(), NodeFacts{});
		auto& node = it->second;
		DirectMutationScanner scanner(_analysis, _mostDerived, function, node);
		if (function.isImplemented())
			function.body().accept(scanner);
		// A modifier's memory-reference parameter aliases its argument. The
		// modifier chain preserves direct identifier aliases, so conservatively
		// thread any enclosing function parameter supplied at such a position.
		// This also lets the existing call-edge fixed point propagate the effect
		// through free/library caller chains.
		for (auto const& invocation: function.modifiers())
		{
			auto const* modifier = SolcFacts::resolveModifier(
				*invocation, _mostDerived);
			auto const* arguments = invocation->arguments();
			if (arguments) for (auto const& argument: *arguments) argument->accept(scanner);
			if (modifier && modifier->isImplemented()) modifier->body().accept(scanner);
			if (!modifier || !arguments)
				continue;
			auto const& parameters = modifier->parameters();
			for (size_t i = 0;
				i < arguments->size() && i < parameters.size(); ++i)
				if (parameters[i]->referenceLocation()
						== VariableDeclaration::Location::Memory)
					scanner.recordModifierMemoryAlias(*(*arguments)[i]);
		}
		for (auto const& edge: node.edges)
			if (edge.target)
				discover(*edge.target);
	};
	discover(_root);

	std::map<int64_t, ParameterMutationSummary> summaries;
	for (auto const& [id, node]: facts)
		summaries[id] = node.direct;

	bool changed;
	do
	{
		changed = false;
		for (auto const& [id, node]: facts)
			for (auto const& edge: node.edges)
			{
				if (!edge.target)
					continue;
				auto const target = summaries.find(edge.target->id());
				auto const& summary = target != summaries.end() ? target->second
					: _analysis.parameterMutationSummaries.at({context, edge.target->id()});
				auto& effects = summaries[id].assemblyEffects;
				auto combined = effects + summary.assemblyEffects;
				changed |= combined != effects;
				effects = combined;
				for (auto const& [targetParam, callerParam]: edge.parameterMap)
					if (summary.mutates(targetParam))
						changed = summaries[id].mutatedParameterIndices
							.insert(callerParam).second || changed;
			}
	}
	while (changed);

	for (auto& [id, summary]: summaries)
		_analysis.parameterMutationSummaries.insert_or_assign(
			MutationKey{context, id}, std::move(summary));
	return _analysis.parameterMutationSummaries.at(rootKey);
}

} // namespace

ParameterMutationSummary const& ProgramAnalysis::parameterMutations(
	ContractDefinition const* _mostDerived,
	FunctionDefinition const& _function) const
{
	return analyzeFrom(*this, _mostDerived, _function);
}

} // namespace puyasol::builder

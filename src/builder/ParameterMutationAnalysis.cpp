#include "builder/ProgramAnalysis.h"

#include "builder/sol-ast/AsmScan.h"

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

ContractDefinition const* lexicalContract(FunctionDefinition const* _function)
{
	return _function ? _function->annotation().contract : nullptr;
}

bool belongsToHierarchy(
	ContractDefinition const& _mostDerived,
	ContractDefinition const* _scope)
{
	if (!_scope)
		return false;
	auto const& bases = _mostDerived.annotation().linearizedBaseContracts;
	return std::find(bases.begin(), bases.end(), _scope) != bases.end();
}

Expression const* unwrappedCallee(Expression const& _expression)
{
	auto const* result = &_expression;
	if (auto const* options = dynamic_cast<FunctionCallOptions const*>(result))
		result = &options->expression();
	if (auto const* tuple = dynamic_cast<TupleExpression const*>(result);
		tuple && tuple->components().size() == 1 && tuple->components()[0])
		result = tuple->components()[0].get();
	return result;
}

FunctionDefinition const* referencedFunction(FunctionCall const& _call)
{
	auto const* callee = unwrappedCallee(_call.expression());
	Declaration const* declaration = nullptr;
	if (auto const* identifier = dynamic_cast<Identifier const*>(callee))
		declaration = identifier->annotation().referencedDeclaration;
	else if (auto const* member = dynamic_cast<MemberAccess const*>(callee))
		declaration = member->annotation().referencedDeclaration;
	return dynamic_cast<FunctionDefinition const*>(declaration);
}

/// Resolve the exact implementation used by this call site. This deliberately
/// mirrors solc's own lookup split: Static keeps an explicit Base.f target,
/// Super starts at the lexical contract's successor, and Virtual searches from
/// the most-derived contract.
FunctionDefinition const* resolveCallTarget(
	ContractDefinition const* _mostDerived,
	FunctionDefinition const* _caller,
	FunctionCall const& _call)
{
	if (!_call.annotation().kind.set()
		|| *_call.annotation().kind != FunctionCallKind::FunctionCall)
		return nullptr;
	auto const* functionType = dynamic_cast<FunctionType const*>(
		_call.expression().annotation().type);
	auto const* declaration = referencedFunction(_call);
	if (!functionType || !declaration)
		return nullptr;
	// Ordinary external/delegate calls cross an ABI boundary and do not
	// preserve memory/storage reference identity.  Public/external LIBRARY
	// calls are the exception in this backend: solc types them as DelegateCall,
	// but CallResolver intentionally lowers them to an internal subroutine.
	// Keep that call edge so its mutated reference parameters are threaded back
	// to the caller just like an internal library function's.
	bool const internalCall =
		functionType->kind() == FunctionType::Kind::Internal;
	auto const* declarationScope = declaration->annotation().contract;
	bool const internalizedLibraryDelegateCall =
		functionType->kind() == FunctionType::Kind::DelegateCall
		&& declarationScope && declarationScope->isLibrary();
	if (!internalCall && !internalizedLibraryDelegateCall)
		return nullptr;
	auto const* scope = declaration->annotation().contract;
	if (!_mostDerived || !scope || scope->isLibrary() || declaration->isFree()
		|| declaration->isConstructor() || !declaration->isOrdinary()
		|| declaration->name().empty()
		|| !belongsToHierarchy(*_mostDerived, scope))
		return declaration;

	auto const* callee = unwrappedCallee(_call.expression());
	if (auto const* member = dynamic_cast<MemberAccess const*>(callee);
		member && member->annotation().requiredLookup.set())
	{
		switch (*member->annotation().requiredLookup)
		{
		case VirtualLookup::Static:
			return declaration;
		case VirtualLookup::Super:
		{
			auto const* callerContract = lexicalContract(_caller);
			auto const* searchStart = callerContract
				? callerContract->superContract(*_mostDerived) : nullptr;
			return searchStart
				? &declaration->resolveVirtual(*_mostDerived, searchStart)
				: declaration;
		}
		case VirtualLookup::Virtual:
			break;
		}
	}

	return &declaration->resolveVirtual(*_mostDerived);
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
		ContractDefinition const* _mostDerived,
		FunctionDefinition const& _caller,
		NodeFacts& _facts)
		: m_mostDerived(_mostDerived), m_caller(_caller), m_facts(_facts)
	{
		for (size_t i = 0; i < _caller.parameters().size(); ++i)
			m_parameterIndexById[_caller.parameters()[i]->id()] = i;
	}

	bool visit(Identifier const& _expression) override
	{
		recordIfWritten(_expression);
		return true;
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

	bool visit(TupleExpression const& _expression) override
	{
		recordIfWritten(_expression);
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
				unwrappedCallee(_call.expression())))
				recordRoots(&member->expression(),
					m_facts.direct.mutatedParameterIndices);

		auto const* target = resolveCallTarget(
			m_mostDerived, &m_caller, _call);
		if (!target)
			return true;

		MutationEdge edge;
		edge.target = target;
		auto const& params = target->parameters();
		bool const bound = functionType && functionType->hasBoundFirstArgument();
		if (bound && !params.empty())
			if (auto const* member = dynamic_cast<MemberAccess const*>(
				unwrappedCallee(_call.expression())))
				mapArgument(edge, 0, member->expression());

		auto arguments = _call.sortedArguments();
		size_t const shift = bound ? 1 : 0;
		for (size_t i = 0; i < arguments.size(); ++i)
			if (arguments[i] && i + shift < params.size())
				mapArgument(edge, i + shift, *arguments[i]);

		m_facts.edges.push_back(std::move(edge));
		return true;
	}

private:
	ContractDefinition const* m_mostDerived;
	FunctionDefinition const& m_caller;
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

	void recordRoots(Expression const* _expression, std::set<size_t>& _out)
	{
		while (_expression)
		{
			if (auto const* index = dynamic_cast<IndexAccess const*>(_expression))
			{
				_expression = &index->baseExpression();
				continue;
			}
			if (auto const* range = dynamic_cast<IndexRangeAccess const*>(_expression))
			{
				_expression = &range->baseExpression();
				continue;
			}
			if (auto const* member = dynamic_cast<MemberAccess const*>(_expression))
			{
				_expression = &member->expression();
				continue;
			}
			// StorageSlot-style helpers return a storage pointer whose `.value`
			// aliases one of their arguments. Follow that declared alias instead
			// of treating the call result as a fresh value.
			if (auto const* call = dynamic_cast<FunctionCall const*>(_expression))
			{
				if (auto const* function = referencedFunction(*call))
					if (auto alias = storagePointerAliasParam(*function))
					{
						auto arguments = call->sortedArguments();
						if (alias->first < arguments.size() && arguments[alias->first])
						{
							_expression = arguments[alias->first].get();
							continue;
						}
					}
			}
			if (auto const* tuple = dynamic_cast<TupleExpression const*>(_expression))
			{
				for (auto const& component: tuple->components())
					if (component)
						recordRoots(component.get(), _out);
				return;
			}
			break;
		}

		if (auto const* identifier = dynamic_cast<Identifier const*>(_expression))
			if (auto const* declaration = identifier->annotation().referencedDeclaration)
				if (auto found = m_parameterIndexById.find(declaration->id());
					found != m_parameterIndexById.end())
					_out.insert(found->second);
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
		if (facts.count(function.id()))
			return;
		auto [it, _] = facts.emplace(function.id(), NodeFacts{});
		auto& node = it->second;
		DirectMutationScanner scanner(_mostDerived, function, node);
		// Modifier-reference write-through is a separate lowering concern: the
		// current modifier chain copies modifier arguments into locals and does
		// not thread those locals back. Summarize the function body itself here;
		// once modifier arguments are true aliases their effects can be added as
		// ordinary edges without changing this fixed-point representation.
		if (function.isImplemented())
			function.body().accept(scanner);
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
				if (target == summaries.end())
					continue;
				for (auto const& [targetParam, callerParam]: edge.parameterMap)
					if (target->second.mutates(targetParam))
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

ParameterMutationSummary const* ProgramAnalysis::parameterMutationsForCall(
	ContractDefinition const* _mostDerived,
	int64_t _callerCallableId,
	FunctionCall const& _call) const
{
	FunctionDefinition const* caller = nullptr;
	if (auto found = functionDeclarations.find(_callerCallableId);
		found != functionDeclarations.end())
		caller = found->second;
	auto const* target = resolveCallTarget(_mostDerived, caller, _call);
	return target ? &parameterMutations(_mostDerived, *target) : nullptr;
}

} // namespace puyasol::builder

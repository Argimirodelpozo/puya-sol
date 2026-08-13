#include "builder/ProgramAnalysis.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>
#include <libsolidity/ast/Types.h>
#include <libsolidity/interface/CompilerStack.h>

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

} // namespace

ProgramAnalysis ProgramAnalysis::analyze(
	CompilerStack& _compiler,
	bool _evmStorageLayout)
{
	ProgramAnalysis result;

	std::set<Type const*> seen;
	for (auto const& sourceName: _compiler.sourceNames())
		for (auto const* contract:
			ASTNode::filteredNodes<ContractDefinition>(_compiler.ast(sourceName).nodes()))
			for (auto const* stateVar: contract->stateVariables())
				collectMappingValueStructs(
					stateVar->type(), result.boxKeyedStructs, seen);

	forEachFunction(_compiler, [&](FunctionDefinition const* function,
		ContractDefinition const* contract) {
		if (!function || !contract || contract->isLibrary())
			return;
		for (auto const& param: function->parameters())
			if (param->referenceLocation() == VariableDeclaration::Location::Storage)
				if (auto const* structure = dynamic_cast<StructType const*>(param->type()))
					result.refPassedStructs.insert(structure->structDefinition().id());
	});

	struct ReassignmentWalker: ASTConstVisitor
	{
		std::set<int64_t>& out;
		explicit ReassignmentWalker(std::set<int64_t>& _out): out(_out) {}
		bool visit(Assignment const& _assignment) override
		{
			if (auto const* identifier =
				dynamic_cast<Identifier const*>(&_assignment.leftHandSide()))
				if (auto const* declaration = dynamic_cast<VariableDeclaration const*>(
						identifier->annotation().referencedDeclaration))
					if (declaration->referenceLocation()
						== VariableDeclaration::Location::Memory)
						out.insert(declaration->id());
			return true;
		}
	} reassignmentWalker(result.reassignedMemoryLocals);

	forEachFunction(_compiler, [&](FunctionDefinition const* function,
		ContractDefinition const*) {
		if (function && function->isImplemented())
			function->body().accept(reassignmentWalker);
	});

	if (!_evmStorageLayout)
	{
		struct OffsetWalker: ASTConstVisitor
		{
			std::set<int64_t>& out;
			explicit OffsetWalker(std::set<int64_t>& _out): out(_out) {}

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

			bool visit(FunctionCall const& call) override
			{
				Declaration const* declaration = nullptr;
				if (auto const* identifier = dynamic_cast<Identifier const*>(&call.expression()))
					declaration = identifier->annotation().referencedDeclaration;
				else if (auto const* member = dynamic_cast<MemberAccess const*>(&call.expression()))
					declaration = member->annotation().referencedDeclaration;
				auto const* function = dynamic_cast<FunctionDefinition const*>(declaration);
				if (!function)
					return true;

				auto arguments = call.sortedArguments();
				auto const& params = function->parameters();
				size_t shift = 0;
				if (params.size() == arguments.size())
					shift = 0;
				else if (params.size() == arguments.size() + 1)
					shift = 1;
				else
					return true;
				for (size_t i = 0; i < arguments.size(); ++i)
				{
					auto paramIndex = i + shift;
					if (paramIndex < params.size()
						&& isArrayElementStructRef(arguments[i].get())
						&& params[paramIndex]->referenceLocation()
							== VariableDeclaration::Location::Storage)
						out.insert(params[paramIndex]->id());
				}
				return true;
			}
		} offsetWalker(result.structRefOffsetParams);

		forEachFunction(_compiler, [&](FunctionDefinition const* function,
			ContractDefinition const*) {
			if (function && function->isImplemented())
				function->body().accept(offsetWalker);
		});
	}

	return result;
}

} // namespace puyasol::builder

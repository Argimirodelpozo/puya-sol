#pragma once

#include "awst/Node.h"
#include "awst/NameGen.h"

#include <cassert>

namespace puyasol::awst
{

/// Decompose a value without mutating its shared AST. With a statement sink,
/// snapshot the whole tuple before component conversions emit any effects.
/// Without one, all projections share an expression-local evaluation.
inline std::vector<std::shared_ptr<Expression>> tupleItems(
	std::shared_ptr<Expression> value, SourceLocation const& loc,
	std::vector<std::shared_ptr<Statement>>* prepend = nullptr)
{
	auto const* type = dynamic_cast<WTuple const*>(value->wtype);
	assert(type);
	if (!prepend)
		if (auto const* literal = dynamic_cast<TupleExpression const*>(value.get()))
			return literal->items;
	if (prepend)
	{
		auto temp = makeVarExpression("__tuple_"
			+ std::to_string(NameGen::next("TupleValue")), type, loc);
		prepend->push_back(makeAssignmentStatement(temp, std::move(value), loc));
		value = std::move(temp);
	}
	else
		value = makeEvalOnce(std::move(value), loc);
	std::vector<std::shared_ptr<Expression>> items;
	for (size_t i = 0; i < type->types().size(); ++i)
		items.push_back(makeTupleItem(value, static_cast<int>(i), type->types()[i], loc));
	return items;
}

} // namespace puyasol::awst

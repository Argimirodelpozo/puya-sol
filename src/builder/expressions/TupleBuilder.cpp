/// @file TupleBuilder.cpp
/// Handles tuple expressions, function call options, and elementary type name expressions.

#include "builder/expressions/ExpressionBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/TypeProvider.h>

#include <algorithm>
#include <sstream>

namespace puyasol::builder
{

bool ExpressionBuilder::visit(solidity::frontend::TupleExpression const& _node)
{
	auto loc = makeLoc(_node.location());

	// Inline array literals: [val1, val2, ...] → NewArray
	// Must check this before the single-element parenthesization check
	if (_node.isInlineArray())
	{
		auto* wtype = m_typeMapper.map(_node.annotation().type);
		auto* elementType = awst::WType::uint64Type();
		if (auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(wtype))
			elementType = refArr->elementType();

		auto e = std::make_shared<awst::NewArray>();
		e->sourceLocation = loc;
		e->wtype = wtype;
		for (auto const& comp: _node.components())
		{
			if (comp)
			{
				auto val = build(*comp);
				val = implicitNumericCast(std::move(val), elementType, loc);
				e->values.push_back(std::move(val));
			}
		}
		push(e);
		return false;
	}

	if (_node.components().size() == 1 && _node.components()[0])
	{
		// Single-element tuple is just parenthesization
		push(build(*_node.components()[0]));
		return false;
	}

	auto e = std::make_shared<awst::TupleExpression>();
	e->sourceLocation = loc;
	std::vector<awst::WType const*> types;
	for (auto const& comp: _node.components())
	{
		if (comp)
		{
			auto translated = build(*comp);
			types.push_back(translated->wtype);
			e->items.push_back(std::move(translated));
		}
	}
	e->wtype = new awst::WTuple(types); // TODO: memory management
	push(e);
	return false;
}

bool ExpressionBuilder::visit(solidity::frontend::FunctionCallOptions const& _node)
{
	auto loc = makeLoc(_node.location());
	auto const& innerExpr = _node.expression();

	// Check for .call{value: X} → payment inner transaction
	if (auto const* innerMember =
			dynamic_cast<solidity::frontend::MemberAccess const*>(&innerExpr))
	{
		if (innerMember->memberName() == "call" || innerMember->memberName() == "send")
		{
			// Extract value option
			std::shared_ptr<awst::Expression> valueAmount;
			auto const& optNames = _node.names();
			auto optValues = _node.options();
			for (size_t i = 0; i < optNames.size(); ++i)
			{
				if (*optNames[i] == "value" && i < optValues.size())
				{
					valueAmount = build(*optValues[i]);
					valueAmount = implicitNumericCast(
						std::move(valueAmount), awst::WType::uint64Type(), loc
					);
					break;
				}
			}

			if (valueAmount)
			{
				auto receiver = build(innerMember->expression());

				std::map<std::string, std::shared_ptr<awst::Expression>> fields;
				fields["Receiver"] = std::move(receiver);
				fields["Amount"] = std::move(valueAmount);

				auto create = buildCreateInnerTransaction(
					TxnTypePay, std::move(fields), loc
				);
				push(buildSubmitAndReturn(
					std::move(create), awst::WType::voidType(), loc
				));
				return false;
			}
		}
	}

	// Non-call options: translate base expression and warn
	Logger::instance().warning(
		"function call options {value:, gas:} ignored on Algorand", loc
	);
	push(build(innerExpr));
	return false;
}

bool ExpressionBuilder::visit(solidity::frontend::ElementaryTypeNameExpression const& _node)
{
	// Type name used as expression (e.g., address(0))
	auto vc = std::make_shared<awst::VoidConstant>();
	vc->sourceLocation = makeLoc(_node.location());
	vc->wtype = awst::WType::voidType();
	push(vc);
	return false;
}


} // namespace puyasol::builder

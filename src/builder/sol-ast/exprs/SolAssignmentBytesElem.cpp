/// @file SolAssignmentBytesElem.cpp — handleBytesElementAssignment: `bytesVar[i] = byteValue`
#include "builder/sol-ast/exprs/SolAssignment.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;
using Token = solidity::frontend::Token;

std::shared_ptr<awst::Expression> SolAssignment::handleBytesElementAssignment(
	awst::IndexExpression const* _indexExpr,
	std::shared_ptr<awst::Expression> _value)
{
	Token op = m_assignment.assignmentOperator();

	if (op != Token::Assign)
	{
		// Reuse built index for current-value read; rebuilding re-evaluates a
		// side-effecting index (e.g. `b[i++] |= v` gave b[1] into b[0], i==2).
		auto currentValue = awst::makeIndexExpression(
			_indexExpr->base, _indexExpr->index, _indexExpr->wtype, m_loc);
		auto* solType = m_assignment.leftHandSide().annotation().type;
		auto builderResult = eb::AssignmentHelper::tryComputeCompoundValue(
			m_ctx, op, solType, currentValue, _value, m_loc);
		if (builderResult)
			_value = std::move(builderResult);
		else
			_value = m_ctx.buildBinaryOp(op, std::move(currentValue), std::move(_value),
				_indexExpr->wtype, m_loc);
	}

	// Coerce value to single byte
	if (_value->wtype == awst::WType::uint64Type())
	{
		auto itob = awst::makeItob(std::move(_value), m_loc);
		_value = awst::makeExtract(std::move(itob), 7, 1, m_loc);
	}
	else if (_value->wtype && _value->wtype->kind() == awst::WTypeKind::Bytes
		&& _value->wtype != awst::WType::bytesType())
	{
		auto cast = awst::makeAsBytes(std::move(_value), m_loc);
		_value = std::move(cast);
	}
	_value = builder::TypeCoercion::stringToBytes(std::move(_value), m_loc);

	auto replace = awst::makeIntrinsicCall("replace3", _indexExpr->base->wtype, m_loc);
	replace->stackArgs.push_back(_indexExpr->base);
	replace->stackArgs.push_back(_indexExpr->index);
	replace->stackArgs.push_back(std::move(_value));

	// For `bytes(x)[i] = …` the IndexExpression base is a ReinterpretCast;
	// unwrap to give puya a plain lvalue, and align target/value wtypes (string↔bytes).
	auto target = _indexExpr->base;
	std::shared_ptr<awst::Expression> replaceValue = replace;
	while (auto const* cast = dynamic_cast<awst::ReinterpretCast const*>(target.get()))
		target = cast->expr;

	// `s.b[i] = v` where s.b is bytes (struct holds it as ARC4 byte[]):
	// ARC4Decode(FieldExpr) isn't an lvalue in puya — route through NewStruct write-back.
	if (auto const* decode = dynamic_cast<awst::ARC4Decode const*>(target.get()))
	{
		if (auto const* fe = dynamic_cast<awst::FieldExpression const*>(decode->value.get()))
		{
			auto const* structType = dynamic_cast<awst::ARC4Struct const*>(fe->base->wtype);
			if (!structType)
				if (auto const* sg = dynamic_cast<awst::StateGet const*>(fe->base.get()))
					structType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);
			if (structType)
				return buildStructFieldBytesWrite(fe, structType, std::move(replaceValue));
		}
	}

	if (target->wtype != replaceValue->wtype)
	{
		auto adaptCast = awst::makeReinterpretCast(std::move(replaceValue), target->wtype, m_loc);
		replaceValue = std::move(adaptCast);
	}

	return awst::makeAssignmentExpression(target, std::move(replaceValue), m_loc);
}

} // namespace puyasol::builder::sol_ast

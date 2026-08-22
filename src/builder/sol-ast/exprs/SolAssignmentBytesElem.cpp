/// @file SolAssignmentBytesElem.cpp — handleBytesElementAssignment: `bytesVar[i] = byteValue`
#include "builder/sol-ast/exprs/SolAssignment.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "awst/NameGen.h"

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
	_value = builder::TypeCoercion::coerceForAssignment(
		std::move(_value), _indexExpr->wtype, m_loc);

	// The value of `b[i] = x` is x (one byte), not the whole updated byte
	// array.  Pin x because it feeds both replace3 and the enclosing expression,
	// then emit the actual container write as an ordered pre-effect.
	//
	// The pin carries the wtype the coercion above actually produced, NOT
	// `_indexExpr->wtype`. For a `bytes` state var those differ: indexing maps
	// to `byte[1]` while the coerced value stays plain `bytes`, so declaring the
	// temp at the index type made every `x[i] = v` on a storage `bytes` fail
	// with puya "assignment target type differs from expression value type".
	// replace3 wants the raw byte anyway.
	auto const* pinnedType = _value->wtype ? _value->wtype : _indexExpr->wtype;
	std::string valueName = "__byte_assign_"
		+ std::to_string(awst::NameGen::next("SolAssignment.byteResult"));
	auto valueRead = [&]() {
		return awst::makeVarExpression(valueName, pinnedType, m_loc);
	};
	m_ctx.queuePreEffect(awst::makeAssignmentStatement(
		valueRead(), std::move(_value), m_loc));

	auto replace = awst::makeIntrinsicCall("replace3", _indexExpr->base->wtype, m_loc);
	replace->stackArgs.push_back(_indexExpr->base);
	replace->stackArgs.push_back(_indexExpr->index);
	replace->stackArgs.push_back(valueRead());

	// For `bytes(x)[i] = …` the IndexExpression base is a ReinterpretCast;
	// unwrap to give puya a plain lvalue, and align target/value wtypes (string↔bytes).
	// A state var also arrives wrapped in the read-with-default StateGet, which
	// is a READ form and not in puya's closed Lvalue union — leaving it as the
	// assignment target made the backend fail to deserialize the program at all
	// ("deserialization failed: 'StateGet'"). The writable lvalue is its field.
	// Both wrappers can nest (`bytes(x)` over a string state var), so peel until
	// neither matches rather than once each.
	auto target = _indexExpr->base;
	std::shared_ptr<awst::Expression> replaceValue = replace;
	for (bool peeled = true; peeled; )
	{
		peeled = false;
		while (auto const* cast = dynamic_cast<awst::ReinterpretCast const*>(target.get()))
		{
			target = cast->expr;
			peeled = true;
		}
		if (auto const* get = dynamic_cast<awst::StateGet const*>(target.get());
			get && get->field)
		{
			target = get->field;
			peeled = true;
		}
	}

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
			{
				auto write = buildStructFieldBytesWrite(
					fe, structType, std::move(replaceValue));
				m_ctx.queuePreEffect(
					awst::makeExpressionStatement(std::move(write), m_loc));
				return valueRead();
			}
		}
	}

	if (target->wtype != replaceValue->wtype)
	{
		auto adaptCast = awst::makeReinterpretCast(std::move(replaceValue), target->wtype, m_loc);
		replaceValue = std::move(adaptCast);
	}

	auto write = awst::makeAssignmentExpression(
		std::move(target), std::move(replaceValue), m_loc);
	m_ctx.queuePreEffect(awst::makeExpressionStatement(std::move(write), m_loc));
	return valueRead();
}

} // namespace puyasol::builder::sol_ast

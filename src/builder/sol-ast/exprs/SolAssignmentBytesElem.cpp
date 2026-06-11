/// @file SolAssignmentBytesElem.cpp
/// Bytes-element assignment translation extracted from SolAssignmentHandlers.cpp:
///   - handleBytesElementAssignment: `bytesVar[i] = byteValue`
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
		// Reuse the already-built index access for the current-value read.
		// Rebuilding the LHS re-runs a side-effecting index — `b[i++] |= v` would
		// bump `i` twice, reading one element and writing another (verified:
		// b[0]=0x01 -> 0x12 from b[1], i==2). The index node is already the
		// hoisted temp, so reusing base+index evaluates the index exactly once.
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

	// AssignmentExpression.target must be an Lvalue (VarExpression,
	// FieldExpression, IndexExpression, TupleExpression, or a storage
	// expression). For `bytes(x)[i] = …` the IndexExpression base is a
	// ReinterpretCast wrapping the actual storage expression; unwrap it
	// so puya sees a plain lvalue, and adapt the target/value wtype to
	// match the underlying storage type (string ↔ bytes).
	auto target = _indexExpr->base;
	std::shared_ptr<awst::Expression> replaceValue = replace;
	while (auto const* cast = dynamic_cast<awst::ReinterpretCast const*>(target.get()))
		target = cast->expr;

	// Nested bytes field: `s.b[i] = v` where `s.b` is `bytes` but the struct
	// holds it as an ARC4 byte[]. The target here is ARC4Decode(FieldExpr)
	// which puya rejects as an lvalue — route through a NewStruct write-back.
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

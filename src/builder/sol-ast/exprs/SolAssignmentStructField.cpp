/// @file SolAssignmentStructField.cpp — handleStructFieldAssignment + buildStructFieldBytesWrite
#include "builder/sol-ast/exprs/SolAssignment.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;
using Token = solidity::frontend::Token;

std::shared_ptr<awst::Expression> SolAssignment::buildStructFieldBytesWrite(
	awst::FieldExpression const* _fieldExpr,
	awst::ARC4Struct const* _structType,
	std::shared_ptr<awst::Expression> _newBytes)
{
	// Shared struct-field COW store (encodes bytes → ARC4 byte[], with-default
	// sibling reads, chain rebuild, writable target, lazy-root-box ensure).
	auto store = eb::AssignmentHelper::buildStructFieldCowStore(
		m_ctx, _fieldExpr, _structType, std::move(_newBytes), m_loc);
	return awst::makeAssignmentExpression(
		std::move(store.target), std::move(store.value), m_loc);
}

std::shared_ptr<awst::Expression> SolAssignment::handleStructFieldAssignment(
	awst::FieldExpression const* _fieldExpr,
	std::shared_ptr<awst::Expression> _value,
	std::shared_ptr<awst::Expression> _unwrappedTarget,
	bool _emitAsStatement)
{
	auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(_fieldExpr->base->wtype);
	if (!arc4StructType)
		if (auto const* sg = dynamic_cast<awst::StateGet const*>(_fieldExpr->base.get()))
			arc4StructType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);
	if (!arc4StructType) return nullptr;

	Token op = m_assignment.assignmentOperator();
	auto base = _fieldExpr->base;
	std::string fieldName = _fieldExpr->name;

	base = awst::unwrapStateGet(std::move(base));

	auto readBase = base;
	if (dynamic_cast<awst::BoxValueExpression const*>(base.get()))
		readBase = builder::StorageMapper::makeStateGetWithDefault(base, base->wtype, m_loc);

	if (op != Token::Assign)
	{
		auto currentField = awst::makeFieldExpression(readBase, fieldName, _fieldExpr->wtype, m_loc);
		auto decoded = awst::makeARC4Decode(std::move(currentField), m_ctx.typeMapper.map(m_assignment.leftHandSide().annotation().type), m_loc);
		auto* solType = m_assignment.leftHandSide().annotation().type;
		auto builderResult = eb::AssignmentHelper::tryComputeCompoundValue(
			m_ctx, op, solType, decoded, _value, m_loc);
		if (builderResult)
			_value = std::move(builderResult);
		else
			_value = m_ctx.buildBinaryOp(op, std::move(decoded), std::move(_value),
				decoded->wtype, m_loc);
	}

	// ARC4Encode the value
	awst::WType const* arc4FieldType = awst::structFieldType(arc4StructType, fieldName);
	if (arc4FieldType && _value->wtype != arc4FieldType)
	{
		// Coerce to native type first (e.g. uint64 "2" → BytesConstant for bytes1 fields)
		auto* nativeType = m_ctx.typeMapper.map(m_assignment.leftHandSide().annotation().type);
		if (nativeType && _value->wtype != nativeType)
			_value = builder::TypeCoercion::coerceForAssignment(std::move(_value), nativeType, m_loc);

		// Signed sub-word → wider-signed implicit widen (`s.f = someInt8;` f:int16): re-extend
		// from the RHS width before encoding (plain `=` only; compound already computed a typed value).
		if (op == Token::Assign)
			_value = builder::TypeCoercion::signExtendSignedWiden(
				std::move(_value), m_assignment.rightHandSide().annotation().type,
				m_assignment.leftHandSide().annotation().type, m_loc);

		auto encode = awst::makeARC4Encode(std::move(_value), arc4FieldType, m_loc);
		_value = std::move(encode);
	}

	// Shared struct-field COW store (encode no-ops — value already encoded above).
	auto store = eb::AssignmentHelper::buildStructFieldCowStore(
		m_ctx, _fieldExpr, arc4StructType, std::move(_value), m_loc);
	auto& fieldChain = store.fieldChain;

	auto e = awst::makeAssignmentExpression(
		std::move(store.target), std::move(store.value), m_loc);

	// Tuple-destructure context: queue the COW store as a statement and return
	// a truthy sentinel (field-value decode uses the single assignment's result
	// type, which is meaningless here).
	if (_emitAsStatement)
	{
		m_ctx.queuePostExpression(e, m_loc);
		return e;
	}

	if (arc4FieldType)
	{
		std::shared_ptr<awst::Expression> extractBase = std::move(e);
		for (auto it = fieldChain.rbegin(); it != fieldChain.rend(); ++it)
		{
			auto fe = awst::makeFieldExpression(std::move(extractBase), it->first, it->second, m_loc);
			extractBase = std::move(fe);
		}
		auto fieldExtract = awst::makeFieldExpression(std::move(extractBase), fieldName, arc4FieldType, m_loc);
		auto decode = awst::makeARC4Decode(std::move(fieldExtract), m_ctx.typeMapper.map(m_assignment.annotation().type), m_loc);
		return decode;
	}
	return e;
}

} // namespace puyasol::builder::sol_ast

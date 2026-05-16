/// @file SolAssignmentStructField.cpp
/// Struct-field-assignment translation extracted from SolAssignmentHandlers.cpp:
///   - handleStructFieldAssignment: `s.f = v` write-through to ARC4Struct
///   - buildStructFieldBytesWrite: copy-on-write helper for bytes-typed field writes
#include "builder/sol-ast/exprs/SolAssignment.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

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
	auto base = _fieldExpr->base;
	std::string fieldName = _fieldExpr->name;

	// Unwrap StateGet for write targets (StateGet is not an Lvalue)
	if (auto const* sg = dynamic_cast<awst::StateGet const*>(base.get()))
		base = sg->field;

	// Wrap bytes → ARC4 byte[] encoding (prepends length prefix in puya)
	awst::WType const* arc4FieldType = nullptr;
	for (auto const& [fname, ftype]: _structType->fields())
		if (fname == fieldName) { arc4FieldType = ftype; break; }

	std::shared_ptr<awst::Expression> encodedValue = std::move(_newBytes);
	if (arc4FieldType && encodedValue->wtype != arc4FieldType)
	{
		auto encode = awst::makeARC4Encode(std::move(encodedValue), arc4FieldType, m_loc);
		encodedValue = std::move(encode);
	}

	// Build NewStruct with replaced field
	auto newStruct = awst::makeNewStruct(_structType, m_loc);
	for (auto const& [fname, ftype]: _structType->fields())
	{
		if (fname == fieldName)
			newStruct->values[fname] = encodedValue;
		else
		{
			auto f = awst::makeFieldExpression(base, fname, ftype, m_loc);
			newStruct->values[fname] = std::move(f);
		}
	}

	// Walk outer FieldExpression chain, rebuilding NewStructs (copy-on-write
	// for nested `outer.inner.b[i] = v` patterns).
	auto cow = eb::AssignmentHelper::rebuildArc4StructChainCOW(
		m_ctx, base, std::move(newStruct), m_loc);
	auto assignTarget = std::move(cow.assignTarget);
	auto assignValue = std::move(cow.assignValue);

	// Normalize the final write target — strip any StateGet / ARC4Decode
	// wrappers in the chain that survived the copy-on-write rebuild above.
	assignTarget = awst::makeWritableTarget(std::move(assignTarget));

	return awst::makeAssignmentExpression(
		std::move(assignTarget), std::move(assignValue), m_loc);
}

std::shared_ptr<awst::Expression> SolAssignment::handleStructFieldAssignment(
	awst::FieldExpression const* _fieldExpr,
	std::shared_ptr<awst::Expression> _value,
	std::shared_ptr<awst::Expression> _unwrappedTarget)
{
	auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(_fieldExpr->base->wtype);
	if (!arc4StructType)
		if (auto const* sg = dynamic_cast<awst::StateGet const*>(_fieldExpr->base.get()))
			arc4StructType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);
	if (!arc4StructType) return nullptr;

	Token op = m_assignment.assignmentOperator();
	auto base = _fieldExpr->base;
	std::string fieldName = _fieldExpr->name;

	if (auto const* sg = dynamic_cast<awst::StateGet const*>(base.get()))
		base = sg->field;

	auto readBase = base;
	if (dynamic_cast<awst::BoxValueExpression const*>(base.get()))
	{
		auto stateGet = awst::makeStateGet(base, builder::StorageMapper::makeDefaultValue(base->wtype, m_loc), base->wtype, m_loc);
		readBase = stateGet;
	}

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
	awst::WType const* arc4FieldType = nullptr;
	for (auto const& [fname, ftype]: arc4StructType->fields())
		if (fname == fieldName) { arc4FieldType = ftype; break; }
	if (arc4FieldType && _value->wtype != arc4FieldType)
	{
		// Coerce value to native type before ARC4 encoding
		// e.g., IntegerConstant(uint64, "2") → BytesConstant(bytes[1]) for bytes1 fields
		auto* nativeType = m_ctx.typeMapper.map(m_assignment.leftHandSide().annotation().type);
		if (nativeType && _value->wtype != nativeType)
			_value = builder::TypeCoercion::coerceForAssignment(std::move(_value), nativeType, m_loc);

		auto encode = awst::makeARC4Encode(std::move(_value), arc4FieldType, m_loc);
		_value = std::move(encode);
	}

	// Build NewStruct with copy-on-write
	auto newStruct = awst::makeNewStruct(arc4StructType, m_loc);
	for (auto const& [fname, ftype]: arc4StructType->fields())
	{
		if (fname == fieldName)
			newStruct->values[fname] = std::move(_value);
		else
		{
			auto field = awst::makeFieldExpression(readBase, fname, ftype, m_loc);
			newStruct->values[fname] = std::move(field);
		}
	}

	// Recursive copy-on-write for nested structs.
	auto cow = eb::AssignmentHelper::rebuildArc4StructChainCOW(
		m_ctx, std::move(base), std::move(newStruct), m_loc);
	auto assignTarget2 = std::move(cow.assignTarget);
	auto assignValue2 = std::move(cow.assignValue);
	auto& fieldChain = cow.fieldChain;

	// If the outermost write target is an IndexExpression whose base is
	// wrapped in StateGet (e.g. `data[2].x = v` on a box-stored static
	// array struct), unwrap the StateGet so the assignment target carries
	// a raw BoxValue — puya rejects StateGet nested inside an Lvalue.
	if (auto const* idx = dynamic_cast<awst::IndexExpression const*>(assignTarget2.get()))
	{
		if (auto const* sg = dynamic_cast<awst::StateGet const*>(idx->base.get()))
		{
			auto newIdx = awst::makeIndexExpression(sg->field, idx->index, idx->wtype, idx->sourceLocation);
			assignTarget2 = std::move(newIdx);
		}
	}

	// Mapping-entry partial write: `n[k][i].f = v` where n is
	// `mapping(K => T[N])` lowers to box_replace on the per-entry key.
	// The per-entry box is created lazily. Emit box_create as a pending
	// pre-statement so the box exists before box_replace runs.
	if (auto const* idx = dynamic_cast<awst::IndexExpression const*>(assignTarget2.get()))
	{
		if (auto const* bv = dynamic_cast<awst::BoxValueExpression const*>(idx->base.get()))
		{
			if (bv->key && dynamic_cast<awst::BoxPrefixedKeyExpression const*>(bv->key.get()))
			{
				bool dynamicArc4 = false;
				if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(bv->wtype))
					dynamicArc4 = builder::TypeCoercion::arc4IsDynamic(sa);
				if (dynamicArc4)
				{
					if (auto enc = builder::TypeCoercion::arc4DefaultEncoding(bv->wtype))
					{
						if (enc->size() > 0 && enc->size() <= 32768)
						{
							auto putCall = awst::makeIntrinsicCall(
								"box_put", awst::WType::voidType(), m_loc);
							putCall->stackArgs.push_back(bv->key);
							putCall->stackArgs.push_back(awst::makeBytesConstant(
								std::move(*enc), m_loc));
							m_ctx.queuePreStmt(std::move(putCall), m_loc);
						}
					}
				}
				else
				{
					uint64_t totalSize = 0;
					if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(bv->wtype))
					{
						int elemSize = builder::TypeCoercion::computeEncodedElementSize(sa->elementType());
						if (elemSize > 0 && sa->arraySize() > 0)
							totalSize = static_cast<uint64_t>(elemSize) * static_cast<uint64_t>(sa->arraySize());
					}
					if (totalSize > 0 && totalSize <= 32768)
					{
						auto createCall = awst::makeIntrinsicCall(
							"box_create", awst::WType::boolType(), m_loc);
						createCall->stackArgs.push_back(bv->key);
						createCall->stackArgs.push_back(
							awst::makeIntegerConstant(totalSize, m_loc));
						m_ctx.queuePreStmt(std::move(createCall), m_loc);
					}
				}
			}
		}
	}

	auto e = awst::makeAssignmentExpression(
		std::move(assignTarget2), std::move(assignValue2), m_loc);

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

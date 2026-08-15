/// @file SolArrayBuilder.cpp
/// Solidity typed array builder — handles index access and .length.

#include "builder/sol-eb/SolArrayBuilder.h"
#include "awst/NameGen.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::eb
{

awst::WType const* SolArrayBuilder::elementType() const
{
	auto* baseWType = wtype();
	if (!baseWType)
		return nullptr;

	switch (baseWType->kind())
	{
	case awst::WTypeKind::ReferenceArray:
		return static_cast<awst::ReferenceArray const*>(baseWType)->elementType();
	case awst::WTypeKind::ARC4DynamicArray:
		return static_cast<awst::ARC4DynamicArray const*>(baseWType)->elementType();
	case awst::WTypeKind::ARC4StaticArray:
		return static_cast<awst::ARC4StaticArray const*>(baseWType)->elementType();
	default:
		return nullptr;
	}
}

std::unique_ptr<InstanceBuilder> SolArrayBuilder::index(
	InstanceBuilder& _idx, awst::SourceLocation const& _loc)
{
	auto base = resolve();
	auto index = _idx.resolve();

	if (index->wtype == awst::WType::biguintType())
		index = TypeCoercion::checkedIndexToUint64(m_ctx.preEffects(), std::move(index), _loc);

	auto* elemType = elementType();
	if (!elemType)
		return nullptr;

	// CALLDATA arrays kept as ARC4 VALUES (asm-mode functions skip the native
	// decode, so `s.m[i]` indexes the raw encoding): puya's IndexExpression
	// lowering has NO length check — it relies on the physical extract failing,
	// and an EMPTY array inside a larger encoding (`s.m[0]` with s = ([]))
	// reads adjacent struct bytes instead of reverting (EVM Panic 0x32).
	// Assert idx < the uint16 length prefix. Calldata is never an lvalue, so
	// the eval-once base wrap is safe. Found by the night-3 stmt-del mutant on
	// viaYul/dirty_calldata_struct (the deletion was incidental — the empty
	// inner array was the trigger).
	if (wtype() && wtype()->kind() == awst::WTypeKind::ARC4DynamicArray
		&& m_arrayType
		&& m_arrayType->dataStoredIn(solidity::frontend::DataLocation::CallData))
	{
		base = awst::makeEvalOnce(std::move(base), _loc);
		std::string tmpName = "__sol_cdix_" + std::to_string(
			awst::NameGen::next("SolArrayBuilder.cdIndex"));
		auto tmpVar = [&]() {
			return awst::makeVarExpression(tmpName, awst::WType::uint64Type(), _loc);
		};
		m_ctx.preEffects().push_back(
			awst::makeAssignmentStatement(tmpVar(), std::move(index), _loc));
		auto len = awst::makeExtractUInt16(
			awst::makeReinterpretCast(base, awst::WType::bytesType(), _loc),
			awst::makeZero(_loc), _loc);
		auto cmp = awst::makeNumericCompare(
			tmpVar(), awst::NumericComparison::Lt, std::move(len), _loc);
		m_ctx.preEffects().push_back(awst::makeExpressionStatement(
			awst::makeAssert(std::move(cmp), _loc, "array index out of bounds"), _loc));
		index = tmpVar();
	}

	auto e = awst::makeIndexExpression(std::move(base), std::move(index), elemType, _loc);

	auto* expectedType = m_ctx.typeMapper.map(m_arrayType->baseType());
	// NB: ARC4Struct is intentionally NOT decoded — a struct element is already the
	// native form (field access reads it directly). For non-recursive struct arrays
	// elemType == expectedType so this never fired; for a recursive struct array the
	// element is a recursion-projection (elemType=`S__rec` != full S) and decoding it
	// to the full struct is both invalid in an lvalue (`s.x[i].v = …`) and semantically
	// wrong — the projection carries the fields needed for the access.
	bool needsDecode = !awst::structurallyEquivalent(elemType, expectedType)
		&& (elemType->kind() == awst::WTypeKind::ARC4StaticArray
			|| elemType->kind() == awst::WTypeKind::ARC4UIntN
			|| elemType->kind() == awst::WTypeKind::ARC4DynamicArray
			// arc4.bool is an ARC4BasicWType of kind `Basic` (same kind as native
			// `bool`), so the kind checks above miss it — a `bool[]` element then
			// stays arc4.bool and, used directly as a condition (`if (flags[i])`),
			// trips the puya backend ("IfElse.condition expected bool"). Decode it
			// to native bool. Found via OZ MerkleProof.multiProofVerify.
			|| elemType == awst::WType::arc4BoolType());

	std::shared_ptr<awst::Expression> result = std::move(e);
	bool signExtendElem = false;
	if (needsDecode)
	{
		result = awst::makeARC4Decode(std::move(result), expectedType, _loc);
		// Signed sub-256 (e.g. int128): defer sign-extension to resolve() so the
		// bare decode stays a valid lvalue for `a[i] = x`.
		signExtendElem = true;
	}

	// Enum: panic(0x21) on out-of-range. Spill to local so assert survives DCE.
	if (auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(
			m_arrayType->baseType()))
	{
		unsigned numMembers = enumType->numberOfMembers();
		std::string tmpName = "__enum_idx_" + std::to_string(awst::NameGen::next("SolArrayBuilder.enumCheckCounter"));

		auto tmpVar = awst::makeVarExpression(tmpName, result->wtype, _loc);

		auto assignTmp = awst::makeAssignmentStatement(tmpVar, result, _loc);
		m_ctx.preEffects().push_back(std::move(assignTmp));

		auto cmpLhs = TypeCoercion::implicitNumericCast(
			tmpVar, awst::WType::uint64Type(), _loc);
		auto assertStmt = awst::makeExpressionStatement(
			awst::makeEnumRangeAssert(std::move(cmpLhs), numMembers, _loc, "Enum out of range"), _loc);
		m_ctx.preEffects().push_back(std::move(assertStmt));

		result = tmpVar;
	}

	auto out = std::make_unique<SolArrayBuilder>(m_ctx, m_arrayType, std::move(result));
	if (signExtendElem)
	{
		out->m_signExtendElem = m_arrayType->baseType();
		out->m_signExtendLoc = _loc;
	}
	return out;
}

std::shared_ptr<awst::Expression> SolArrayBuilder::resolve()
{
	// rvalue read: sign-extend a decoded signed sub-256 element to canonical
	// 256-bit (no-op for unsigned / int256 / <=64-bit — see TypeCoercion).
	if (m_signExtendElem)
		return TypeCoercion::signExtendSignedElement(m_expr, m_signExtendElem, m_signExtendLoc);
	return m_expr;
}

std::shared_ptr<awst::Expression> SolArrayBuilder::resolve_lvalue()
{
	// Assignment target: the bare decoded element. Never sign-extend (a
	// CommaExpression is not a valid lvalue).
	return m_expr;
}

std::unique_ptr<NodeBuilder> SolArrayBuilder::member_access(
	std::string const& _name, awst::SourceLocation const& _loc)
{
	if (_name == "length")
	{
		auto base = resolve();
		auto kind = base->wtype ? base->wtype->kind() : awst::WTypeKind::Bytes;
		if (kind == awst::WTypeKind::ReferenceArray
			|| kind == awst::WTypeKind::ARC4StaticArray
			|| kind == awst::WTypeKind::ARC4DynamicArray)
		{
			auto e = awst::makeArrayLength(std::move(base), awst::WType::uint64Type(), _loc);
			return std::make_unique<SolArrayBuilder>(m_ctx, m_arrayType, std::move(e));
		}
		// For other types (bytes): use len intrinsic
		auto len = awst::makeLen(std::move(base), _loc);
		return std::make_unique<SolArrayBuilder>(m_ctx, m_arrayType, std::move(len));
	}

	// .push/.pop/.concat arrive as FunctionCalls → SolArrayMethod.cpp, not here.
	return nullptr;
}

} // namespace puyasol::builder::eb

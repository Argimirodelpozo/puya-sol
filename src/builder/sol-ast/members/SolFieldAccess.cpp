/// @file SolFieldAccess.cpp
/// Struct field access (ARC4Struct, WTuple).
/// Migrated from MemberAccessBuilder.cpp lines 712-754.

#include "builder/sol-ast/members/SolFieldAccess.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolFieldAccess::toAwst()
{
	auto base = buildExpr(baseExpression());
	std::string member = memberName();

	if (base->wtype && base->wtype->kind() == awst::WTypeKind::ARC4Struct)
	{
		auto const* structType = static_cast<awst::ARC4Struct const*>(base->wtype);
		awst::WType const* arc4FieldType = nullptr;
		for (auto const& [fname, ftype]: structType->fields())
			if (fname == member)
			{
				arc4FieldType = ftype;
				break;
			}

		auto field = awst::makeFieldExpression(std::move(base), member, arc4FieldType ? arc4FieldType
			: m_ctx.typeMapper.map(m_memberAccess.annotation().type), m_loc);

		auto* nativeType = m_ctx.typeMapper.map(m_memberAccess.annotation().type);
		if (arc4FieldType && arc4FieldType != nativeType)
		{
			std::shared_ptr<awst::Expression> decode =
				awst::makeARC4Decode(std::move(field), nativeType, m_loc);
			// A packed signed sub-word field (arc4.intN, N<64) decodes
			// (extract + btoi) to the RAW N-bit value, dropping the sign
			// (-60 int24 -> +16777156). Sign-extend to the 64-bit two's-
			// complement so widening / int64 casts / signed arithmetic on the
			// field read see the correct negative value. (No-op for
			// non-negative values; unsigned fields are skipped.)
			//
			// ONLY for rvalue reads: when this member access is an assignment
			// TARGET (willBeWrittenTo), the write-back path
			// (SolAssignment::tryStructOrNamedTupleFieldAssignment) must see the
			// bare ARC4Decode/FieldExpression, so leave it unwrapped.
			if (!m_memberAccess.annotation().willBeWrittenTo)
				if (auto const* fieldInt = dynamic_cast<solidity::frontend::IntegerType const*>(
						m_memberAccess.annotation().type))
				{
					if (fieldInt->isSigned() && fieldInt->numBits() < 64
						&& nativeType == awst::WType::uint64Type())
						decode = TypeCoercion::signExtendToUint64(
							std::move(decode), fieldInt->numBits(), m_loc);
				}
			return decode;
		}
		return field;
	}

	if (base->wtype && base->wtype->kind() == awst::WTypeKind::WTuple)
	{
		auto e = awst::makeFieldExpression(std::move(base), member, m_ctx.typeMapper.map(m_memberAccess.annotation().type), m_loc);
		return e;
	}

	return nullptr;
}

} // namespace puyasol::builder::sol_ast

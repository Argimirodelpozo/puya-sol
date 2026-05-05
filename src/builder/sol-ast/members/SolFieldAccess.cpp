/// @file SolFieldAccess.cpp
/// Struct field access (ARC4Struct, WTuple).
/// Migrated from MemberAccessBuilder.cpp lines 712-754.

#include "builder/sol-ast/members/SolFieldAccess.h"
#include "builder/sol-types/TypeMapper.h"

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
			auto decode = awst::makeARC4Decode(std::move(field), nativeType, m_loc);
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

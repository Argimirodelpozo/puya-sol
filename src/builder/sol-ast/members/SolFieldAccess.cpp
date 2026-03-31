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

		auto field = std::make_shared<awst::FieldExpression>();
		field->sourceLocation = m_loc;
		field->base = std::move(base);
		field->name = member;
		field->wtype = arc4FieldType ? arc4FieldType
			: m_ctx.typeMapper.map(m_memberAccess.annotation().type);

		auto* nativeType = m_ctx.typeMapper.map(m_memberAccess.annotation().type);
		if (arc4FieldType && arc4FieldType != nativeType)
		{
			auto decode = std::make_shared<awst::ARC4Decode>();
			decode->sourceLocation = m_loc;
			decode->wtype = nativeType;
			decode->value = std::move(field);
			return decode;
		}
		return field;
	}

	if (base->wtype && base->wtype->kind() == awst::WTypeKind::WTuple)
	{
		auto e = std::make_shared<awst::FieldExpression>();
		e->sourceLocation = m_loc;
		e->base = std::move(base);
		e->name = member;
		e->wtype = m_ctx.typeMapper.map(m_memberAccess.annotation().type);
		return e;
	}

	return nullptr;
}

} // namespace puyasol::builder::sol_ast

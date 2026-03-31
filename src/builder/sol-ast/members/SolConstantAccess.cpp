/// @file SolConstantAccess.cpp
/// Contract/library constant inlining, event member access, contract member names.
/// Migrated from MemberAccessBuilder.cpp lines 362-380, 557-580.

#include "builder/sol-ast/members/SolConstantAccess.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

std::shared_ptr<awst::Expression> SolConstantAccess::toAwst()
{
	auto const* refDecl = m_memberAccess.annotation().referencedDeclaration;

	// Event member access: L.E → VoidConstant placeholder (for .selector)
	if (dynamic_cast<EventDefinition const*>(refDecl))
	{
		auto vc = std::make_shared<awst::VoidConstant>();
		vc->sourceLocation = m_loc;
		vc->wtype = awst::WType::voidType();
		return vc;
	}

	// Constant inlining: Contract.CONSTANT → inline initializer
	if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(refDecl))
	{
		if (varDecl->isConstant() && varDecl->value())
			return buildExpr(*varDecl->value());
	}

	// Contract member name (e.g., token.transfer in abi.encodeCall context)
	auto const* baseType = baseExpression().annotation().type;
	if (baseType && baseType->category() == Type::Category::Contract)
	{
		std::string member = memberName();
		auto e = std::make_shared<awst::BytesConstant>();
		e->sourceLocation = m_loc;
		e->wtype = awst::WType::bytesType();
		e->encoding = awst::BytesEncoding::Utf8;
		e->value = std::vector<uint8_t>(member.begin(), member.end());
		return e;
	}

	return nullptr;
}

} // namespace puyasol::builder::sol_ast

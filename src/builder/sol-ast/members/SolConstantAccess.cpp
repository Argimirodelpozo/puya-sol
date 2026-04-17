/// @file SolConstantAccess.cpp
/// Contract/library constant inlining, event member access, contract member names.
/// Migrated from MemberAccessBuilder.cpp lines 362-380, 557-580.

#include "builder/sol-ast/members/SolConstantAccess.h"

#include "builder/storage/StorageMapper.h"
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

		// Non-constant state variable: Contract.stateVar → read from storage
		if (varDecl->isStateVariable() && !varDecl->isConstant())
		{
			auto* wtype = m_ctx.typeMapper.map(varDecl->type());
			std::string name = varDecl->name();
			auto kind = builder::StorageMapper::shouldUseBoxStorage(*varDecl)
				? awst::AppStorageKind::Box
				: awst::AppStorageKind::AppGlobal;
			return m_ctx.storageMapper.createStateRead(name, wtype, kind, m_loc);
		}
	}

	// Contract member name (e.g., token.transfer in abi.encodeCall context)
	auto const* baseType = baseExpression().annotation().type;
	if (baseType && baseType->category() == Type::Category::Contract)
	{
		return awst::makeUtf8BytesConstant(memberName(), m_loc);
	}

	return nullptr;
}

} // namespace puyasol::builder::sol_ast

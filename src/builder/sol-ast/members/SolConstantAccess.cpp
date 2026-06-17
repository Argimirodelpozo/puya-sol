/// @file SolConstantAccess.cpp
/// Contract/library constant inlining, event member access, contract member names.
/// Migrated from MemberAccessBuilder.cpp lines 362-380, 557-580.

#include "builder/sol-ast/members/SolConstantAccess.h"

#include "builder/storage/StorageMapper.h"
#include "Logger.h"
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
		auto vc = awst::makeVoidConstant(m_loc);
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

	// Module-aliased contract/library ref (`import "x" as M; M.L`).
	// As a CALL receiver, SolInternalCall dispatches by FunctionDefinition
	// (this value unused). As a VALUE (`address(M.L)`): AVM libraries are
	// inlined as subroutines with no on-chain identity, so stub as address(0)
	// and warn — code branching on `address(L)` diverges from EVM.
	if (auto const* contractDef = dynamic_cast<ContractDefinition const*>(refDecl))
	{
		Logger::instance().warning(
			"`" + contractDef->name() + "` is a "
			+ (contractDef->isLibrary() ? "library" : "contract")
			+ " referenced via module alias; on AVM there is no on-chain "
			"identity for Solidity libraries (calls inline as subroutines), "
			"so `address(" + contractDef->name() + ")` is stubbed as "
			"address(0) — code that branches on the library address will "
			"behave differently than on EVM.",
			m_loc);

		// Evaluate base for side effects (e.g. `((flag=true) ? M : M).D`).
		(void) buildExpr(baseExpression());

		std::vector<unsigned char> zeros(32, 0);
		auto bc = awst::makeBytesConstant(std::move(zeros), m_loc);
		return awst::makeAsAccount(std::move(bc), m_loc);
	}

	return nullptr;
}

} // namespace puyasol::builder::sol_ast

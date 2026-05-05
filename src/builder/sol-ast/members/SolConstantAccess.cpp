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

	// Module-aliased contract reference: `import "x" as M; M.L` where the
	// MemberAccess resolves to a ContractDefinition. When this is a CALL
	// receiver (`M.L.f(...)`), SolInternalCall's last-resort library
	// resolver dispatches by the outer FunctionDefinition and the value
	// we return here is unused. When it's a VALUE (`address(M.L)`), the
	// EVM semantic is the library's deployed address — but on AVM
	// Solidity libraries are inlined / dispatched as subroutines, with
	// no stable on-chain identity, so we don't have an honest answer.
	// Emit a 32-byte zero (so `address(L) == address(0)` evaluates true),
	// and warn loudly that contract code branching on `address(L)` will
	// see different behaviour than on EVM.
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

		// Build the base expression for its side effects (e.g. an
		// AssignmentExpression in `((flag = true) ? M : M).D` would
		// otherwise be silently dropped — the surrounding ternary's
		// SolConditional already emitted the assignment as a pre-pending
		// statement, but visiting the base here lets any nested
		// expression-builder side effects flush too).
		(void) buildExpr(baseExpression());

		std::vector<unsigned char> zeros(32, 0);
		auto bc = awst::makeBytesConstant(std::move(zeros), m_loc);
		return awst::makeReinterpretCast(std::move(bc), awst::WType::accountType(), m_loc);
	}

	return nullptr;
}

} // namespace puyasol::builder::sol_ast

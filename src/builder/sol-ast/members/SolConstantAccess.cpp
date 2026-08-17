/// @file SolConstantAccess.cpp
/// Contract/library constant inlining, event member access, contract member names.
/// Migrated from MemberAccessBuilder.cpp lines 362-380, 557-580.

#include "builder/sol-ast/members/SolConstantAccess.h"

#include "builder/EvmFeaturePolicy.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/storage/EvmLayoutMode.h"
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
			// Slot mode: `C.x` denotes the same slot as the bare `x`, so it must
			// go through the slot space too — the legacy app-global read below
			// would look up a key the constructor never wrote.
			if (m_ctx.typeMapper.profile().evmStorageLayout && !varDecl->immutable()
				&& varDecl->referenceLocation()
					!= VariableDeclaration::Location::Transient)
			{
				bool const bytesLike = EvmSlotLowering::isBytesLike(varDecl->type());
				if ((varDecl->type() && varDecl->type()->isValueType()) || bytesLike)
				{
					EvmSlotLowering low(m_ctx, m_scope, m_loc);
					auto addr = low.resolve(m_memberAccess);
					if (!addr)
						return nullptr;
					return bytesLike ? low.readBytesValue(*addr) : low.readValue(*addr);
				}
			}
			auto* wtype = m_ctx.typeMapper.map(varDecl->type());
			auto binding = m_ctx.storageMapper.physicalBindingFor(*varDecl);
			return m_ctx.storageMapper.createStateRead(
				binding.name, wtype, binding.kind, m_loc);
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
	// (this value unused). As a VALUE (`address(M.L)`), there is no honest AVM
	// identity because libraries lower to subroutines.
	if (auto const* contractDef = dynamic_cast<ContractDefinition const*>(refDecl))
	{
		builder::EvmFeaturePolicy::report(
			builder::EvmFeature::LibraryAddress,
			m_ctx.typeMapper.profile(), m_loc);

		// Evaluate base for side effects (e.g. `((flag=true) ? M : M).D`).
		(void) buildExpr(baseExpression());

		// Type-correct poison value; policy error prevents output.
		std::vector<unsigned char> zeros(32, 0);
		auto bc = awst::makeBytesConstant(std::move(zeros), m_loc);
		return awst::makeAsAccount(std::move(bc), m_loc);
	}

	return nullptr;
}

} // namespace puyasol::builder::sol_ast

#pragma once

#include "builder/ast/SolFunctionCall.h"

namespace puyasol::builder { class ConstructorWirePlan; }

namespace puyasol::builder::sol_ast
{

/// new bytes(N), new T[](N), new Contract(...).
/// Handles object creation expressions.
class SolNewExpression: public SolFunctionCall
{
public:
	using SolFunctionCall::SolFunctionCall;
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	// ── toAwst shape rungs (SolNewExpression.cpp), in dispatch order ────
	std::shared_ptr<awst::Expression> handleNewBytes();
	std::shared_ptr<awst::Expression> allocationSize(uint64_t capacity);
	std::shared_ptr<awst::Expression> handleNewArray();
	/// `new C(...)`: inner appl-create with TemplateVar programs, MBR
	/// funding, and the [pay, __postInit] group when the child defers init.
	std::shared_ptr<awst::Expression> handleNewContract(
		solidity::frontend::ContractType const& _contractType);

	// ── handleNewContract stages, in emission order ─────────────────────
	/// `new C{salt: ...}` (CREATE2) has no AVM equivalent: log the error
	/// (no early exit — the create still lowers).
	void rejectCreate2Salt();
	/// ApprovalProgramPages tuple: TMPL_APPROVAL_<C>_P0/_P1, or the two
	/// runtime slices of the deployer-provisioned "__cp_<C>" box.
	std::shared_ptr<awst::TupleExpression> buildChildApprovalPages(
		std::string const& _childName);
	/// Encode already-evaluated operands for the selected child entry.
	std::shared_ptr<awst::TupleExpression> buildChildArgs(
		ConstructorWirePlan const& wire,
		std::vector<std::shared_ptr<awst::Expression>> values, bool postInit);
	/// Fund the child's escrow with MBR plus any create-time call value.
	void emitChildFunding(
		std::shared_ptr<awst::Expression> const& appId,
		std::shared_ptr<awst::Expression> callValue);
	/// Submit the [pay(value), __postInit(args)] group.
	void emitChildPostInit(
		std::shared_ptr<awst::Expression> appId,
		std::shared_ptr<awst::Expression> args,
		std::shared_ptr<awst::Expression> callValue);
};

} // namespace puyasol::builder::sol_ast

#pragma once

#include "builder/sol-ast/SolFunctionCall.h"

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
	std::shared_ptr<awst::Expression> handleNewString();
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
	/// ApplicationArgs for the create-time ctor (no __postInit): the one EVM
	/// calldata body, or one ARC4-encoded arg per slot — nullptr when the
	/// ARC4 encoding yields no args (the field is then left unset).
	std::shared_ptr<awst::TupleExpression> buildChildCreateArgs(
		solidity::frontend::FunctionDefinition const& _childCtor, bool _childHasPostInit);
	/// Ctor args, each built + numeric-cast to its param wtype and encoded
	/// for the create-path reader or __postInit's router (`_childHasPostInit`).
	std::vector<std::shared_ptr<awst::Expression>> buildEncodedCtorArgs(
		solidity::frontend::FunctionDefinition const* _childCtor, bool _childHasPostInit);
	/// One ctor arg's wire encoding: bytesN / biguint / uint64 / bool /
	/// reference-array shapes; anything else passes through unchanged.
	std::shared_ptr<awst::Expression> encodeCtorArg(
		std::shared_ptr<awst::Expression> _argVal,
		solidity::frontend::Type const* _paramSolType,
		bool _childHasPostInit);
	/// Pay txn funding the child's escrow: MBR, plus `{value:}` when the
	/// ctor runs at create time (with __postInit the value rides its group).
	void emitChildFunding(
		std::shared_ptr<awst::Expression> const& _createdAppId, bool _childHasPostInit);
	/// [pay(value), __postInit(args)] group against the created app id.
	void emitChildPostInit(
		solidity::frontend::FunctionDefinition const* _childCtor,
		std::string const& _newAppIdVarName);
};

} // namespace puyasol::builder::sol_ast

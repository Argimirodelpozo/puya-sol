#pragma once

#include "builder/sol-ast/SolMemberAccess.h"

namespace puyasol::builder::sol_ast
{

/// address.code, address.balance, etc.
class SolAddressProperty: public SolMemberAccess
{
public:
	using SolMemberAccess::SolMemberAccess;
	std::shared_ptr<awst::Expression> toAwst() override;

	enum class CodeProperty { Bytes, Size, Hash };
	/// Shared receiver evaluation and AVM adaptation for .code/.code.length/.codehash.
	static std::shared_ptr<awst::Expression> buildCodeMetadata(
		eb::ContractContext& _ctx, Context& _scope,
		solidity::frontend::Expression const& _address, CodeProperty _property,
		awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::sol_ast

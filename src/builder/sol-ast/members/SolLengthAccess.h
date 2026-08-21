#pragma once

#include "builder/sol-ast/SolMemberAccess.h"

namespace puyasol::builder::sol_ast
{

/// array.length, bytes.length — including box-backed array special case.
class SolLengthAccess: public SolMemberAccess
{
public:
	using SolMemberAccess::SolMemberAccess;
	std::shared_ptr<awst::Expression> toAwst() override;

	/// Runtime length (uint64) of a DYNAMIC box-backed state array named
	/// `name`. Mapping/nested-dynamic elements (encoded size 0) read the
	/// uint16 length prefix (missing box → 0); fixed-stride elements compute
	/// (max(box_len, 2) − 2) / elemSize. Shared with the index-bounds guards
	/// in SolIndexAccess so asserts agree with `.length` reads.
	static std::shared_ptr<awst::Expression> stateDynArrayLength(
		eb::ContractContext& ctx,
		std::string const& name,
		solidity::frontend::ArrayType const* arrType,
		awst::SourceLocation const& loc);

	/// Runtime-key form used by box-keyed storage-ref parameters.
	static std::shared_ptr<awst::Expression> stateDynArrayLengthForKey(
		eb::ContractContext& ctx,
		std::shared_ptr<awst::Expression> key,
		solidity::frontend::ArrayType const* arrType,
		awst::SourceLocation const& loc);
};

} // namespace puyasol::builder::sol_ast

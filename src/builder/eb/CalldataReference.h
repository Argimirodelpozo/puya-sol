#pragma once

#include "builder/context/TranslationContext.h"

namespace puyasol::builder::sol_ast
{

/// A solc calldata reference: an offset, and a length only when stackItems()
/// declares one. ABI-dynamic fixed arrays and structs are single pointers.
struct CalldataReference
{
	using Expr = std::shared_ptr<awst::Expression>;
	solidity::frontend::Type const* type;
	Expr offset, length;
	Expr data = nullptr; ///< Native ARC4 msg.data and word-shaped Yul calldata can differ.
	bool packedByte = false;

	static bool hasLength(solidity::frontend::Type const* type);
	static std::optional<CalldataReference> local(Context const& scope,
		solidity::frontend::VariableDeclaration const& declaration, awst::SourceLocation const& loc);
	static std::optional<CalldataReference> resolve(eb::ContractContext& ctx,
		solidity::frontend::Expression const& source, awst::SourceLocation const& loc);
	Expr pack(awst::SourceLocation const& loc) const;
	static std::optional<CalldataReference> unpack(solidity::frontend::Type const* type,
		Expr value, awst::SourceLocation const& loc);
	void bind(eb::ContractContext& ctx, solidity::frontend::VariableDeclaration const& declaration,
		awst::SourceLocation const& loc) const;
	Expr read(eb::ContractContext& ctx, awst::SourceLocation const& loc) const;
};

} // namespace puyasol::builder::sol_ast

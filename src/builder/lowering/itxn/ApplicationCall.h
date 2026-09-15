#pragma once

#include "awst/Node.h"

namespace solidity::frontend { class Type; }

namespace puyasol::builder
{
class TypeMapper;

/// Transport after source operands are evaluated. Solidity and Yul share
/// application validation, submission, and the return-data lifetime.
class ApplicationCall
{
public:
	using Expr = std::shared_ptr<awst::Expression>;
	using Statements = std::vector<std::shared_ptr<awst::Statement>>;

	static Expr submit(TypeMapper& types, Expr receiver, Expr arguments, Expr payment,
		awst::SourceLocation const& loc, Statements& out);
	static Expr submitRaw(TypeMapper& types, Expr receiver, Expr bytes, Expr payment,
		awst::SourceLocation const& loc, Statements& out);
	/// Capture an already submitted app call, before another inner transaction
	/// can replace its log. Returns a stable local snapshot of the payload.
	/// Only the ARC4-prefixed return record is data; ordinary events are not.
	static Expr capture(TypeMapper& types, awst::SourceLocation const& loc, Statements& out);
	static Expr setReturnData(TypeMapper& types, Expr bytes,
		awst::SourceLocation const& loc, Statements& out);
	/// Publish a modeled self-call using the same return wire as its external
	/// transport. The caller may reuse a single-evaluation value after this.
	static Expr setTypedReturnData(TypeMapper& types, Expr value,
		std::vector<solidity::frontend::Type const*> const& returns, bool evmWire,
		awst::SourceLocation const& loc, Statements& out);
	static Expr returnData(TypeMapper& types, awst::SourceLocation const& loc);
	/// Preserve short and empty payloads; do not read a fabricated selector.
	static Expr splitPayload(TypeMapper& types, Expr bytes, awst::SourceLocation const& loc);

private:
	static void submitOnly(TypeMapper& types, Expr receiver, Expr arguments, Expr payment,
		awst::SourceLocation const& loc, Statements& out);
};

} // namespace puyasol::builder

#pragma once

#include "builder/sol-ast/EvmSlotLowering.h"

namespace puyasol::builder::sol_ast
{

/// An already-evaluated assignment destination. Address/key/index evaluation
/// belongs to construction; reads and writes never lower the Solidity AST again.
/// Values use the solc type's native carrier, not the storage encoding.
class ResolvedLValue
{
public:
	using Expr = std::shared_ptr<awst::Expression>;
	ResolvedLValue(eb::ContractContext& _ctx,
		solidity::frontend::Expression const& _source,
		awst::SourceLocation const& _loc, Expr _built = nullptr);

	/// These destinations have addresses rather than assignable AWST expressions.
	static bool isAddressed(eb::ContractContext& _ctx,
		solidity::frontend::Expression const& _source);

	Expr read();
	/// Store a native value now and return the assigned scalar/value, or the
	/// destination slot handle for a storage-reference assignment result.
	Expr write(Expr _value);
	void clear();

private:
	void writeTarget(Expr _target, Expr _value);
	Expr resolveTarget(Expr _target);
	Expr pin(Expr _value);

	eb::ContractContext& m_ctx;
	solidity::frontend::Type const* m_type;
	awst::WType const* m_native;
	awst::SourceLocation m_loc;
	Expr m_target, m_blob, m_byteIndex;
	std::optional<EvmSlotLowering::Addr> m_slot;
	solidity::frontend::VariableDeclaration const* m_transient = nullptr;
	bool m_packedBlobByte = false;
};

} // namespace puyasol::builder::sol_ast

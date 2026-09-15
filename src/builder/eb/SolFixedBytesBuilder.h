#pragma once

#include "builder/eb/NodeBuilder.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

/// Instance builder for Solidity fixed-size bytes types (bytes1..bytes32).
///
/// Handles:
///   - binary_op: BitOr (|), BitXor (^), BitAnd (&) → BytesBinaryOperation
///   - compare: Eq/Ne → BytesComparisonExpression, Lt/Gt/etc. → b</b>/b<=/b>=
///   - bool_eval: bytes != zero
class SolFixedBytesBuilder: public InstanceBuilder
{
public:
	SolFixedBytesBuilder(
		ContractContext& _ctx,
		solidity::frontend::FixedBytesType const* _bytesType,
		std::shared_ptr<awst::Expression> _expr);

	solidity::frontend::Type const* solType() const override { return m_bytesType; }

	std::unique_ptr<InstanceBuilder> binary_op(
		InstanceBuilder& _other, BuilderBinaryOp _op,
		awst::SourceLocation const& _loc, bool _reverse = false) override;

	std::unique_ptr<InstanceBuilder> compare(
		InstanceBuilder& _other, BuilderComparisonOp _op,
		awst::SourceLocation const& _loc) override;

	std::unique_ptr<InstanceBuilder> bool_eval(
		awst::SourceLocation const& _loc, bool _negate = false) override;

	unsigned numBytes() const { return m_numBytes; }

private:
	solidity::frontend::FixedBytesType const* m_bytesType;
	unsigned m_numBytes;
};

/// EVM bytesN compares 32-byte LEFT-ALIGNED words: bytes3("abc")==bytes4("abc")
/// is true and "b" > "aa" (0x62.. > 0x6161..). AVM operands are N raw bytes, so
/// right-pad the shorter side to the common declared width — constants fold at
/// compile time (BytesConstant, or the bare 2-byte StringConstant a string
/// literal arrives as); RUNTIME operands pad too, since solc legally widens
/// bytesM→bytesN (`bytes2 a == bytes4 b`). No-op unless a side has a declared
/// width. Shared by SolFixedBytesBuilder::compare and buildBinaryOp.
void padBytesOperandsToCommonWidth(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression>& _lhs,
	std::shared_ptr<awst::Expression>& _rhs);

} // namespace puyasol::builder::eb

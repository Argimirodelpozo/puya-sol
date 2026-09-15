#pragma once

#include "builder/ast/SolExpression.h"

#include <libsolidity/ast/AST.h>
#include <optional>
#include <unordered_map>

namespace puyasol::builder::sol_ast
{

class EvmSlotLowering;
class ResolvedLValue;

/// Assignment expressions: =, +=, -=, *=, /=, etc.
/// Handles tuple decomposition, struct copy-on-write, bytes element assignment,
/// ARC4 encoding for storage targets, and compound assignment operators.
class SolAssignment: public SolExpression
{
public:
	SolAssignment(eb::ContractContext& _ctx, solidity::frontend::Assignment const& _node);
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	/// Classification of an already-lowered assignment target.  Keeping this
	/// decision in one place prevents the top-level translator from becoming a
	/// growing chain of mutually-exclusive dynamic_cast probes.
	enum class LValueKind
	{
		SlotArray,
		SlotScalar,
		Tuple,
		Generic,
	};

	struct LValuePlan
	{
		LValueKind kind = LValueKind::Generic;
	};

	solidity::frontend::Assignment const& m_assignment;
	std::unordered_map<int64_t, std::shared_ptr<ResolvedLValue>> m_tupleTargets;

	LValuePlan planLValue(std::shared_ptr<awst::Expression> const& _target) const;
	std::shared_ptr<awst::Expression> emitLValuePlan(
		LValuePlan _plan,
		solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> _target,
		std::shared_ptr<awst::Expression> _value);
	std::shared_ptr<awst::Expression> emitGenericAssignment(
		solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> _target,
		std::shared_ptr<awst::Expression> _value);

	std::shared_ptr<awst::Expression> handleTupleAssignment(
		std::shared_ptr<awst::Expression> _target,
		std::shared_ptr<awst::Expression> _value,
		solidity::frontend::TupleExpression const* _sourceLhs = nullptr,
		solidity::frontend::TupleType const* _sourceType = nullptr);

	// ── handleTupleAssignment pieces (SolAssignmentTuple.cpp) ───────────
	enum class TupleComponentAction { NotApplicable, Handled, Abort };
	std::shared_ptr<awst::Expression> snapshotTupleCallRhs(
		std::shared_ptr<awst::Expression> _value);
	std::shared_ptr<awst::Expression> pinLiteralTupleRhs(
		std::shared_ptr<awst::Expression> _value,
		solidity::frontend::TupleExpression const* _sourceLhs);
	TupleComponentAction tryStoragePointerComponent(
		size_t i,
		std::shared_ptr<awst::Expression> const& item,
		std::shared_ptr<awst::Expression> const& _value,
		solidity::frontend::TupleExpression const* _sourceLhs);
	void coerceTupleComponentValue(
		std::shared_ptr<awst::Expression> const& assignTarget,
		std::shared_ptr<awst::Expression>& assignValue);
	bool emitTupleComponentWrite(
		size_t i,
		std::shared_ptr<awst::Expression> const& itemIn,
		std::shared_ptr<awst::Expression> const& _value,
		solidity::frontend::TupleExpression const* _sourceLhs,
		solidity::frontend::TupleType const* _sourceType,
		std::vector<size_t>& componentGroupEnds);

	/// The aggregate-root writers' shared VALUE pipeline: compound compute at
	/// the leaf's native type (current decoded when needed, RHS widened),
	/// then coerceForAssignment + signExtendSignedWiden. The multibox copy
	/// used to skip the sign-extend — plain `big[i] = int8(-5)` stored the
	/// raw 64-bit two's complement (2^64-5) into the int256 element.
	std::shared_ptr<awst::Expression> computeAggregateStoreValue(
		solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> _current,
		std::shared_ptr<awst::Expression> _rhs,
		awst::WType const* _nativeW);

	/// Pre-buildExpr early-out handlers (each claims the shape or returns nullopt).

	/// `tx = v` / `tx += v` for a transient state var; routes through TransientStorage.
	/// --evm-storage-layout: any value-type write rooted at a persistent state
	/// var lowers to __storage_write at its EVM word address (EvmSlotLowering).
	std::optional<std::shared_ptr<awst::Expression>> tryHandleEvmStorageWrite();

	/// Runtime storage-reference rebinding; nullopt means this is not a rebind.
	std::optional<std::shared_ptr<awst::Expression>> tryEvmStoragePointerRebind(
		solidity::frontend::Expression const& _lhs);
	/// Fixed storage copies use solc's scalar-word or recursive value strategy.
	std::optional<std::shared_ptr<awst::Expression>> tryEvmFixedArrayWrite(
		solidity::frontend::Expression const& _lhs);
	/// Aggregate/converting fixed arrays: recursive per-element read/convert/write.
	std::shared_ptr<awst::Expression> emitEvmConvertingArrayCopy(
		EvmSlotLowering& _low,
		solidity::frontend::ArrayType const* _lat,
		solidity::frontend::ArrayType const* _rat,
		std::shared_ptr<awst::Expression> const& _lslot,
		std::shared_ptr<awst::Expression> const& _rslot);
	/// EVM blob memory: whole-variable assignment to a blob-backed memory
	/// local/param/named-return RE-SPILLS the value into a fresh blob region
	/// and re-points the offset var (EVM allocates fresh memory per result).
	std::optional<std::shared_ptr<awst::Expression>> tryHandleBlobRespill();

	std::optional<std::shared_ptr<awst::Expression>> tryHandleAddressedWrite();

	/// `m = m2` for a local storage-pointer: updates compile-time alias (state-var)
	/// or emits a runtime bytes assignment (mapping-key param).
	std::optional<std::shared_ptr<awst::Expression>> tryHandleStoragePointerReassign();

	/// `arr.push() = v`: scope RHS as the LHS push call's explicit value;
	/// SolArrayMethod folds it into ArrayExtend. Returns ArrayExtend or nullopt.
	std::optional<std::shared_ptr<awst::Expression>> tryHandlePushAssignRewrite(
		solidity::frontend::Token _op);

	/// EVM panic 0x21 on out-of-range enum assign; pre-emit assert.
	std::shared_ptr<awst::Expression> applyEnumRangeCheck(
		std::shared_ptr<awst::Expression> _value,
		solidity::frontend::Token _op);

	/// `slot = arr` (slot is biguint, arr is static-sized): expand to
	/// per-element __storage_write(slot+j, arr[j]).
	std::optional<std::shared_ptr<awst::Expression>> trySlotBasedArrayWrite(
		solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> const& _target,
		std::shared_ptr<awst::Expression> const& _value);

	/// `slot = v` (computed biguint slot): emit __storage_write(btoi(slot), v),
	/// with read-modify-write for compound assigns.
	std::optional<std::shared_ptr<awst::Expression>> trySlotBasedScalarWrite(
		solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> const& _target,
		std::shared_ptr<awst::Expression>& _value);

	/// `(a, b) = expr`: delegates to handleTupleAssignment; nullopt if not a tuple target.
	std::optional<std::shared_ptr<awst::Expression>> tryTupleAssignment(
		std::shared_ptr<awst::Expression>& _target,
		std::shared_ptr<awst::Expression>& _value);

	/// Compound-assign RHS canonicalization: a narrower SIGNED rhs is widened
	/// to the TARGET type's canonical form (`a op= b` == `a = a op T(b)`)
	/// before the compound compute — else the target-typed signed-div/mod
	/// path sign-extends the divisor from the wrong (target) width. Shared
	/// by every compound site. No-op for non-int/unsigned/non-narrower rhs.
	std::shared_ptr<awst::Expression> widenSignedCompoundRhs(
		std::shared_ptr<awst::Expression> _value);



};

} // namespace puyasol::builder::sol_ast

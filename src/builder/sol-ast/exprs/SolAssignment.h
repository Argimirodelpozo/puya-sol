#pragma once

#include "builder/sol-ast/SolExpression.h"

#include <libsolidity/ast/AST.h>
#include <optional>

namespace puyasol::builder::sol_ast
{

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
		BytesElement,
		Field,
		Generic,
	};

	struct LValuePlan
	{
		LValueKind kind = LValueKind::Generic;
	};

	solidity::frontend::Assignment const& m_assignment;

	LValuePlan planLValue(std::shared_ptr<awst::Expression> const& _target) const;
	std::shared_ptr<awst::Expression> emitLValuePlan(
		LValuePlan _plan,
		solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> _target,
		std::shared_ptr<awst::Expression> _value,
		bool _deferTupleLhsEffects,
		eb::ContractContext::OperandDeltas _tupleLhsEffects);
	std::shared_ptr<awst::Expression> emitGenericAssignment(
		solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> _target,
		std::shared_ptr<awst::Expression> _value);

	std::shared_ptr<awst::Expression> handleTupleAssignment(
		std::shared_ptr<awst::Expression> _target,
		std::shared_ptr<awst::Expression> _value,
		solidity::frontend::TupleExpression const* _sourceLhs = nullptr);

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
		std::vector<size_t>& componentGroupEnds);

	std::shared_ptr<awst::Expression> handleBytesElementAssignment(
		awst::IndexExpression const* _indexExpr,
		std::shared_ptr<awst::Expression> _value);

	/// `s.b[i] = v` where `s.b` is bytes (ARC4 byte[]): copy-on-write struct write-back.
	/// _newBytes is the replace3 result; builds NewStruct chain and emits the assignment.
	std::shared_ptr<awst::Expression> buildStructFieldBytesWrite(
		awst::FieldExpression const* _fieldExpr,
		awst::ARC4Struct const* _structType,
		std::shared_ptr<awst::Expression> _newBytes);

	// _emitAsStatement: in tuple-destructure context, queue the COW store as a
	// statement and return a truthy sentinel (tuple path only needs the side effect).
	std::shared_ptr<awst::Expression> handleStructFieldAssignment(
		awst::FieldExpression const* _fieldExpr,
		std::shared_ptr<awst::Expression> _value,
		std::shared_ptr<awst::Expression> _unwrappedTarget,
		bool _emitAsStatement = false);

	/// Build a TupleExpression with one field replaced.
	std::shared_ptr<awst::Expression> buildTupleWithUpdatedField(
		std::shared_ptr<awst::Expression> _base,
		std::string const& _fieldName,
		std::shared_ptr<awst::Expression> _newValue);

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

	/// The boxed-path/offset-struct writers' shared TAIL: pin the computed
	/// value, ARC4-encode it at the leaf type, replace the leaf inside the
	/// root temp (post-effect), then run the site's root write-back
	/// statement. Returns the pinned value (the assignment-expression
	/// result); nullptr when `_value` is null.
	std::shared_ptr<awst::Expression> emitAggregateLeafStore(
		std::shared_ptr<awst::Expression> _target,
		std::shared_ptr<awst::Expression> _value,
		std::string const& _tempStem,
		char const* _nameGenKey,
		std::shared_ptr<awst::Statement> _rootWriteback);

	/// Pre-buildExpr early-out handlers (each claims the shape or returns nullopt).

	/// `tx = v` / `tx += v` for a transient state var; routes through TransientStorage.
	/// --evm-storage-layout: any value-type write rooted at a persistent state
	/// var lowers to __storage_write at its EVM word address (EvmSlotLowering).
	std::optional<std::shared_ptr<awst::Expression>> tryHandleEvmStorageWrite();

	/// --evm-memory-layout: whole-variable assignment to a blob-backed memory
	/// local/param/named-return RE-SPILLS the value into a fresh blob region
	/// and re-points the offset var (EVM allocates fresh memory per result).
	std::optional<std::shared_ptr<awst::Expression>> tryHandleBlobRespill();

	std::optional<std::shared_ptr<awst::Expression>> tryHandleTransientStateWrite();

	/// `m = m2` for a local storage-pointer: updates compile-time alias (state-var)
	/// or emits a runtime bytes assignment (mapping-key param).
	std::optional<std::shared_ptr<awst::Expression>> tryHandleStoragePointerReassign();

	/// Write anywhere below a multi-box array (>32KB): select the outer
	/// element-aligned page, recursively mutate that element, and replace it.
	std::optional<std::shared_ptr<awst::Expression>> tryHandleMultiBoxArrayWrite();

	/// Write through any single-box aggregate root (a box-backed state variable
	/// or a box-keyed storage-ref parameter). Replays an arbitrary member/index
	/// path over a complete root value and persists it once.
	std::optional<std::shared_ptr<awst::Expression>> tryHandleBoxedAggregatePathWrite();

	/// A member-chain write rooted in a struct storage-ref PARAM carrying a
	/// runtime OFFSET. Rebuilds the complete fixed-layout struct slice and
	/// replaces it once, so nested structs and packed bool fields are generic.
	std::optional<std::shared_ptr<awst::Expression>> tryHandleOffsetStructRefFieldWrite();

	/// `a[i] = v` for a >4KB blob-backed aggregate. Computes base+i*elemSize,
	/// pads rhs to 32 B, emits writeMemWordDirect through the pre-effect frame.
	std::optional<std::shared_ptr<awst::Expression>> tryHandleBlobAggregateWrite();

	/// `arr.push() = v`: scope RHS as the LHS push call's explicit value;
	/// SolArrayMethod folds it into ArrayExtend. Returns ArrayExtend or nullopt.
	std::optional<std::shared_ptr<awst::Expression>> tryHandlePushAssignRewrite(
		solidity::frontend::Token _op);

	/// EVM panic 0x21 on out-of-range enum assign; pre-emit assert.
	std::shared_ptr<awst::Expression> applyEnumRangeCheck(
		std::shared_ptr<awst::Expression> _value,
		solidity::frontend::Token _op);

	/// Any index/member write rooted in a slot handle. Address derivation and
	/// value dispatch recurse through the declared Solidity type, so packed
	/// leaves and arbitrary array/struct depth use one path.
	std::optional<std::shared_ptr<awst::Expression>> tryHandleSlotHandleElemWrite();

	std::optional<std::shared_ptr<awst::Expression>> tryHandleSlotHandleFieldWrite();
	std::optional<std::shared_ptr<awst::Expression>> tryHandleSlotHandleWrite(
		solidity::frontend::Expression const& _lhs);

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

	/// `b[i] = v` where b is bytes: delegates to handleBytesElementAssignment;
	/// nullopt if target isn't IndexExpression on bytes.
	std::optional<std::shared_ptr<awst::Expression>> tryBytesElemAssignment(
		std::shared_ptr<awst::Expression> const& _target,
		std::shared_ptr<awst::Expression>& _value);

	/// `s.f = v` for ARC4 struct field or named-WTuple field.
	std::optional<std::shared_ptr<awst::Expression>> tryStructOrNamedTupleFieldAssignment(
		solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> const& _target,
		std::shared_ptr<awst::Expression>& _value);

	/// Compound-assign RHS canonicalization: a narrower SIGNED rhs is widened
	/// to the TARGET type's canonical form (`a op= b` == `a = a op T(b)`)
	/// before the compound compute — else the target-typed signed-div/mod
	/// path sign-extends the divisor from the wrong (target) width. Shared
	/// by every compound site. No-op for non-int/unsigned/non-narrower rhs.
	std::shared_ptr<awst::Expression> widenSignedCompoundRhs(
		std::shared_ptr<awst::Expression> _value);

	/// Compound assigns: read current target value, apply op, return new value.
	/// Simple Assign passes through unchanged.
	std::shared_ptr<awst::Expression> applyCompoundAssignment(
		solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> const& _target,
		std::shared_ptr<awst::Expression> _value);

	/// Assignment-boundary coercion: int→bytes[N], string→bytes, string↔bytes reinterpret.
	std::shared_ptr<awst::Expression> applyAssignmentTypeCoercion(
		std::shared_ptr<awst::Expression> _value,
		std::shared_ptr<awst::Expression> const& _target);


};

} // namespace puyasol::builder::sol_ast

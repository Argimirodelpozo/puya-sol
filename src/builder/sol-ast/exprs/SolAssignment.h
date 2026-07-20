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
	solidity::frontend::Assignment const& m_assignment;

	std::shared_ptr<awst::Expression> handleTupleAssignment(
		std::shared_ptr<awst::Expression> _target,
		std::shared_ptr<awst::Expression> _value,
		solidity::frontend::TupleExpression const* _sourceLhs = nullptr);

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

	/// Pre-buildExpr early-out handlers (each claims the shape or returns nullopt).

	/// `tx = v` / `tx += v` for a transient state var; routes through TransientStorage.
	std::optional<std::shared_ptr<awst::Expression>> tryHandleTransientStateWrite();

	/// `m = m2` for a local storage-pointer: updates compile-time alias (state-var)
	/// or emits a runtime bytes assignment (mapping-key param).
	std::optional<std::shared_ptr<awst::Expression>> tryHandleStoragePointerReassign();

	/// `arr[i] = v` for a multi-box array (>32KB). Emits
	/// box_replace(<name>++itob(page), (i%elemsPerBox)*elemSize, ARC4-encoded-rhs).
	std::optional<std::shared_ptr<awst::Expression>> tryHandleMultiBoxArrayWrite();

	/// `a[i].field = v` / `a[i] = v` where `a` is a box-keyed array REF PARAM (handle
	/// model): emits box_replace(paramKey, 2 + i*elemSize + fieldOffset, ARC4-encoded-rhs)
	/// directly, so the write hits the caller's box (a real side-effect, not DCE'd) instead
	/// of the COW reconstruction that drops it. Single-box, fixed-size struct elements only.
	std::optional<std::shared_ptr<awst::Expression>> tryHandleBoxedArrayElemWrite();

	/// `s.field = v` where `s` is a struct storage-ref PARAM carrying a runtime OFFSET (handle-model
	/// dual handle): emits box_replace(paramKey, offsetVar + fieldOffset, ARC4-rhs) so the write
	/// hits the element slice the caller passed (`f(arr[i])`), not the whole array box. Whole-box
	/// callers pass offset 0. Fixed-layout structs only.
	std::optional<std::shared_ptr<awst::Expression>> tryHandleOffsetStructRefFieldWrite();

	/// `a[i] = v` for a >4KB blob-backed aggregate. Computes base+i*elemSize,
	/// pads rhs to 32 B, emits writeMemWordDirect via prePendingStatements.
	std::optional<std::shared_ptr<awst::Expression>> tryHandleBlobAggregateWrite();

	/// `arr.push() = v`: stash RHS as pendingArrayPushValue before LHS build;
	/// SolArrayMethod folds it into ArrayExtend. Returns ArrayExtend or nullopt.
	std::optional<std::shared_ptr<awst::Expression>> tryHandlePushAssignRewrite(
		solidity::frontend::Token _op);

	/// EVM panic 0x21 on out-of-range enum assign; pre-emit assert.
	std::shared_ptr<awst::Expression> applyEnumRangeCheck(
		std::shared_ptr<awst::Expression> _value,
		solidity::frontend::Token _op);

	/// `arr[i] = v` through a slot handle where elements are PACKED sub-word
	/// scalars or STRUCTS: re-derive (slot, byteOffset) via SlotHandleAccess —
	/// the generic slot paths write whole words, wrong for both. Pre-buildExpr
	/// (controls building of base/idx/rhs itself).
	std::optional<std::shared_ptr<awst::Expression>> tryHandleSlotHandleElemWrite();

	/// `ptr.field = v` where ptr is a struct SLOT HANDLE (biguint): write the
	/// field's packed bytes into its word (full-word or read-modify-write).
	std::optional<std::shared_ptr<awst::Expression>> tryHandleSlotHandleFieldWrite();

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

	/// If target is ARC4-typed and value wtype differs, emit ARC4 encode
	/// (with widening/narrowing handling).
	std::shared_ptr<awst::Expression> applyArc4EncodeIfNeeded(
		std::shared_ptr<awst::Expression> _value,
		std::shared_ptr<awst::Expression> const& _target);

	/// For dynamic-elem static arrays and per-entry mapping boxes, emit a
	/// guarded box_put or box_create so the box exists before box_replace.
	void maybePrePopulateBox(
		std::shared_ptr<awst::Expression> const& _target);
};

} // namespace puyasol::builder::sol_ast

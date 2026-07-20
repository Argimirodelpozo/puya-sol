/// @file SolAssignment.cpp
/// Top-level assignment translator (try*/apply* pipeline).
/// Shape-specific handlers live in sibling SolAssignment*.cpp.

#include <algorithm>
#include "builder/sol-ast/exprs/SolAssignment.h"
#include "awst/NameGen.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/sol-types/Arc4ArrayWidening.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-ast/exprs/SolIndexAccess.h"
#include "builder/storage/SlotHandleAccess.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;
using Token = solidity::frontend::Token;

SolAssignment::SolAssignment(eb::ContractContext& _ctx, Assignment const& _node)
	: SolExpression(_ctx, _node), m_assignment(_node)
{
}

// toAwst pipeline:
//   (1) Pre-buildExpr early-outs (transient, storage-ptr, multi-box, push-assign)
//   (2) Build target + value
//   (3) Per-shape early-outs (enum check, slot writes, tuple, bytes-elem, struct/WTuple)
//   (4) Generic finalization (compound op, coerce, lvalue norm, ARC4 encode, box pre-populate)
std::shared_ptr<awst::Expression> SolAssignment::toAwst()
{
	Token op = m_assignment.assignmentOperator();

	// (1) Pre-buildExpr early-outs.
	if (auto r = tryHandleTransientStateWrite())     return std::move(*r);
	if (auto r = tryHandleStoragePointerReassign())  return std::move(*r);
	if (auto r = tryHandleMultiBoxArrayWrite())      return std::move(*r);
	if (auto r = tryHandleBoxedArrayElemWrite())     return std::move(*r);
	if (auto r = tryHandleOffsetStructRefFieldWrite()) return std::move(*r);
	if (auto r = tryHandleBlobAggregateWrite())      return std::move(*r);
	if (auto r = tryHandlePushAssignRewrite(op))     return std::move(*r);
	if (auto r = tryHandleSlotHandleElemWrite())     return std::move(*r);
	if (auto r = tryHandleSlotHandleFieldWrite())    return std::move(*r);

	// (2) Build target + value (if tryHandlePushAssignRewrite claimed, it already returned).
	auto target = buildExpr(m_assignment.leftHandSide());
	auto value = buildExpr(m_assignment.rightHandSide());

	// (3) Per-shape early-outs.
	value = applyEnumRangeCheck(std::move(value), op);
	if (auto r = trySlotBasedArrayWrite(op, target, value))                     return std::move(*r);
	if (auto r = trySlotBasedScalarWrite(op, target, value))                    return std::move(*r);
	if (auto r = tryTupleAssignment(target, value))                             return std::move(*r);
	if (auto r = tryBytesElemAssignment(target, value))                         return std::move(*r);
	if (auto r = tryStructOrNamedTupleFieldAssignment(op, target, value))       return std::move(*r);

	// (4) Generic finalization.
	value = applyCompoundAssignment(op, target, std::move(value));
	value = applyAssignmentTypeCoercion(std::move(value), target);
	target = awst::makeWritableTarget(std::move(target));
	value = applyArc4EncodeIfNeeded(std::move(value), target);
	maybePrePopulateBox(target);
	return awst::makeAssignmentExpression(std::move(target), std::move(value), m_loc);
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryHandlePushAssignRewrite(Token _op)
{
	// `arr.push() = value`: stash RHS as pendingArrayPushValue before LHS build so
	// SolArrayMethod::push folds it into ArrayExtend (we don't model Solidity refs).
	if (_op != Token::Assign) return std::nullopt;
	auto const* lhsCall = dynamic_cast<FunctionCall const*>(&m_assignment.leftHandSide());
	if (!lhsCall || !lhsCall->arguments().empty()) return std::nullopt;
	auto const* member = dynamic_cast<MemberAccess const*>(&lhsCall->expression());
	if (!member || member->memberName() != "push") return std::nullopt;

	m_ctx.pendingArrayPushValue = buildExpr(m_assignment.rightHandSide());
	auto target = buildExpr(m_assignment.leftHandSide());
	m_ctx.pendingArrayPushValue.reset();
	return target; // ArrayExtend emitted by SolArrayMethod
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryHandleBlobAggregateWrite()
{
	// Writes into a blob-backed aggregate: scalar leaves (`a[i]=v`, `p.f.x=v`)
	// and struct/array-valued copies (`p.w1 = bytesToG1Point(...)`).
	if (m_assignment.assignmentOperator() != Token::Assign)
		return std::nullopt;
	auto const& lhs = m_assignment.leftHandSide();
	auto const* lhsType = lhs.annotation().type;
	if (!lhsType)
		return std::nullopt;
	auto off = SolIndexAccess::resolveBlobOffset(m_ctx, m_scope, lhs, m_loc);
	if (!off)
		return std::nullopt;

	// Struct/array copy: write word-by-word via writeMemWordDirect.
	// Only 32-byte-aligned aggregates (e.g. Honk G1Point = 2×uint256 = 64 B).
	if (dynamic_cast<ArrayType const*>(lhsType) || dynamic_cast<StructType const*>(lhsType))
	{
		int sz = builder::computeEncodedElementSize(m_ctx.typeMapper.map(lhsType));
		if (sz <= 0 || sz % 32 != 0)
			return std::nullopt;
		auto agg = buildExpr(m_assignment.rightHandSide());
		auto aggBytes = awst::makeAsBytes(std::move(agg), m_loc);
		std::string offN = "__blobwa_off_" + std::to_string(m_assignment.id());
		std::string vN = "__blobwa_v_" + std::to_string(m_assignment.id());
		m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(offN, awst::WType::uint64Type(), m_loc), std::move(off), m_loc));
		m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(vN, awst::WType::bytesType(), m_loc), std::move(aggBytes), m_loc));
		for (int i = 0; i * 32 < sz; ++i)
		{
			auto wordOff = awst::makeUInt64BinOp(
				awst::makeVarExpression(offN, awst::WType::uint64Type(), m_loc),
				awst::UInt64BinaryOperator::Add,
				awst::makeIntegerConstant(static_cast<uint64_t>(i * 32), m_loc), m_loc);
			auto word = awst::makeExtract3(
				awst::makeVarExpression(vN, awst::WType::bytesType(), m_loc),
				awst::makeIntegerConstant(static_cast<uint64_t>(i * 32), m_loc),
				awst::makeIntegerConstant("32", m_loc), m_loc);
			builder::AssemblyBuilder::writeMemWordDirect(
				std::move(wordOff), std::move(word), m_loc, m_ctx.prePendingStatements);
		}
		return std::optional<std::shared_ptr<awst::Expression>>(
			awst::makeVarExpression(vN, awst::WType::bytesType(), m_loc));
	}

	// Materialise rhs; coerce to biguint so asBytes gives a valid 32-byte pad
	// (uint256/Fr stored as a full 32-byte EVM-memory word).
	auto v = buildExpr(m_assignment.rightHandSide());
	v = builder::TypeCoercion::implicitNumericCast(
		std::move(v), awst::WType::biguintType(), m_loc);
	std::string vN = "__blobassign_v_" + std::to_string(m_assignment.id());
	m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(vN, awst::WType::biguintType(), m_loc), std::move(v), m_loc));

	// Pad to exactly 32 big-endian bytes and write the blob word.
	auto vbytes = awst::makeExtractLastN(
		awst::makeLeftPad(awst::makeAsBytes(
			awst::makeVarExpression(vN, awst::WType::biguintType(), m_loc), m_loc), 32, m_loc), 32, m_loc);
	builder::AssemblyBuilder::writeMemWordDirect(
		std::move(off), std::move(vbytes), m_loc, m_ctx.prePendingStatements);

	return std::optional<std::shared_ptr<awst::Expression>>(
		awst::makeVarExpression(vN, awst::WType::biguintType(), m_loc));
}

std::shared_ptr<awst::Expression>
SolAssignment::applyEnumRangeCheck(std::shared_ptr<awst::Expression> _value, Token _op)
{
	// EVM panic 0x21 on out-of-range enum assign; pre-emit assert.
	if (_op != Token::Assign) return _value;
	auto const* lhsType = m_assignment.leftHandSide().annotation().type;
	auto const* enumType = dynamic_cast<EnumType const*>(lhsType);
	if (!enumType) return _value;

	unsigned numMembers = enumType->numberOfMembers();
	// EvalOnce: the value is referenced by the queued assert AND the returned
	// assignment value — a call-valued RHS ran twice (its twins in
	// SolExpressionStatement/SolEmitStatement already carry this fix).
	_value = awst::makeEvalOnce(std::move(_value), m_loc);
	auto val = builder::TypeCoercion::implicitNumericCast(_value, awst::WType::uint64Type(), m_loc);
	m_ctx.queuePreStmt(awst::makeEnumRangeAssert(val, numMembers, m_loc), m_loc);
	return val;
}

namespace
{
/// True iff `e` peels (through index layers) to an Identifier registered as a
/// slot-storage-ref local. Side-effect-free by construction — used to gate the
/// slot-handle write intercepts BEFORE building any expression (building a
/// side-effecting base like `m[1].push()` twice would double its effects).
bool rootsInSlotHandle(
	solidity::frontend::Expression const& e,
	puyasol::builder::sol_ast::Context& scope)
{
	auto const* cur = &e;
	while (auto const* ia = dynamic_cast<solidity::frontend::IndexAccess const*>(cur))
		cur = &ia->baseExpression();
	auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(cur);
	if (!id)
		return false;
	auto const* vd = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
		id->annotation().referencedDeclaration);
	return vd && vd->isLocalVariable() && scope.findSlotStorageRef(vd->id()) != nullptr;
}
} // namespace

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryHandleSlotHandleElemWrite()
{
	// `arr[i] = v` (and, for PACKED scalars, `arr[i] op= v`) on a slot-handle
	// base with PACKED sub-word or STRUCT elements. Full-word scalars keep the
	// generic slot path — its whole-word read-modify-write is correct there,
	// including compound ops. Packed compound MUST intercept: the generic path
	// addresses slot base+idx unscaled and clobbers a whole neighboring word.
	bool isCompound = m_assignment.assignmentOperator() != Token::Assign;
	auto const* lhs = dynamic_cast<IndexAccess const*>(&m_assignment.leftHandSide());
	if (!lhs || !lhs->indexExpression()) return std::nullopt;
	auto const* arrType = dynamic_cast<ArrayType const*>(lhs->baseExpression().annotation().type);
	if (!arrType || arrType->isDynamicallySized()
		|| !arrType->dataStoredIn(DataLocation::Storage)) return std::nullopt;
	auto const* elemType = arrType->baseType();
	auto const* structElem = dynamic_cast<StructType const*>(elemType);
	auto layout = builder::SlotHandleAccess::layoutFor(elemType);
	if (!structElem && layout.perSlot <= 1) return std::nullopt;
	if (structElem && isCompound) return std::nullopt; // no compound ops on structs

	// Gate on AST shape BEFORE building: the base chain must root in a
	// slot-handle local (building a side-effecting base twice would double
	// its effects — e.g. `m[1].push().a = v` box-model writes).
	if (!rootsInSlotHandle(lhs->baseExpression(), m_scope)) return std::nullopt;
	auto base = buildExpr(lhs->baseExpression());
	if (!base) return std::nullopt;
	if (base->wtype != awst::WType::biguintType())
	{
		// Chained bases (`_x[0]`) and struct-typed locals build as biguint
		// slot math above. A BARE array-typed local builds as its DECLARED
		// (arc4 array) type instead — read its HANDLE var directly (mirrors
		// SolIndexAccess's slot-ref path); without this the intercept never
		// fired for such locals and packed writes fell to the whole-word
		// wrong-slot path.
		auto const* baseId = dynamic_cast<Identifier const*>(&lhs->baseExpression());
		auto const* baseDecl = baseId
			? dynamic_cast<VariableDeclaration const*>(baseId->annotation().referencedDeclaration)
			: nullptr;
		if (!baseDecl) return std::nullopt;
		base = awst::makeVarExpression(
			baseDecl->name(), awst::WType::biguintType(), m_loc);
	}
	auto idx = buildExpr(*lhs->indexExpression());
	if (!idx) return std::nullopt;
	if (idx->wtype == awst::WType::uint64Type())
		idx = awst::makeAsBiguint(awst::makeItob(std::move(idx), m_loc), m_loc);
	// FIXED array (guaranteed by the gate above): assert idx < length before
	// the address math (EVM Panic 0x32; OOB would clobber a neighboring slot).
	idx = builder::SlotHandleAccess::boundsCheckIndex(
		m_ctx.prePendingStatements, std::move(idx), arrType, m_loc);

	std::vector<std::shared_ptr<awst::Statement>> out;
	if (structElem)
	{
		auto const* structW = dynamic_cast<awst::ARC4Struct const*>(
			m_ctx.typeMapper.map(structElem));
		if (!structW) return std::nullopt;
		auto value = buildExpr(m_assignment.rightHandSide());
		if (!value) return std::nullopt;
		auto stride = awst::makeIntegerConstant(
			structElem->storageSize().str(), m_loc, awst::WType::biguintType());
		auto elemBase = awst::makeBigUIntBinOp(std::move(base),
			awst::BigUIntBinaryOperator::Add,
			awst::makeBigUIntBinOp(std::move(idx), awst::BigUIntBinaryOperator::Mult,
				std::move(stride), m_loc), m_loc);
		builder::SlotHandleAccess::writeStructElem(
			out, std::move(elemBase), structElem, structW, std::move(value), m_loc);
	}
	else
	{
		std::shared_ptr<awst::Expression> value;
		if (isCompound)
		{
			// `p[i] op= v` on a PACKED element: packed-aware read (sign-
			// extended canonical biguint), compute at the element's Solidity
			// type, write back sub-word. base/idx are eval-once'd so the read
			// and the write share one evaluation. The read is cast to the
			// element's NATIVE carrier first (uint64 for ≤64-bit: low 8 bytes
			// of the canonical TC = the 64-bit-TC carrier) so the compound
			// arithmetic keeps checked-overflow semantics at the declared
			// width (decode-before-arith, as the box-array path does).
			base = awst::makeEvalOnce(std::move(base), m_loc);
			idx = awst::makeEvalOnce(std::move(idx), m_loc);
			auto current = builder::SlotHandleAccess::readScalarElem(
				base, idx, layout, elemType, m_loc);
			auto* nativeType = m_ctx.typeMapper.map(elemType);
			if (nativeType && current->wtype != nativeType)
				current = builder::TypeCoercion::implicitNumericCast(
					std::move(current), nativeType, m_loc);
			auto rhs = buildExpr(m_assignment.rightHandSide());
			if (!rhs) return std::nullopt;
			value = applyCompoundAssignment(
				m_assignment.assignmentOperator(), current, std::move(rhs));
		}
		else
		{
			value = buildExpr(m_assignment.rightHandSide());
			if (!value) return std::nullopt;
		}
		// canonical biguint value
		if (value->wtype && value->wtype->kind() == awst::WTypeKind::ARC4UIntN)
			value = awst::makeARC4Decode(std::move(value), awst::WType::biguintType(), m_loc);
		else if (value->wtype == awst::WType::uint64Type())
			value = awst::makeAsBiguint(awst::makeItob(std::move(value), m_loc), m_loc);
		else if (value->wtype == awst::WType::boolType())
			value = awst::makeConditional(std::move(value),
				awst::makeIntegerConstant("1", m_loc, awst::WType::biguintType()),
				awst::makeIntegerConstant("0", m_loc, awst::WType::biguintType()),
				awst::WType::biguintType(), m_loc);
		builder::SlotHandleAccess::writeScalarElem(
			out, std::move(base), std::move(idx), layout, std::move(value), m_loc);
	}
	for (auto& st: out)
		m_ctx.queuePending(std::move(st));
	return std::shared_ptr<awst::Expression>{awst::makeZero(m_loc, awst::WType::biguintType())};
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryHandleSlotHandleFieldWrite()
{
	if (m_assignment.assignmentOperator() != Token::Assign) return std::nullopt;
	auto const* lhs = dynamic_cast<MemberAccess const*>(&m_assignment.leftHandSide());
	if (!lhs) return std::nullopt;
	auto const* solStruct = dynamic_cast<StructType const*>(lhs->expression().annotation().type);
	if (!solStruct || !solStruct->dataStoredIn(DataLocation::Storage)) return std::nullopt;
	// The handle local resolves with its DECLARED struct wtype through the
	// generic identifier path — consult the slot-storage-ref registry directly.
	// ONLY registry-rooted bases: building an arbitrary (possibly
	// side-effecting) base to inspect its wtype would double its effects.
	std::shared_ptr<awst::Expression> base;
	if (auto const* baseId = dynamic_cast<Identifier const*>(&lhs->expression()))
		if (auto const* vd = dynamic_cast<VariableDeclaration const*>(
				baseId->annotation().referencedDeclaration))
			if (vd->isLocalVariable() && m_scope.findSlotStorageRef(vd->id()))
				base = awst::makeVarExpression(vd->name(), awst::WType::biguintType(), m_loc);
	if (!base && rootsInSlotHandle(lhs->expression(), m_scope))
	{
		base = buildExpr(lhs->expression());
		if (!base || base->wtype != awst::WType::biguintType())
			return std::nullopt;
	}
	if (!base)
		return std::nullopt;

	auto const& off = solStruct->storageOffsetsOfMember(lhs->memberName());
	auto const* fieldSolType = lhs->annotation().type;
	unsigned size = fieldSolType ? fieldSolType->storageBytes() : 32;

	auto value = buildExpr(m_assignment.rightHandSide());
	if (!value) return std::nullopt;
	// canonical biguint
	if (value->wtype && value->wtype->kind() == awst::WTypeKind::ARC4UIntN)
		value = awst::makeARC4Decode(std::move(value), awst::WType::biguintType(), m_loc);
	else if (value->wtype == awst::WType::uint64Type())
		value = awst::makeAsBiguint(awst::makeItob(std::move(value), m_loc), m_loc);
	else if (value->wtype == awst::WType::boolType())
		value = awst::makeConditional(std::move(value),
			awst::makeIntegerConstant("1", m_loc, awst::WType::biguintType()),
			awst::makeIntegerConstant("0", m_loc, awst::WType::biguintType()),
			awst::WType::biguintType(), m_loc);
	if (value->wtype != awst::WType::biguintType()) return std::nullopt;

	auto slotExpr = awst::makeBigUIntBinOp(std::move(base),
		awst::BigUIntBinaryOperator::Add,
		awst::makeIntegerConstant(off.first.str(), m_loc, awst::WType::biguintType()), m_loc);

	std::vector<std::shared_ptr<awst::Statement>> out;
	if (size == 32 && off.second == 0)
		out.push_back(builder::SlotHandleAccess::writeSlot(
			std::move(slotExpr), std::move(value), m_loc));
	else
	{
		// packed field: word read-modify-write at compile-time position
		std::string tmp = "__slotf_" + std::to_string(m_assignment.id());
		out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(tmp, awst::WType::biguintType(), m_loc),
			std::move(slotExpr), m_loc));
		auto slotVar = [&]() {
			return awst::makeVarExpression(tmp, awst::WType::biguintType(), m_loc);
		};
		unsigned start = 32 - off.second - size;
		auto fieldB = awst::makeExtract(
			awst::makeZeroExtendToN(awst::makeAsBytes(std::move(value), m_loc), 32, m_loc),
			static_cast<int>(32 - size), static_cast<int>(size), m_loc);
		auto wordB = awst::makeLeftPadToN(awst::makeAsBytes(
			builder::SlotHandleAccess::readSlot(slotVar(), m_loc), m_loc), 32, m_loc);
		auto newWord = awst::makeReplace3(std::move(wordB),
			awst::makeIntegerConstant(static_cast<uint64_t>(start), m_loc),
			std::move(fieldB), m_loc);
		out.push_back(builder::SlotHandleAccess::writeSlot(slotVar(),
			awst::makeAsBiguint(std::move(newWord), m_loc), m_loc));
	}
	for (auto& st: out)
		m_ctx.queuePending(std::move(st));
	return std::shared_ptr<awst::Expression>{awst::makeZero(m_loc, awst::WType::biguintType())};
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::trySlotBasedArrayWrite(
	Token _op,
	std::shared_ptr<awst::Expression> const& _target,
	std::shared_ptr<awst::Expression> const& _value)
{
	// Slot-based array write: target is a biguint slot handle for a fixed
	// array. Three shapes:
	//   rhs slot handle (biguint)  → slot-level copy over storageSize() slots
	//                                (type-agnostic: packed/multislot/mixed)
	//   rhs array VALUE            → packed-aware per-element writes, with the
	//                                lhs tail ZERO-FILLED (EVM partial-assign
	//                                semantics: copy then clear the rest)
	//   struct elements            → per-slot word writes via SlotHandleAccess
	if (_op != Token::Assign || _target->wtype != awst::WType::biguintType()) return std::nullopt;
	auto const* lhsType = m_assignment.leftHandSide().annotation().type;
	auto const* arrType = lhsType ? dynamic_cast<ArrayType const*>(lhsType) : nullptr;
	if (!arrType)
	{
		auto const* rhsType = m_assignment.rightHandSide().annotation().type;
		arrType = rhsType ? dynamic_cast<ArrayType const*>(rhsType) : nullptr;
	}
	if (!arrType || arrType->isDynamicallySized()) return std::nullopt;

	std::vector<std::shared_ptr<awst::Statement>> out;

	// rhs is itself a slot handle → copy every slot of the array footprint.
	if (_value->wtype == awst::WType::biguintType())
	{
		auto slots = arrType->storageSize();
		if (slots > 256)
		{
			Logger::instance().error(
				"slot-handle array copy of " + slots.str()
				+ " slots exceeds the unroll cap (256)", m_loc);
			return std::nullopt;
		}
		auto srcVar = [&]() { return _value; };
		auto dstVar = [&]() { return _target; };
		unsigned n = static_cast<unsigned>(slots);
		for (unsigned j = 0; j < n; ++j)
		{
			auto jc = [&]() { return awst::makeIntegerConstant(j, m_loc, awst::WType::biguintType()); };
			auto src = awst::makeBigUIntBinOp(srcVar(), awst::BigUIntBinaryOperator::Add, jc(), m_loc);
			auto dst = awst::makeBigUIntBinOp(dstVar(), awst::BigUIntBinaryOperator::Add, jc(), m_loc);
			out.push_back(builder::SlotHandleAccess::writeSlot(
				std::move(dst), builder::SlotHandleAccess::readSlot(std::move(src), m_loc), m_loc));
		}
		for (auto& st: out)
			m_ctx.queuePending(std::move(st));
		return std::shared_ptr<awst::Expression>{awst::makeZero(m_loc, awst::WType::biguintType())};
	}

	// rhs is an array VALUE.
	unsigned lhsLen = static_cast<unsigned>(arrType->length());
	if (lhsLen > 64)
	{
		Logger::instance().error(
			"slot-handle array assignment of length " + std::to_string(lhsLen)
			+ " exceeds the unroll cap (64)", m_loc);
		return std::nullopt;
	}
	unsigned rhsLen = lhsLen;
	if (auto const* rhsArr = dynamic_cast<ArrayType const*>(
			m_assignment.rightHandSide().annotation().type))
		if (!rhsArr->isDynamicallySized())
			rhsLen = static_cast<unsigned>(rhsArr->length());

	auto const* elemType = arrType->baseType();
	auto const* structElem = dynamic_cast<StructType const*>(elemType);
	auto layout = builder::SlotHandleAccess::layoutFor(elemType);

	// bind target + value once
	std::string tBase = "__slotw_base_" + std::to_string(m_assignment.id());
	out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(tBase, awst::WType::biguintType(), m_loc), _target, m_loc));
	auto baseVar = [&]() { return awst::makeVarExpression(tBase, awst::WType::biguintType(), m_loc); };
	std::string tVal = "__slotw_val_" + std::to_string(m_assignment.id());
	out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(tVal, _value->wtype, m_loc), _value, m_loc));
	auto valVar = [&]() { return awst::makeVarExpression(tVal, _value->wtype, m_loc); };

	awst::WType const* elemWtype;
	if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(_value->wtype))
		elemWtype = sa->elementType();
	else if (auto const* da = dynamic_cast<awst::ARC4DynamicArray const*>(_value->wtype))
		elemWtype = da->elementType();
	else
		elemWtype = m_ctx.typeMapper.map(elemType);

	auto const* structW = structElem
		? dynamic_cast<awst::ARC4Struct const*>(m_ctx.typeMapper.map(structElem))
		: nullptr;
	if (structElem && !structW)
		return std::nullopt;

	for (unsigned j = 0; j < lhsLen; ++j)
	{
		auto jConst = [&]() {
			return awst::makeIntegerConstant(j, m_loc, awst::WType::biguintType());
		};
		if (structElem)
		{
			auto elemBase = awst::makeBigUIntBinOp(baseVar(),
				awst::BigUIntBinaryOperator::Add,
				awst::makeIntegerConstant(
					(structElem->storageSize() * j).str(), m_loc, awst::WType::biguintType()),
				m_loc);
			if (j < rhsLen)
			{
				auto elemVal = awst::makeIndexExpression(valVar(),
					awst::makeIntegerConstant(j, m_loc), structW, m_loc);
				builder::SlotHandleAccess::writeStructElem(
					out, std::move(elemBase), structElem, structW, std::move(elemVal), m_loc);
			}
			else
			{
				// zero-fill: clear every slot of the element
				unsigned stride = static_cast<unsigned>(structElem->storageSize());
				for (unsigned st = 0; st < stride; ++st)
				{
					auto slotJ = awst::makeBigUIntBinOp(baseVar(),
						awst::BigUIntBinaryOperator::Add,
						awst::makeIntegerConstant(
							(structElem->storageSize() * j + st).str(),
							m_loc, awst::WType::biguintType()), m_loc);
					out.push_back(builder::SlotHandleAccess::writeSlot(std::move(slotJ),
						awst::makeIntegerConstant("0", m_loc, awst::WType::biguintType()), m_loc));
				}
			}
			continue;
		}

		std::shared_ptr<awst::Expression> elemVal;
		if (j < rhsLen)
		{
			elemVal = awst::makeIndexExpression(valVar(),
				awst::makeIntegerConstant(j, m_loc), elemWtype, m_loc);
			if (elemVal->wtype && elemVal->wtype->kind() == awst::WTypeKind::ARC4UIntN)
				elemVal = awst::makeARC4Decode(std::move(elemVal), awst::WType::biguintType(), m_loc);
			else if (elemVal->wtype == awst::WType::uint64Type())
				elemVal = awst::makeAsBiguint(awst::makeItob(std::move(elemVal), m_loc), m_loc);
		}
		else
			elemVal = awst::makeIntegerConstant("0", m_loc, awst::WType::biguintType());

		builder::SlotHandleAccess::writeScalarElem(
			out, baseVar(), jConst(), layout, std::move(elemVal), m_loc);
	}
	for (auto& st: out)
		m_ctx.queuePending(std::move(st));
	return std::shared_ptr<awst::Expression>{awst::makeZero(m_loc, awst::WType::biguintType())};
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::trySlotBasedScalarWrite(
	Token _op,
	std::shared_ptr<awst::Expression> const& _target,
	std::shared_ptr<awst::Expression>& _value)
{
	// Scalar slot-based write: emit __storage_write(btoi(slot), value).
	if (!dynamic_cast<awst::BigUIntBinaryOperation const*>(_target.get())
		|| _target->wtype != awst::WType::biguintType())
		return std::nullopt;

	// Compound: read current first, apply op.
	if (_op != Token::Assign)
	{
		auto readSlot = builder::StorageMapper::biguintSlotToBtoi(_target, m_loc);
		auto readCall = awst::makeSubroutineCall(
			awst::SubroutineID{"__puyasol___storage_read"}, awst::WType::biguintType(), m_loc);
		awst::pushCallArg(readCall->args, "__slot", std::move(readSlot));

		auto* targetSolType = m_assignment.leftHandSide().annotation().type;
		_value = widenSignedCompoundRhs(std::move(_value));
		auto builderResult = eb::AssignmentHelper::tryComputeCompoundValue(
			m_ctx, _op, targetSolType, readCall, _value, m_loc);
		if (builderResult)
			_value = std::move(builderResult);
		else
			_value = m_ctx.buildBinaryOp(_op, std::move(readCall), std::move(_value),
				_target->wtype, m_loc);
	}

	auto btoi = builder::StorageMapper::biguintSlotToBtoi(_target, m_loc);
	auto call = awst::makeSubroutineCall(
		awst::SubroutineID{"__puyasol___storage_write"}, awst::WType::voidType(), m_loc);
	awst::pushCallArg(call->args, "__slot", std::move(btoi));
	awst::pushCallArg(call->args, "__value", std::move(_value));
	m_ctx.queueStmt(std::move(call), m_loc);
	return std::shared_ptr<awst::Expression>{awst::makeZero(m_loc, awst::WType::biguintType())};
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryTupleAssignment(
	std::shared_ptr<awst::Expression>& _target,
	std::shared_ptr<awst::Expression>& _value)
{
	if (!dynamic_cast<awst::TupleExpression const*>(_target.get())) return std::nullopt;
	auto const* sourceLhs = dynamic_cast<solidity::frontend::TupleExpression const*>(
		&m_assignment.leftHandSide());
	return handleTupleAssignment(std::move(_target), std::move(_value), sourceLhs);
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryBytesElemAssignment(
	std::shared_ptr<awst::Expression> const& _target,
	std::shared_ptr<awst::Expression>& _value)
{
	auto const* indexExpr = dynamic_cast<awst::IndexExpression const*>(_target.get());
	if (!indexExpr || !indexExpr->base || !indexExpr->base->wtype
		|| indexExpr->base->wtype->kind() != awst::WTypeKind::Bytes)
		return std::nullopt;
	return handleBytesElementAssignment(indexExpr, std::move(_value));
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryStructOrNamedTupleFieldAssignment(
	Token _op,
	std::shared_ptr<awst::Expression> const& _target,
	std::shared_ptr<awst::Expression>& _value)
{
	// Unwrap ARC4Decode for Lvalue purposes
	auto unwrappedTarget = _target;
	if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(_target.get()))
		unwrappedTarget = decodeExpr->value;

	auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(unwrappedTarget.get());
	if (!fieldExpr) return std::nullopt;

	// ARC4Struct field: copy-on-write.
	auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(fieldExpr->base->wtype);
	if (!arc4StructType)
		if (auto const* sg = dynamic_cast<awst::StateGet const*>(fieldExpr->base.get()))
			arc4StructType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);
	if (arc4StructType)
		return handleStructFieldAssignment(fieldExpr, std::move(_value), unwrappedTarget);

	// Named-WTuple field assignment (`t.field = v`, return-tuple style).
	auto const* tupleType = dynamic_cast<awst::WTuple const*>(fieldExpr->base->wtype);
	if (!tupleType || !tupleType->names().has_value()) return std::nullopt;

	auto base = fieldExpr->base;
	std::string fieldName = fieldExpr->name;

	if (_op != Token::Assign)
	{
		auto currentField = awst::makeFieldExpression(base, fieldName, fieldExpr->wtype, m_loc);
		auto* solType = m_assignment.leftHandSide().annotation().type;
		auto builderResult = eb::AssignmentHelper::tryComputeCompoundValue(
			m_ctx, _op, solType, currentField, _value, m_loc);
		if (builderResult)
			_value = std::move(builderResult);
		else
			_value = m_ctx.buildBinaryOp(_op, std::move(currentField), std::move(_value),
				fieldExpr->wtype, m_loc);
	}

	_value = builder::TypeCoercion::implicitNumericCast(
		std::move(_value), fieldExpr->wtype, m_loc);
	auto newTuple = buildTupleWithUpdatedField(base, fieldName, std::move(_value));

	auto writeTarget = awst::unwrapStateGet(base);

	auto e = awst::makeAssignmentExpression(std::move(writeTarget), std::move(newTuple), m_loc);
	return awst::makeFieldExpression(std::move(e), fieldName, fieldExpr->wtype, m_loc);
}

std::shared_ptr<awst::Expression>
SolAssignment::widenSignedCompoundRhs(std::shared_ptr<awst::Expression> _value)
{
	// Solidity `a op= b` is `a = a op T(b)`: the RHS converts to the TARGET
	// type FIRST. A narrower SIGNED rhs must reach the compound compute in
	// the target's CANONICAL form — tryComputeCompoundValue builds both
	// operand builders at the TARGET type, so the signed-div/mod path
	// sign-extends from the target width and a not-yet-widened negative
	// divisor read as huge-positive (`int128 x; int16 y=-32768; x /= y`
	// divided by +1.8e19). uint64-carried rhs into a biguint-backed target
	// needs promotion + extension to 256-bit TC; same-carrier widens go
	// through signExtendSignedWiden. Shared by every compound site
	// (applyCompoundAssignment, transient, slot-scalar) so they can't drift.
	auto const* rhsSolType = m_assignment.rightHandSide().annotation().type;
	auto const* tgtSolType = m_assignment.leftHandSide().annotation().type;
	auto rhsInt = builder::SolIntType::fromSol(rhsSolType);
	auto tgtInt = builder::SolIntType::fromSol(tgtSolType);
	if (!_value || !rhsInt || !tgtInt || !rhsInt->isSigned || !tgtInt->isSigned
		|| rhsInt->bits >= tgtInt->bits)
		return _value;
	if (tgtInt->bits > 64 && _value->wtype == awst::WType::uint64Type())
		return builder::TypeCoercion::signExtendToUint256(
			builder::TypeCoercion::implicitNumericCast(
				std::move(_value), awst::WType::biguintType(), m_loc),
			rhsInt->bits, m_loc);
	return builder::TypeCoercion::signExtendSignedWiden(
		std::move(_value), rhsSolType, tgtSolType, m_loc);
}

std::shared_ptr<awst::Expression>
SolAssignment::applyCompoundAssignment(
	Token _op,
	std::shared_ptr<awst::Expression> const& _target,
	std::shared_ptr<awst::Expression> _value)
{
	if (_op == Token::Assign) return _value;

	_value = widenSignedCompoundRhs(std::move(_value));

	// Reuse the already-built target to avoid re-evaluating a side-effecting
	// index (e.g. `arr[i++] += 5` gave i==2 when LHS was rebuilt). The built
	// target is the read form; wrap BoxValue in StateGet to read the stored value.
	// makeWritableTarget in toAwst still yields the write target from the same node.
	auto currentValue = _target;
	auto* targetSolType = m_assignment.leftHandSide().annotation().type;
	if (dynamic_cast<awst::BoxValueExpression const*>(currentValue.get()))
		currentValue = builder::StorageMapper::makeStateGetWithDefault(currentValue, currentValue->wtype, m_loc);
	else if (auto const* idx = dynamic_cast<awst::IndexExpression const*>(currentValue.get()))
	{
		// Storage dynamic-array element: the write-form indexes a box and yields the ARC4-ENCODED
		// element (Encoded(uintN)); the compound arithmetic itob's that and fails. Decode to the native
		// value (memory/calldata index exprs already carry native values, so gate on a box base; the
		// later applyArc4EncodeIfNeeded re-encodes the result). Mirrors the read path's decode.
		if (dynamic_cast<awst::BoxValueExpression const*>(idx->base.get()))
		{
			auto* nativeType = m_ctx.typeMapper.map(targetSolType);
			if (currentValue->wtype != nativeType)
				currentValue = builder::TypeCoercion::signExtendSignedElement(
					awst::makeARC4Decode(currentValue, nativeType, m_loc), targetSolType, m_loc);
		}
	}
	auto builderResult = eb::AssignmentHelper::tryComputeCompoundValue(
		m_ctx, _op, targetSolType, currentValue, _value, m_loc);
	if (builderResult)
		return builderResult;
	return m_ctx.buildBinaryOp(_op, std::move(currentValue), std::move(_value),
		_target->wtype, m_loc);
}

std::shared_ptr<awst::Expression>
SolAssignment::applyAssignmentTypeCoercion(
	std::shared_ptr<awst::Expression> _value,
	std::shared_ptr<awst::Expression> const& _target)
{
	// int→bytes[N], string→bytes, numeric casts.
	_value = builder::TypeCoercion::coerceForAssignment(std::move(_value), _target->wtype, m_loc);
	// Signed sub-word → wider-signed implicit widen (`b = someInt8;` b:int16): coerceForAssignment
	// is a uint64→uint64 no-op that drops the sign. Re-extend from the RHS width. Plain `=` only
	// (compound `+=` coerces a same-typed computed value, not the raw RHS).
	if (m_assignment.assignmentOperator() == Token::Assign)
		_value = builder::TypeCoercion::signExtendSignedWiden(
			std::move(_value), m_assignment.rightHandSide().annotation().type,
			m_assignment.leftHandSide().annotation().type, m_loc);
	if (_value->wtype != _target->wtype && _target->wtype
		&& _target->wtype->kind() == awst::WTypeKind::Bytes)
	{
		auto const* bytesType = dynamic_cast<awst::BytesWType const*>(_target->wtype);
		auto const* strConst = dynamic_cast<awst::StringConstant const*>(_value.get());
		if (bytesType && bytesType->length().has_value() && *bytesType->length() > 0 && strConst)
		{
			if (auto padded = builder::TypeCoercion::stringToBytesN(
					_value.get(), _target->wtype, *bytesType->length(), m_loc))
				_value = std::move(padded);
		}
		else
		{
			bool valueIsCompatible = _value->wtype == awst::WType::stringType()
				|| (_value->wtype && _value->wtype->kind() == awst::WTypeKind::Bytes);
			if (valueIsCompatible)
				_value = awst::makeReinterpretCast(std::move(_value), _target->wtype, m_loc);
		}
	}
	if (_value->wtype != _target->wtype && _target->wtype == awst::WType::stringType()
		&& _value->wtype && (_value->wtype->kind() == awst::WTypeKind::Bytes
			|| _value->wtype == awst::WType::bytesType()))
	{
		_value = awst::makeReinterpretCast(std::move(_value), _target->wtype, m_loc);
	}
	return _value;
}

std::shared_ptr<awst::Expression>
SolAssignment::applyArc4EncodeIfNeeded(
	std::shared_ptr<awst::Expression> _value,
	std::shared_ptr<awst::Expression> const& _target)
{
	if (_value->wtype == _target->wtype) return _value;

	bool targetIsArc4 = false;
	switch (_target->wtype->kind())
	{
	case awst::WTypeKind::ARC4UIntN:
	case awst::WTypeKind::ARC4StaticArray:
	case awst::WTypeKind::ARC4DynamicArray:
	case awst::WTypeKind::ARC4Struct:
		targetIsArc4 = true; break;
	default: break;
	}
	if (!targetIsArc4) return _value;

	// Skip encode if types match structurally (TypeMapper may not intern pointers;
	// double-encoding would corrupt an ARC4 aggregate).
	bool sameShape = _value->wtype->kind() == _target->wtype->kind()
		&& _value->wtype->name() == _target->wtype->name();
	if (sameShape) return _value;

	_value = builder::TypeCoercion::stringToBytes(std::move(_value), m_loc);

	// Array element-wise widening: arc4 array<intM> → arc4 array<intN> (M<N) has no
	// puya codec. Pin source bytes to a temp and call the helper.
	bool const sourceIsArc4Array =
		_value->wtype->kind() == awst::WTypeKind::ARC4StaticArray
		|| _value->wtype->kind() == awst::WTypeKind::ARC4DynamicArray;
	bool const targetIsArc4Array =
		_target->wtype->kind() == awst::WTypeKind::ARC4StaticArray
		|| _target->wtype->kind() == awst::WTypeKind::ARC4DynamicArray;
	if (sourceIsArc4Array && targetIsArc4Array)
	{
		std::string tmpName = "__widen_src_" + std::to_string(awst::NameGen::next("SolAssignment.s_widCounter"));
		auto srcAsBytes = awst::makeAsBytes(_value, m_loc);
		auto tmpVar = awst::makeVarExpression(tmpName, awst::WType::bytesType(), m_loc);
		m_ctx.prePendingStatements.push_back(
			awst::makeAssignmentStatement(tmpVar, std::move(srcAsBytes), m_loc));
		auto const* sourceType = _value->wtype;
		auto mkSrc = [&]() {
			return awst::makeVarExpression(tmpName, awst::WType::bytesType(), m_loc);
		};
		std::shared_ptr<awst::Expression> widened;
		if (_target->wtype->kind() == awst::WTypeKind::ARC4StaticArray)
			widened = builder::tryWidenArc4StaticArrayInt(
				sourceType, _target->wtype, mkSrc, m_loc);
		else
			widened = builder::tryWidenArc4DynamicArrayInt(
				sourceType, _target->wtype, mkSrc,
				[this](std::shared_ptr<awst::Statement> _s) {
					m_ctx.prePendingStatements.push_back(std::move(_s));
				},
				m_loc);
		if (widened) return widened;
	}

	// Narrowing: uint64 → arc4.uintN (N < 64).
	if (auto narrowed = builder::tryNarrowUInt64ToArc4UIntN(
			_value, _target->wtype, m_loc))
		return narrowed;

	// bytes/string → dynamic ARC4 byte-array (arc4.string / arc4.dynamic_bytes / uint8[]):
	// puya rejects makeARC4Encode(bytes, arc4.string) ("cannot encode bytes to (len+utf8[])").
	// Build [uint16 len][raw bytes] directly and reinterpret.
	// e.g. `string[] s; s[0] = "hi"` hits this path.
	// (Inverse of the abi.encode string-element fix in encodeFromArc4Bytes.)
	if (_target->wtype->kind() == awst::WTypeKind::ARC4DynamicArray
		&& (_value->wtype == awst::WType::bytesType()
			|| (_value->wtype && _value->wtype->kind() == awst::WTypeKind::Bytes)))
	{
		auto const* da = static_cast<awst::ARC4DynamicArray const*>(_target->wtype);
		if (da->elementType()
			&& ::puyasol::builder::computeEncodedElementSize(da->elementType()) == 1)
		{
			auto once = awst::makeEvalOnce(std::move(_value), m_loc);
			auto header = awst::makeUInt16Bytes(awst::makeLen(once, m_loc), m_loc);
			auto arc4Bytes = awst::makeConcat(std::move(header), once, m_loc);
			return awst::makeReinterpretCast(std::move(arc4Bytes), _target->wtype, m_loc);
		}
	}

	return awst::makeARC4Encode(std::move(_value), _target->wtype, m_loc);
}

void SolAssignment::maybePrePopulateBox(
	std::shared_ptr<awst::Expression> const& _target)
{
	// Centralized: a PARTIAL element write into a lazily-created state-var or mapping-entry box needs
	// the root box to exist first (else box_replace hits "no such box"). See
	// StorageMapper::makeEnsureRootBoxForWrite (the single source of truth, shared with the push/pop
	// path in SolArrayMethod and the mapping-entry field write in SolAssignmentStructField).
	if (auto stmt = builder::StorageMapper::makeEnsureRootBoxForWrite(
			m_ctx.typeMapper, _target, /*isResize=*/false, m_loc))
		m_ctx.queuePrePending(std::move(stmt));
}

} // namespace puyasol::builder::sol_ast

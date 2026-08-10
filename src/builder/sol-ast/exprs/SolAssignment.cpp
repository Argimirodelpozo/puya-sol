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
#include "builder/sol-ast/EffectScan.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/contract/ContractBuilder.h"
#include "builder/sol-ast/exprs/SolIndexAccess.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/storage/SlotHandleAccess.h"
#include "builder/storage/SlotWordCodec.h"
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
	if (auto r = tryHandleEvmStorageWrite())         return std::move(*r);
	if (auto r = tryHandleBlobRespill())             return std::move(*r);
	if (auto r = tryHandleStoragePointerReassign())  return std::move(*r);
	if (auto r = tryHandleMultiBoxArrayWrite())      return std::move(*r);
	if (auto r = tryHandleBoxedArrayElemWrite())     return std::move(*r);
	if (auto r = tryHandleOffsetStructRefFieldWrite()) return std::move(*r);
	if (auto r = tryHandleBlobAggregateWrite())      return std::move(*r);
	if (auto r = tryHandlePushAssignRewrite(op))     return std::move(*r);
	if (auto r = tryHandleSlotHandleElemWrite())     return std::move(*r);
	if (auto r = tryHandleSlotHandleFieldWrite())    return std::move(*r);

	// (2) Build target + value (if tryHandlePushAssignRewrite claimed, it already returned).
	// Legacy solc evaluates the RHS fully FIRST (verified vs 0.8.20 + py-evm:
	// `arr[j++] = j` stores the pre-increment j; `s.f = bump(s)` — the STORE
	// wins over the callee's write-back). Capture both sides' queued effects
	// and re-emit RHS-first: RHS pre, a pin of the RHS value, RHS write-backs
	// (hoisted before the store), then the LHS effects. Effect-free sides
	// re-emit byte-identically with no pin. Tuple assignments keep the plain
	// build (their element-wise handler owns sequencing).
	std::shared_ptr<awst::Expression> target, value;
	// Slot mode, tuple LHS: building a storage-element component (arrayData[3])
	// eagerly queues its bounds assert into prePending — BEFORE the RHS
	// snapshot the tuple handler emits, so `(.., arrayData[3]) = (.., grow(),
	// ..)` asserts on the PRE-grow length. EVM checks lvalues after the RHS:
	// capture the LHS build's effects and flush them after the tuple handler
	// has queued its snapshot (asserts still precede every store, which the
	// handler puts in pendingStatements).
	eb::ContractContext::OperandDeltas tupleLhsD;
	bool deferTupleLhsEffects = false;
	if (dynamic_cast<TupleType const*>(m_assignment.leftHandSide().annotation().type))
	{
		if (builder::evmStorageLayout())
		{
			target = m_ctx.buildScopedOperand(
				[&] { return buildExpr(m_assignment.leftHandSide()); }, tupleLhsD,
				/*_conditional=*/false);
			deferTupleLhsEffects = true;
			value = buildExpr(m_assignment.rightHandSide());
		}
		else
		{
			target = buildExpr(m_assignment.leftHandSide());
			value = buildExpr(m_assignment.rightHandSide());
		}
	}
	else
	{
		eb::ContractContext::OperandDeltas lhsD, rhsD;
		target = m_ctx.buildScopedOperand(
			[&] { return buildExpr(m_assignment.leftHandSide()); }, lhsD, /*_conditional=*/false);
		value = m_ctx.buildScopedOperand(
			[&] { return buildExpr(m_assignment.rightHandSide()); }, rhsD, /*_conditional=*/false);
		if (m_ctx.viaIRSequencing)
		{
			// via-IR keeps build order (LHS then RHS effects).
			m_ctx.restoreOperandDeltas(std::move(lhsD));
			m_ctx.restoreOperandDeltas(std::move(rhsD));
		}
		else
		{
			// Static scan: a directly-state-writing RHS call must execute (and
			// the store still win) before the target's key/index reads;
			// skipped for a plain local-value target. Symmetrically a
			// side-effecting LHS index needs the RHS value frozen first —
			// unless it only reads locals.
			bool lhsPlainLocal = false;
			if (auto const* lid = dynamic_cast<Identifier const*>(&m_assignment.leftHandSide()))
				if (auto const* lvd = dynamic_cast<VariableDeclaration const*>(
						lid->annotation().referencedDeclaration))
					lhsPlainLocal = lvd->isLocalVariable()
						&& !lvd->type()->dataStoredIn(DataLocation::Storage);
			bool staticNeed =
				(builder::EffectScan::mayWrite(m_assignment.rightHandSide()) && !lhsPlainLocal)
				|| (builder::EffectScan::mayWrite(m_assignment.leftHandSide())
					&& !builder::onlyLocalPure(m_assignment.rightHandSide()));
			bool reorder = !lhsD.empty() || !rhsD.post.empty() || staticNeed;
			value = m_ctx.emitSequencedOperand(std::move(rhsD), std::move(value), reorder, m_loc);
			for (auto& s: lhsD.pre)
				m_ctx.prePendingStatements.push_back(std::move(s));
			for (auto& s: lhsD.post)
				m_ctx.pendingStatements.push_back(std::move(s));
		}
	}

	// Tripwire (possible_solc item 6): a plain `=` must be a solc-legal
	// implicit conversion; a trip = wrong src/target annotation plumbing.
	// Compound ops follow binaryOperatorResult rules instead — skip; tuples
	// compare element-wise — skip.
	if (op == Token::Assign
		&& m_assignment.rightHandSide().annotation().type
		&& !dynamic_cast<TupleType const*>(m_assignment.rightHandSide().annotation().type)
		&& !dynamic_cast<TupleType const*>(m_assignment.leftHandSide().annotation().type))
		builder::TypeCoercion::assertImplicitlyConvertible(
			m_assignment.rightHandSide().annotation().type,
			m_assignment.leftHandSide().annotation().type, m_loc, "assignment");

	// (3) Per-shape early-outs.
	value = applyEnumRangeCheck(std::move(value), op);
	if (auto r = trySlotBasedArrayWrite(op, target, value))                     return std::move(*r);
	if (auto r = trySlotBasedScalarWrite(op, target, value))                    return std::move(*r);
	if (auto r = tryTupleAssignment(target, value))
	{
		if (deferTupleLhsEffects)
		{
			for (auto& st: tupleLhsD.pre)
				m_ctx.prePendingStatements.push_back(std::move(st));
			for (auto& st: tupleLhsD.post)
				m_ctx.pendingStatements.push_back(std::move(st));
		}
		return std::move(*r);
	}
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
SolAssignment::tryHandleBlobRespill()
{
	if (!builder::evmMemoryLayout()
		|| m_assignment.assignmentOperator() != Token::Assign)
		return std::nullopt;
	auto const* lid = dynamic_cast<Identifier const*>(&m_assignment.leftHandSide());
	if (!lid)
		return std::nullopt;
	auto const* lvd = dynamic_cast<VariableDeclaration const*>(
		lid->annotation().referencedDeclaration);
	if (!lvd
		|| lvd->referenceLocation() != VariableDeclaration::Location::Memory
		|| m_scope.findBlobAggregate(lvd->id()).empty())
		return std::nullopt;
	auto value = buildExpr(m_assignment.rightHandSide());
	if (!value)
		return std::nullopt;
	std::string offN = m_scope.findBlobAggregate(lvd->id());
	std::vector<std::shared_ptr<awst::Statement>> out;
	if (builder::emitBlobBackValue(m_ctx.typeMapper, lvd->type(),
			m_ctx.typeMapper.map(lvd->type()), std::move(value), offN,
			static_cast<int>(awst::NameGen::next("SolAssignment.respill")),
			m_loc, out))
		for (auto& st: out)
			m_ctx.queuePending(std::move(st));
	return std::shared_ptr<awst::Expression>{
		awst::makeZero(m_loc, awst::WType::biguintType())};
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryHandleEvmStorageWrite()
{
	if (!builder::evmStorageLayout())
		return std::nullopt;
	auto const& lhsExpr = m_assignment.leftHandSide();
	if (!EvmSlotLowering::isStorageStateRef(lhsExpr))
		return std::nullopt;

	// Storage POINTER rebind: `ptr = <storage ref>` on a storage local
	// re-points the biguint slot handle (runtime value — safe in conditionals,
	// unlike the compile-time alias rebinding of the named-cell model).
	if (m_assignment.assignmentOperator() == Token::Assign)
		if (auto const* lid = dynamic_cast<Identifier const*>(&lhsExpr))
			if (auto const* lvd = dynamic_cast<VariableDeclaration const*>(
					lid->annotation().referencedDeclaration))
				if (!lvd->isStateVariable() && lvd->isLocalVariable()
					&& lvd->referenceLocation() == VariableDeclaration::Location::Storage)
				{
					auto const* rhsT = m_assignment.rightHandSide().annotation().type;
					bool rhsStorage = rhsT
						&& (dynamic_cast<MappingType const*>(rhsT)
							|| (dynamic_cast<ReferenceType const*>(rhsT)
								&& rhsT->dataStoredIn(DataLocation::Storage)));
					if (rhsStorage)
					{
						EvmSlotLowering low(m_ctx, m_scope, m_loc);
						auto r = low.resolve(m_assignment.rightHandSide());
						if (!r)
							return std::shared_ptr<awst::Expression>{
								awst::makeZero(m_loc, awst::WType::biguintType())};
						// PRE-pending, and the expression VALUE is the pointer:
						// `(m = m2)[2] = 21` indexes the assignment's value —
						// makeZero here sent the write to SLOT 0 (= the first
						// mapping!), and a post-queued rebind ran after it.
						m_ctx.prePendingStatements.push_back(
							awst::makeAssignmentStatement(
								awst::makeVarExpression(lvd->name(),
									awst::WType::biguintType(), m_loc),
								r->slot, m_loc));
						return std::shared_ptr<awst::Expression>{
							awst::makeVarExpression(lvd->name(),
								awst::WType::biguintType(), m_loc)};
					}
				}

	auto const* lhsType = lhsExpr.annotation().type;
	// bytes/string element WRITE: whole-value read-modify-write through the
	// short/long subroutines (replace3 at the asserted index).
	if (m_assignment.assignmentOperator() == Token::Assign)
		if (auto const* bia =
			dynamic_cast<solidity::frontend::IndexAccess const*>(&lhsExpr))
			if (auto const* bbt = dynamic_cast<ArrayType const*>(
					bia->baseExpression().annotation().type);
				bbt && bbt->isByteArrayOrString()
				&& bbt->dataStoredIn(DataLocation::Storage)
				&& EvmSlotLowering::isStorageStateRef(lhsExpr)
				&& bia->indexExpression())
			{
				EvmSlotLowering low(m_ctx, m_scope, m_loc);
				auto baseAddr = low.resolve(bia->baseExpression());
				if (!baseAddr)
					return std::shared_ptr<awst::Expression>{
						awst::makeZero(m_loc, awst::WType::biguintType())};
				baseAddr->slot = awst::makeEvalOnce(baseAddr->slot, m_loc);
				baseAddr->solType = bbt;
				auto whole = low.readBytesValue(*baseAddr);
				if (whole && whole->wtype != awst::WType::bytesType())
					whole = awst::makeAsBytes(std::move(whole), m_loc);
				std::string nm = "__evm_bw_" + std::to_string(
					awst::NameGen::next("SolAssignment.bytesElemW"));
				m_ctx.queuePending(awst::makeAssignmentStatement(
					awst::makeVarExpression(nm, awst::WType::bytesType(), m_loc),
					std::move(whole), m_loc));
				auto wv = [&]() {
					return awst::makeVarExpression(
						nm, awst::WType::bytesType(), m_loc);
				};
				auto idx = buildExpr(*bia->indexExpression());
				if (!idx)
					return std::nullopt;
				{
					std::vector<std::shared_ptr<awst::Statement>> idxPre;
					idx = TypeCoercion::checkedIndexToUint64(idxPre, std::move(idx), m_loc);
					for (auto& ps: idxPre)
						m_ctx.queuePending(std::move(ps));
				}
				idx = awst::makeEvalOnce(std::move(idx), m_loc);
				auto inBounds = awst::makeNumericCompare(idx,
					awst::NumericComparison::Lt,
					awst::makeLen(wv(), m_loc), m_loc);
				m_ctx.queuePending(awst::makeExpressionStatement(
					awst::makeAssert(std::move(inBounds), m_loc,
						"bytes index out of range"), m_loc));
				auto value = buildExpr(m_assignment.rightHandSide());
				if (!value)
					return std::nullopt;
				if (value->wtype != awst::WType::bytesType())
					value = awst::makeAsBytes(std::move(value), m_loc);
				value = awst::makeExtract(std::move(value), 0, 1, m_loc);
				value = awst::makeEvalOnce(std::move(value), m_loc);
				std::vector<std::shared_ptr<awst::Statement>> writesE;
				low.writeBytesValue(*baseAddr,
					awst::makeReplace3(wv(), idx, value, m_loc), writesE);
				for (auto& stE: writesE)
					m_ctx.queuePending(std::move(stE));
				return std::shared_ptr<awst::Expression>{value};
			}
	// Whole FIXED-array assignment: resolve the LHS slot and delegate to the
	// slot-handle array writer (handles storage→storage slot copies, value
	// RHS with EVM zero-fill, struct elements).
	if (m_assignment.assignmentOperator() == Token::Assign)
		if (auto const* lat = dynamic_cast<ArrayType const*>(lhsType);
			lat && !lat->isDynamicallySized() && !lat->isByteArrayOrString())
		{
			EvmSlotLowering low(m_ctx, m_scope, m_loc);
			auto laddr = low.resolve(lhsExpr);
			if (!laddr)
				return std::shared_ptr<awst::Expression>{
					awst::makeZero(m_loc, awst::WType::biguintType())};
			std::shared_ptr<awst::Expression> value;
			auto const& rhsExpr = m_assignment.rightHandSide();
			auto const* rat = dynamic_cast<ArrayType const*>(rhsExpr.annotation().type);
			if (EvmSlotLowering::isStorageStateRef(rhsExpr))
			{
				auto raddr = low.resolve(rhsExpr);
				if (!raddr)
					return std::shared_ptr<awst::Expression>{
						awst::makeZero(m_loc, awst::WType::biguintType())};
				// Same element type + length → raw slot copy. DIFFERENT shapes
				// (bytes8[9] = bytes17[10], packed vs full-slot) need a
				// per-element read/convert/write loop — a raw copy would smear
				// the source packing into the target's layout.
				if (rat && !rat->isDynamicallySized()
					&& lat->baseType()->identifier() != rat->baseType()->identifier())
				{
					unsigned lhsLen = static_cast<unsigned>(lat->length());
					unsigned rhsLen = static_cast<unsigned>(rat->length());
					if (lhsLen > 64)
					{
						Logger::instance().error(
							"--evm-storage-layout: converting array copy of length "
							+ std::to_string(lhsLen) + " exceeds the unroll cap (64)",
							m_loc);
						return std::shared_ptr<awst::Expression>{
							awst::makeZero(m_loc, awst::WType::biguintType())};
					}
					auto pinB = [&](std::shared_ptr<awst::Expression> e, char const* tag) {
						std::string nm = std::string("__evm_cpy") + tag + "_"
							+ std::to_string(awst::NameGen::next("SolAssignment.evmCpy"));
						m_ctx.queuePending(awst::makeAssignmentStatement(
							awst::makeVarExpression(nm, awst::WType::biguintType(), m_loc),
							std::move(e), m_loc));
						return nm;
					};
					std::string lNm = pinB(laddr->slot, "l");
					std::string rNm = pinB(raddr->slot, "r");
					auto lBase = [&]() { return awst::makeVarExpression(
						lNm, awst::WType::biguintType(), m_loc); };
					auto rBase = [&]() { return awst::makeVarExpression(
						rNm, awst::WType::biguintType(), m_loc); };
					auto const* lbw = dynamic_cast<awst::BytesWType const*>(
						m_ctx.typeMapper.map(lat->baseType()));
					for (unsigned j = 0; j < lhsLen; ++j)
					{
						auto jc = [&]() { return awst::makeIntegerConstant(
							j, m_loc, awst::WType::biguintType()); };
						auto la = low.elemAddr(lBase(), jc(), lat->baseType());
						std::vector<std::shared_ptr<awst::Statement>> ws;
						if (j < rhsLen)
						{
							auto ra = low.elemAddr(rBase(), jc(), rat->baseType());
							auto v = low.readValue(ra);
							// bytesN -> bytesM: LEFT-aligned (right-pad / truncate)
							if (lbw && lbw->length().has_value())
							{
								unsigned toN = static_cast<unsigned>(*lbw->length());
								unsigned fromN = ra.size;
								if (toN > fromN)
									v = awst::makeConcat(awst::makeAsBytes(std::move(v), m_loc),
										awst::makeBzero(static_cast<int>(toN - fromN), m_loc),
										m_loc);
								else if (toN < fromN)
									v = awst::makeExtract(awst::makeAsBytes(std::move(v), m_loc),
										0, static_cast<int>(toN), m_loc);
								v = awst::makeReinterpretCast(std::move(v), la.wtype, m_loc);
							}
							else
								v = low.coerceToNative(std::move(v), la);
							low.writeValue(la, std::move(v), ws);
						}
						else if (lbw && lbw->length().has_value())
							low.writeValue(la, awst::makeBytesConstant(
								std::vector<uint8_t>(static_cast<size_t>(*lbw->length()), 0),
								m_loc, awst::BytesEncoding::Base16, la.wtype), ws);
						else
							low.writeValue(la, awst::makeIntegerConstant(
								"0", m_loc, awst::WType::biguintType()), ws);
						for (auto& st: ws)
							m_ctx.queuePending(std::move(st));
					}
					return std::shared_ptr<awst::Expression>{
						awst::makeZero(m_loc, awst::WType::biguintType())};
				}
				value = raddr->slot;   // biguint -> slot-level copy
			}
			else
				value = buildExpr(rhsExpr);
			if (!value)
				return std::nullopt;
			// FIXED array of DYNAMIC elements (uint[][2] = calldata/memory):
			// per-element head slicing + inner length/keccak-region writes —
			// only writeArrayValue models that; the slot-route writer below
			// smears the head/tail encoding across raw words.
			if (auto const* lelem2 = dynamic_cast<ArrayType const*>(lat->baseType());
				lelem2 && lelem2->isDynamicallySized()
				&& !lat->isDynamicallySized() && !lat->isByteArrayOrString())
			{
				EvmSlotLowering lowFD(m_ctx, m_scope, m_loc);
				EvmSlotLowering::Addr la2 = *laddr;
				la2.solType = lhsType;
				la2.wtype = m_ctx.typeMapper.map(lhsType);
				std::vector<std::shared_ptr<awst::Statement>> wsFD;
				if (lowFD.writeArrayValue(la2, lat, std::move(value), wsFD))
				{
					for (auto& st: wsFD)
						m_ctx.queuePending(std::move(st));
					return std::shared_ptr<awst::Expression>{
						awst::makeZero(m_loc, awst::WType::biguintType())};
				}
				return std::shared_ptr<awst::Expression>{
					awst::makeZero(m_loc, awst::WType::biguintType())};
			}
			if (auto r = trySlotBasedArrayWrite(Token::Assign, laddr->slot, value))
				return std::move(*r);
			return std::shared_ptr<awst::Expression>{
				awst::makeZero(m_loc, awst::WType::biguintType())};
		}
	// Whole-STRUCT assignment (`st = S(...)`, `bridges[b].minterParams = p`):
	// split into per-member slot writes, recursing into nested members.
	if (m_assignment.assignmentOperator() == Token::Assign)
		if (auto const* lst = dynamic_cast<StructType const*>(lhsType))
		{
			EvmSlotLowering low(m_ctx, m_scope, m_loc);
			auto laddr = low.resolve(lhsExpr);
			if (!laddr)
				return std::shared_ptr<awst::Expression>{
					awst::makeZero(m_loc, awst::WType::biguintType())};
			std::shared_ptr<awst::Expression> value;
			auto const& rhsExpr2 = m_assignment.rightHandSide();
			if (EvmSlotLowering::isStorageStateRef(rhsExpr2)
				&& dynamic_cast<StructType const*>(rhsExpr2.annotation().type))
			{
				// storage → storage: materialise then re-split (correct, if
				// not minimal; slot-copy would need identical layouts anyway)
				auto raddr = low.resolve(rhsExpr2);
				if (raddr)
				{
					EvmSlotLowering::Addr ra = *raddr;
					ra.solType = rhsExpr2.annotation().type;
					ra.wtype = m_ctx.typeMapper.map(ra.solType);
					value = low.readStructValue(ra);
				}
			}
			else
				value = buildExpr(rhsExpr2);
			if (!value)
				return std::shared_ptr<awst::Expression>{
					awst::makeZero(m_loc, awst::WType::biguintType())};
			laddr->solType = lhsType;
			laddr->wtype = m_ctx.typeMapper.map(lhsType);
			std::vector<std::shared_ptr<awst::Statement>> out;
			if (low.writeStructValue(*laddr, std::move(value), out))
				for (auto& st2: out)
					m_ctx.queuePending(std::move(st2));
			return std::shared_ptr<awst::Expression>{
				awst::makeZero(m_loc, awst::WType::biguintType())};
		}
	if (lhsType && !lhsType->isValueType()
		&& EvmSlotLowering::isBytesLike(lhsType)
		&& m_assignment.assignmentOperator() == Token::Assign)
	{
		EvmSlotLowering low(m_ctx, m_scope, m_loc);
		auto addr = low.resolve(lhsExpr);
		if (!addr)
			return std::shared_ptr<awst::Expression>{
				awst::makeZero(m_loc, awst::WType::biguintType())};
		auto value = buildExpr(m_assignment.rightHandSide());
		if (!value)
			return std::nullopt;
		std::string valNm = "__evm_aval_"
			+ std::to_string(awst::NameGen::next("SolAssignment.evmAssignVal"));
		auto const* valW = value->wtype;
		m_ctx.queuePending(awst::makeAssignmentStatement(
			awst::makeVarExpression(valNm, valW, m_loc), std::move(value), m_loc));
		std::vector<std::shared_ptr<awst::Statement>> out;
		low.writeBytesValue(*addr,
			awst::makeVarExpression(valNm, valW, m_loc), out);
		for (auto& st: out)
			m_ctx.queuePending(std::move(st));
		return std::shared_ptr<awst::Expression>{
			awst::makeVarExpression(valNm, valW, m_loc)};
	}
	if (auto const* lat = dynamic_cast<ArrayType const*>(lhsType);
		lat && lat->dataStoredIn(DataLocation::Storage)
		&& !lat->isByteArrayOrString()
		&& m_assignment.assignmentOperator() == Token::Assign)
	{
		EvmSlotLowering low(m_ctx, m_scope, m_loc);
		auto addr = low.resolve(lhsExpr);
		if (!addr)
			return std::shared_ptr<awst::Expression>{
				awst::makeZero(m_loc, awst::WType::biguintType())};
		auto const& rhsExprA = m_assignment.rightHandSide();
		auto const* rt = rhsExprA.annotation().type;
		std::shared_ptr<awst::Expression> value;
		if (auto const* rat = dynamic_cast<ArrayType const*>(rt);
			rat && rat->dataStoredIn(DataLocation::Storage))
		{
			// storage → storage: materialise, then re-split element-wise
			auto raddr = low.resolve(rhsExprA);
			if (raddr)
			{
				EvmSlotLowering::Addr ra = *raddr;
				ra.solType = rt;
				ra.wtype = m_ctx.typeMapper.map(rt);
				value = low.readArrayValue(ra, rat);
			}
		}
		else
			value = buildExpr(rhsExprA);
		if (!value)
			return std::shared_ptr<awst::Expression>{
				awst::makeZero(m_loc, awst::WType::biguintType())};
		// Element-type widening (int8[]→int16[]: per-element SIGN-extend) before
		// the slot writer — its metrics come from the LHS type, so a narrower
		// RHS mis-slices (dynarr) or zero-extends (fixed; chop_sign_bits).
		// Widen helpers ONLY — the generic encode fallback would reject the
		// same-width static→dynamic shape the writer already consumes natively
		// (uint8[5] into uint8[] storage, inline_array_return).
		if (auto const* tgtW = m_ctx.typeMapper.map(lhsType);
			tgtW && value->wtype && value->wtype != tgtW
			&& (value->wtype->kind() == awst::WTypeKind::ARC4StaticArray
				|| value->wtype->kind() == awst::WTypeKind::ARC4DynamicArray))
		{
			// Pin FIRST: the dynamic widen emits its loop via prePending during
			// the call, and those statements read the pinned source var.
			std::string wn = "__evm_wsrc_"
				+ std::to_string(awst::NameGen::next("SolAssignment.evmWidenSrc"));
			m_ctx.prePendingStatements.push_back(
				awst::makeAssignmentStatement(
					awst::makeVarExpression(wn, awst::WType::bytesType(), m_loc),
					awst::makeAsBytes(value, m_loc), m_loc));
			auto mkSrc = [&, wn]() {
				return awst::makeVarExpression(wn, awst::WType::bytesType(), m_loc);
			};
			std::shared_ptr<awst::Expression> widened;
			if (tgtW->kind() == awst::WTypeKind::ARC4StaticArray)
				widened = builder::tryWidenArc4StaticArrayInt(
					value->wtype, tgtW, mkSrc, m_loc);
			else if (tgtW->kind() == awst::WTypeKind::ARC4DynamicArray)
				widened = builder::tryWidenArc4DynamicArrayInt(
					value->wtype, tgtW, mkSrc,
					[this](std::shared_ptr<awst::Statement> _s) {
						m_ctx.prePendingStatements.push_back(std::move(_s));
					},
					m_loc);
			if (widened)
				value = std::move(widened);
		}
		value = TypeCoercion::coerceForAssignment(
			std::move(value), m_ctx.typeMapper.map(lhsType), m_loc);
		addr->solType = lhsType;
		addr->wtype = m_ctx.typeMapper.map(lhsType);
		std::vector<std::shared_ptr<awst::Statement>> out;
		if (low.writeArrayValue(*addr, lat, std::move(value), out))
			for (auto& st2: out)
				m_ctx.queuePending(std::move(st2));
		return std::shared_ptr<awst::Expression>{
			awst::makeZero(m_loc, awst::WType::biguintType())};
	}
	if (!lhsType || !lhsType->isValueType())
	{
		Logger::instance().error(
			"--evm-storage-layout: aggregate storage assignment not yet supported",
			m_loc);
		return std::shared_ptr<awst::Expression>{
			awst::makeZero(m_loc, awst::WType::biguintType())};
	}

	// RHS FIRST (solc legacy + viaYul): `arr[j++] = j` stores the
	// PRE-increment j, and callee write-backs in `s.f = bump(s)` land before
	// the lvalue's index/key effects. Pin the built value via PREpending so it
	// is captured before resolve() queues those effects (a post-queued pin
	// would read the mutated state no matter the build order).
	Token op = m_assignment.assignmentOperator();
	auto rhsBuilt = buildExpr(m_assignment.rightHandSide());
	if (!rhsBuilt)
		return std::nullopt;
	// Panic 0x21 on out-of-range enum stores (asm-scribbled locals) — this
	// early-out otherwise bypasses the default path's check entirely.
	rhsBuilt = applyEnumRangeCheck(std::move(rhsBuilt), op);
	std::string rhsNm = "__evm_arhs_"
		+ std::to_string(awst::NameGen::next("SolAssignment.evmAssignRhs"));
	auto const* rhsW = rhsBuilt->wtype;
	m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(rhsNm, rhsW, m_loc), std::move(rhsBuilt), m_loc));

	EvmSlotLowering low(m_ctx, m_scope, m_loc);
	auto addr = low.resolve(lhsExpr);
	if (!addr)
		return std::shared_ptr<awst::Expression>{
			awst::makeZero(m_loc, awst::WType::biguintType())};
	// The slot may embed side-effecting key/index builds and is referenced by
	// both the compound read and the write — pin it. All uses land in the one
	// write statement, so SingleEvaluation is safe.
	addr->slot = awst::makeEvalOnce(addr->slot, m_loc);

	std::shared_ptr<awst::Expression> value =
		awst::makeVarExpression(rhsNm, rhsW, m_loc);
	if (op != Token::Assign)
	{
		auto current = low.readValue(*addr);
		value = applyCompoundAssignment(op, current, std::move(value));
	}

	value = low.coerceToNative(std::move(value), *addr);
	if (!value)
		return std::nullopt;

	// Pin the value to a temp: the write reads it AND the assignment
	// EXPRESSION evaluates to it (chained `a = b = v` — returning a zero
	// sentinel here wrote 0 through the outer link; storage_packed_array_copy
	// ctor's `_y[8] = _y[9] = ...`). Single-use pins are copy-propagated by
	// the backend. PREpending, not pending: the OUTER link of a chain pins
	// its RHS (this var) via prePending, which flushes before the statement —
	// a post-queued assignment here left that read undefined.
	std::string valNm = "__evm_aval_"
		+ std::to_string(awst::NameGen::next("SolAssignment.evmAssignVal"));
	auto const* valW = value->wtype;
	m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(valNm, valW, m_loc), std::move(value), m_loc));
	std::vector<std::shared_ptr<awst::Statement>> out;
	low.writeValue(*addr, awst::makeVarExpression(valNm, valW, m_loc), out);
	for (auto& st: out)
		m_ctx.prePendingStatements.push_back(std::move(st));
	return std::shared_ptr<awst::Expression>{
		awst::makeVarExpression(valNm, valW, m_loc)};
}

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

	// rhs is itself a slot handle → copy the SOURCE footprint's slots, then
	// zero-fill the target's tail (EVM partial-assign: uint256[4] = uint256[2]
	// copies 2 and DELETES the rest — copying all 4 sequentially also re-read
	// freshly written dst slots when the regions adjoin).
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
		auto srcSlots = slots;
		if (auto const* rhsArr = dynamic_cast<ArrayType const*>(
				m_assignment.rightHandSide().annotation().type);
			rhsArr && !rhsArr->isDynamicallySized())
			srcSlots = rhsArr->storageSize();
		auto srcVar = [&]() { return _value; };
		auto dstVar = [&]() { return _target; };
		unsigned n = static_cast<unsigned>(slots);
		unsigned srcN = static_cast<unsigned>(
			srcSlots < slots ? srcSlots : slots);
		for (unsigned j = 0; j < n; ++j)
		{
			auto jc = [&]() { return awst::makeIntegerConstant(j, m_loc, awst::WType::biguintType()); };
			auto dst = awst::makeBigUIntBinOp(dstVar(), awst::BigUIntBinaryOperator::Add, jc(), m_loc);
			if (j < srcN)
			{
				auto src = awst::makeBigUIntBinOp(srcVar(), awst::BigUIntBinaryOperator::Add, jc(), m_loc);
				out.push_back(builder::SlotHandleAccess::writeSlot(
					std::move(dst), builder::SlotHandleAccess::readSlot(std::move(src), m_loc), m_loc));
			}
			else
				out.push_back(builder::SlotHandleAccess::writeSlot(
					std::move(dst),
					awst::makeIntegerConstant("0", m_loc, awst::WType::biguintType()), m_loc));
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

	// int8[2] memory → int16[2] storage: per-element SIGN-extend first — the
	// packed writes below slice the value at the LHS element width, so a
	// narrower source mis-slices / zero-extends (chop_sign_bits).
	std::shared_ptr<awst::Expression> value = _value;
	if (auto const* tgtW = m_ctx.typeMapper.map(arrType);
		tgtW && value->wtype != tgtW
		&& value->wtype->kind() == awst::WTypeKind::ARC4StaticArray
		&& tgtW->kind() == awst::WTypeKind::ARC4StaticArray)
	{
		std::string wn = "__slotw_wsrc_" + std::to_string(m_assignment.id());
		out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(wn, awst::WType::bytesType(), m_loc),
			awst::makeAsBytes(value, m_loc), m_loc));
		auto mkSrc = [&, wn]() {
			return awst::makeVarExpression(wn, awst::WType::bytesType(), m_loc);
		};
		if (auto widened = builder::tryWidenArc4StaticArrayInt(
				value->wtype, tgtW, mkSrc, m_loc))
			value = std::move(widened);
	}

	// bind target + value once
	std::string tBase = "__slotw_base_" + std::to_string(m_assignment.id());
	out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(tBase, awst::WType::biguintType(), m_loc), _target, m_loc));
	auto baseVar = [&]() { return awst::makeVarExpression(tBase, awst::WType::biguintType(), m_loc); };
	std::string tVal = "__slotw_val_" + std::to_string(m_assignment.id());
	out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(tVal, value->wtype, m_loc), value, m_loc));
	auto valVar = [&]() { return awst::makeVarExpression(tVal, value->wtype, m_loc); };

	awst::WType const* elemWtype;
	if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(value->wtype))
		elemWtype = sa->elementType();
	else if (auto const* da = dynamic_cast<awst::ARC4DynamicArray const*>(value->wtype))
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
			else if (auto const* vw = elemVal->wtype;
				vw && vw != awst::WType::biguintType()
				&& (vw->kind() == awst::WTypeKind::Bytes
					|| [&]{
						auto const* sa =
							dynamic_cast<awst::ARC4StaticArray const*>(vw);
						auto const* eu = sa ? dynamic_cast<awst::ARC4UIntN const*>(
							sa->elementType()) : nullptr;
						return eu && eu->n() == 8;
					}()))
			{
				// BYTE-STRING handles only (external fn-ptr byte[12] in its
				// 24-byte share, bytesN): the codec owns the in-window
				// alignment; its packed form's biguint IS the canonical
				// element value. Aggregate element wtypes (nested arrays,
				// structs) must pass through untouched — the codec rejects
				// them ("unsupported type in packed storage slot").
				elemVal = awst::makeAsBiguint(
					builder::SlotWordCodec::nativeToPackedBytes(
						std::move(elemVal), vw, layout.size, m_loc), m_loc);
			}
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
	// arc4.bool is an ARC4BasicWType of kind `Basic` (same kind as native bool),
	// so the switch misses it — assigning a native bool into an arc4.bool slot
	// (a `bool[]` element write `flags[i] = v`, or an arc4.bool struct field)
	// then leaves the value native bool and puya rejects it ("target type differs
	// from expression value type"). Encode it. (Read side fixed in 19d7e1ba32.)
	if (_target->wtype == awst::WType::arc4BoolType())
		targetIsArc4 = true;
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

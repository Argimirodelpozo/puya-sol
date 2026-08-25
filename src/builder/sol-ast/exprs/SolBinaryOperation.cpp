/// @file SolBinaryOperation.cpp — migrated from BinaryOperationBuilder.cpp.

#include "builder/sol-types/SolcConstFold.h"
#include "awst/NameGen.h"
#include "builder/sol-ast/EffectScan.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/sol-ast/exprs/SolBinaryOperation.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/sol-eb/BuilderOps.h"
#include "builder/sol-eb/BigUIntMathHelpers.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>
#include <libsolutil/Numeric.h>

#include <sstream>

namespace puyasol::builder::sol_ast
{


using namespace solidity::frontend;
using Token = solidity::frontend::Token;

SolBinaryOperation::SolBinaryOperation(
	eb::ContractContext& _ctx,
	BinaryOperation const& _node)
	: SolExpression(_ctx, _node), m_binOp(_node)
{
}

std::shared_ptr<awst::Expression> SolBinaryOperation::tryUserDefinedOp()
{
	auto const* userFunc = *m_binOp.annotation().userDefinedFunction;
	if (!userFunc) return nullptr;

	auto const* symbol = m_ctx.functionSymbols.resolve(userFunc->id());
	if (!symbol)
		return nullptr;

	auto left = buildExpr(m_binOp.leftExpression());
	auto right = buildExpr(m_binOp.rightExpression());
	auto* resultType = m_ctx.typeMapper.map(m_binOp.annotation().type);

	auto call = awst::makeSubroutineCall(awst::SubroutineID{*symbol}, resultType, m_loc);
	awst::pushCallArg(call->args, userFunc->parameters()[0]->name(), std::move(left));
	awst::pushCallArg(call->args, userFunc->parameters()[1]->name(), std::move(right));
	return call;
}

std::shared_ptr<awst::Expression> SolBinaryOperation::tryConstantFold()
{
	// Solc folded the whole op → emit its value (the canonical constant path).
	if (auto folded = builder::SolcConstFold::foldAnnotated(m_binOp, m_ctx.typeMapper, m_loc))
		return folded;
	// intN-typed constant expression (arithmetic over constant variables) —
	// folds only under foldTyped's every-node-in-range guard, so intermediate
	// overflow/wrap semantics are never swallowed.
	return builder::SolcConstFold::foldTyped(m_binOp, m_loc);
}

std::shared_ptr<awst::Expression> SolBinaryOperation::trySolShortCircuit()
{
	auto op = m_binOp.getOperator();
	if (op != Token::And && op != Token::Or)
		return nullptr;

	// Left evaluates unconditionally and FIRST: hoist its write-backs (post-
	// pendings) so the RHS observes them (`bump(s) > 0 && s.f == 6` — the RHS
	// runs at call-return state on EVM, not pre-write-back). Pin the left value
	// before the hoist so it keeps its pre-write-back reads.
	auto left = m_ctx.pinIfWriteBacks(
		m_ctx.lower(m_binOp.leftExpression(), false), m_loc);

	// Build the RHS, capturing any side effects it pushes (a checked op's overflow/zero assert, a
	// `**` square-and-multiply loop, a nested short-circuit, a call's write-back). They must run
	// ONLY when the RHS is evaluated, else `b != 0 && a / b > x` divides by zero when b == 0
	// (EVM short-circuits). lowerOperand owns the explicit effect capture.
	auto loweredRight = m_ctx.lower(m_binOp.rightExpression());
	auto right = std::move(loweredRight.value);
	auto rhsD = std::move(loweredRight.effects);

	auto boolOp = (op == Token::And)
		? awst::BinaryBooleanOperator::And : awst::BinaryBooleanOperator::Or;
	if (rhsD.empty())
		return awst::makeBoolBinOp(std::move(left), boolOp, std::move(right), m_loc);

	// RHS has side effects -> gate them behind the condition (mirror the ternary, SolConditional):
	//   a && b  ==  a ? b : false   (b runs iff a is true)
	//   a || b  ==  a ? true : b    (b runs iff a is false)
	std::string tempName = "__sc_" + std::to_string(awst::NameGen::next("SolBinaryOperation.s_counter"));
	auto* boolType = awst::WType::boolType();
	auto tempVar = [&] { return awst::makeVarExpression(tempName, boolType, m_loc); };

	// RHS: its captured pre-statements run, then temp = right, then its
	// write-backs — all gated with the operand (OperandPlan block).
	auto rhsBlock = eb::ContractContext::makeScopedResultBlock(
		std::move(rhsD.pre), tempVar(), std::move(right), m_loc, std::move(rhsD.post));
	// Short-circuit branch: temp = the constant that skips the RHS.
	auto shortBlock = eb::ContractContext::makeScopedResultBlock(
		{}, tempVar(), awst::makeBoolConstant(op == Token::Or, m_loc), m_loc);

	if (op == Token::And)
		m_ctx.preEffects().push_back(
			awst::makeIfElse(std::move(left), std::move(rhsBlock), std::move(shortBlock), m_loc));
	else
		m_ctx.preEffects().push_back(
			awst::makeIfElse(std::move(left), std::move(shortBlock), std::move(rhsBlock), m_loc));

	return tempVar();
}

std::shared_ptr<awst::Expression> SolBinaryOperation::trySolEbDispatch(
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right)
{
	auto solOp = m_binOp.getOperator();
	auto* leftSolType = m_binOp.leftExpression().annotation().type;
	auto* rightSolType = m_binOp.rightExpression().annotation().type;

	// Use common type for arithmetic overflow checks (e.g. uint8+uint16 → uint16).
	auto const* commonSolType = m_binOp.annotation().commonType;

	auto leftBuilder = m_ctx.builderForInstance(leftSolType, _left);
	if (!leftBuilder && commonSolType)
		leftBuilder = m_ctx.builderForInstance(commonSolType, _left);
	if (!leftBuilder) return nullptr;

	auto rightBuilder = m_ctx.builderForInstance(rightSolType, _right);
	if (!rightBuilder && commonSolType)
		rightBuilder = m_ctx.builderForInstance(commonSolType, _right);
	if (!rightBuilder && leftSolType)
		rightBuilder = m_ctx.builderForInstance(leftSolType, _right);
	if (!rightBuilder) return nullptr;

	// Try comparison operators
	eb::BuilderComparisonOp cmpOp;
	bool hasCmpOp = true;
	switch (solOp)
	{
	case Token::Equal:              cmpOp = eb::BuilderComparisonOp::Eq; break;
	case Token::NotEqual:           cmpOp = eb::BuilderComparisonOp::Ne; break;
	case Token::LessThan:           cmpOp = eb::BuilderComparisonOp::Lt; break;
	case Token::LessThanOrEqual:    cmpOp = eb::BuilderComparisonOp::Lte; break;
	case Token::GreaterThan:        cmpOp = eb::BuilderComparisonOp::Gt; break;
	case Token::GreaterThanOrEqual: cmpOp = eb::BuilderComparisonOp::Gte; break;
	default: hasCmpOp = false; break;
	}
	if (hasCmpOp)
	{
		// Drive operand conversion off solc's commonType: coerce both integer
		// operands to the comparison's common type (canonicalising), so compare()
		// gets uniform same-width canonical operands — one solc-driven point that
		// replaces the per-operand sign-extension / narrowConstIfNegative inside
		// compare(). Non-integer comparisons (address/bytes) keep their builders.
		auto* cmpL = leftBuilder.get();
		auto* cmpR = rightBuilder.get();
		std::unique_ptr<eb::InstanceBuilder> clHold, crHold;
		if (commonSolType && dynamic_cast<IntegerType const*>(commonSolType))
		{
			// Solc-convertibility tripwire: both operands must be
			// solc-implicitly-convertible to the comparison's common type.
			builder::TypeCoercion::assertImplicitlyConvertible(
				leftSolType, commonSolType, m_loc, "binop common-type (cmp)");
			builder::TypeCoercion::assertImplicitlyConvertible(
				rightSolType, commonSolType, m_loc, "binop common-type (cmp)");
			auto* commonW = m_ctx.typeMapper.map(commonSolType);
			auto lv = builder::TypeCoercion::coerceToCommonInt(_left, leftSolType, commonW, m_loc);
			auto rv = builder::TypeCoercion::coerceToCommonInt(_right, rightSolType, commonW, m_loc);
			clHold = m_ctx.builderForInstance(commonSolType, lv);
			crHold = m_ctx.builderForInstance(commonSolType, rv);
			if (clHold && crHold) { cmpL = clHold.get(); cmpR = crHold.get(); }
		}
		auto result = cmpL->compare(*cmpR, cmpOp, m_loc);
		if (result) return result->resolve();
	}

	// Try arithmetic/bitwise operators
	eb::BuilderBinaryOp builderOp;
	bool hasBinOp = true;
	switch (solOp)
	{
	case Token::Add: case Token::AssignAdd: builderOp = eb::BuilderBinaryOp::Add; break;
	case Token::Sub: case Token::AssignSub: builderOp = eb::BuilderBinaryOp::Sub; break;
	case Token::Mul: case Token::AssignMul: builderOp = eb::BuilderBinaryOp::Mult; break;
	case Token::Div: case Token::AssignDiv: builderOp = eb::BuilderBinaryOp::FloorDiv; break;
	case Token::Mod: case Token::AssignMod: builderOp = eb::BuilderBinaryOp::Mod; break;
	case Token::Exp: builderOp = eb::BuilderBinaryOp::Pow; break;
	case Token::SHL: case Token::AssignShl: builderOp = eb::BuilderBinaryOp::LShift; break;
	case Token::SHR: case Token::SAR: case Token::AssignShr: case Token::AssignSar:
		builderOp = eb::BuilderBinaryOp::RShift; break;
	case Token::BitOr: case Token::AssignBitOr: builderOp = eb::BuilderBinaryOp::BitOr; break;
	case Token::BitXor: case Token::AssignBitXor: builderOp = eb::BuilderBinaryOp::BitXor; break;
	case Token::BitAnd: case Token::AssignBitAnd: builderOp = eb::BuilderBinaryOp::BitAnd; break;
	default: hasBinOp = false; break;
	}
	if (hasBinOp)
	{
		// Drive operand conversion off solc's commonType (mirrors the comparison path): coerce BOTH
		// integer operands to commonType, canonicalising — sign-extend a narrower SIGNED operand to
		// the common width. Else `int128 & int16(-1)` ANDs the raw 16-bit value, not the sign-extended
		// int128 (the D-residual from solc-todo.md opportunity D; the narrower-RIGHT case was active,
		// not just latent). Shifts keep the left's own type and the right is the untouched shift
		// amount, so skip them; a non-integer commonType keeps the bare left reinterpret.
		auto* arithBuilder = leftBuilder.get();
		auto* arithRight = rightBuilder.get();
		std::unique_ptr<eb::InstanceBuilder> commonLeftHold, commonRightHold;
		bool isShift = (builderOp == eb::BuilderBinaryOp::LShift
			|| builderOp == eb::BuilderBinaryOp::RShift);
		if (commonSolType && dynamic_cast<IntegerType const*>(commonSolType) && !isShift)
		{
			// Solc-convertibility tripwire. Pow excluded: solc's commonType
			// for `**` is the BASE type — the exponent is legitimately not
			// convertible to it.
			if (builderOp != eb::BuilderBinaryOp::Pow)
			{
				builder::TypeCoercion::assertImplicitlyConvertible(
					leftSolType, commonSolType, m_loc, "binop common-type (arith)");
				builder::TypeCoercion::assertImplicitlyConvertible(
					rightSolType, commonSolType, m_loc, "binop common-type (arith)");
			}
			auto* commonW = m_ctx.typeMapper.map(commonSolType);
			if (commonSolType != leftSolType)
			{
				auto lv = builder::TypeCoercion::coerceToCommonInt(_left, leftSolType, commonW, m_loc);
				commonLeftHold = m_ctx.builderForInstance(commonSolType, lv);
				if (commonLeftHold) arithBuilder = commonLeftHold.get();
			}
			if (commonSolType != rightSolType)
			{
				auto rv = builder::TypeCoercion::coerceToCommonInt(_right, rightSolType, commonW, m_loc);
				commonRightHold = m_ctx.builderForInstance(commonSolType, rv);
				if (commonRightHold) arithRight = commonRightHold.get();
			}
		}
		else if (commonSolType && commonSolType != leftSolType)
		{
			commonLeftHold = m_ctx.builderForInstance(commonSolType, _left);
			if (commonLeftHold) arithBuilder = commonLeftHold.get();
		}
		auto result = arithBuilder->binary_op(*arithRight, builderOp, m_loc);
		if (result) return result->resolve();
	}

	return nullptr;
}

std::shared_ptr<awst::Expression> SolBinaryOperation::toAwst()
{
	// 1. User-defined operator overloading
	if (auto result = tryUserDefinedOp())
		return result;

	// 2. Constant folding
	if (auto result = tryConstantFold())
		return result;

	// 2b. Short-circuit && / || whose RHS has side effects: gate them behind the condition.
	if (auto result = trySolShortCircuit())
		return result;

	// 3. Build operands. Legacy solc evaluates the RIGHT operand first
	// (verified vs 0.8.20 + py-evm: `bump(s) + s.f` reads the PRE-call s.f,
	// `s.f + bump(s)` the post-call one). Capture each side's queued effects
	// and re-emit in that order: right's pre, a pin of right's value, right's
	// write-backs (hoisted), then left's pre — so left's inline reads see
	// right's effects and right's pinned value predates left's. Effect-free
	// operands re-emit byte-identically with no pin.
	auto loweredLeft = m_ctx.lower(m_binOp.leftExpression(), false);
	auto loweredRight = m_ctx.lower(m_binOp.rightExpression(), false);
	auto left = std::move(loweredLeft.value);
	auto right = std::move(loweredRight.value);
	auto ld = std::move(loweredLeft.effects);
	auto rd = std::move(loweredRight.effects);
	if (m_ctx.viaIRSequencing)
	{
		// via-IR evaluates left-to-right == build order: keep everything put.
		m_ctx.restoreOperandDeltas(std::move(ld));
		m_ctx.restoreOperandDeltas(std::move(rd));
	}
	else
	{
		// Static scan: calls that write state DIRECTLY (handle-model storage
		// params) execute inline and queue nothing — pin the right operand so
		// it still evaluates before them. Skipped when the other side only
		// reads locals (nothing inline effects can disturb).
		bool staticNeed =
			(builder::EffectScan::mayWrite(m_binOp.leftExpression())
				&& !builder::onlyLocalPure(m_binOp.rightExpression()))
			|| (builder::EffectScan::mayWrite(m_binOp.rightExpression())
				&& !builder::onlyLocalPure(m_binOp.leftExpression()));
		bool reorder = !ld.empty() || !rd.post.empty() || staticNeed;
		right = m_ctx.emitSequencedOperand(std::move(rd), std::move(right), reorder, m_loc);
		// Left evaluates inline after all re-emitted effects; its own
		// write-backs stay at the statement boundary (nothing later in this
		// expression).
		for (auto& s: ld.pre)
			m_ctx.preEffects().push_back(std::move(s));
		for (auto& s: ld.post)
			m_ctx.postEffects().push_back(std::move(s));
	}
	auto* resultType = m_ctx.typeMapper.map(m_binOp.annotation().type);

	// Checked `**` references its operands in the 0**0 special case AND the
	// pow computation (verified: `x ** f()` ran f twice). The signed path
	// below wraps unconditionally; pin here for the unsigned Exp path too
	// (makeEvalOnce is idempotent and skips trivially-duplicable leaves).
	if (m_binOp.getOperator() == Token::Exp)
	{
		left = awst::makeEvalOnce(std::move(left), m_loc);
		right = awst::makeEvalOnce(std::move(right), m_loc);
	}

	// 4. Signed integer arithmetic (mod 2^N + overflow); must precede sol-eb dispatch.
	auto const* commonType = m_binOp.annotation().commonType;
	if (auto const* intType = dynamic_cast<IntegerType const*>(commonType))
	{
		if (intType->isSigned())
		{
			auto op = m_binOp.getOperator();
			// Signed handlers reference each operand multiple times (sign/overflow/range).
			// makeEvalOnce prevents repeat execution (verified: `a()+b()` ran ~4× each);
			// pure leaves pass through, keeping `x+y`/`x+1` codegen byte-identical.
			left = awst::makeEvalOnce(std::move(left), m_loc);
			right = awst::makeEvalOnce(std::move(right), m_loc);
			bool const isAddSubMul = (op == Token::Add || op == Token::AssignAdd
				|| op == Token::Sub || op == Token::AssignSub
				|| op == Token::Mul || op == Token::AssignMul);
			// MIXED-WIDTH signed add/sub/mul: a NARROWER signed operand arrives as its
			// OWN-width two's complement (int32 -10 = uint64 2^64-10 / int128 -10 =
			// biguint 2^128-10). buildSignedArithmetic then masks to 2^commonBits, which
			// EMBEDS that narrow-width TC as a large positive (int32(-10)*int192(20) gave
			// -20*(2^64-10) not 200). Sign-extend each narrower signed operand to the
			// common width first — mirrors the div/mod path below. Gated on src<common so
			// the same-width common case stays byte-identical (incl. the __smul_l_ puya
			// workaround). Found by the corpus-mutation fuzzer (small_signed_types int64->int192).
			bool leftWidened = false;
			if (isAddSubMul)
			{
				auto* commonW = m_ctx.typeMapper.map(commonType);
				unsigned const bits = intType->numBits();
				auto widen = [&](std::shared_ptr<awst::Expression> v,
						solidity::frontend::Type const* srcT, bool* flag) {
					if (auto s = builder::SolIntType::fromSol(srcT); s && s->isSigned && s->bits < bits)
					{
						if (flag) *flag = true;
						return builder::TypeCoercion::coerceToCommonInt(std::move(v), srcT, commonW, m_loc);
					}
					return v;
				};
				left = widen(std::move(left), m_binOp.leftExpression().annotation().type, &leftWidened);
				right = widen(std::move(right), m_binOp.rightExpression().annotation().type, nullptr);
			}
			// puya mis-lowers a SingleEvaluation(complex expr) used as the LEFT operand of a signed
			// MULTIPLY (stack-slot miscount in the abs/overflow codegen) -> false revert for
			// `(bitwise/shift/cast/ternary) * x`. Materialise a complex left operand to a REAL local
			// (an explicit `T t = expr; t * x` is clean -- fuzzer discriminator); the right operand and
			// add/sub are unaffected. A widened left (mixed-width mul) is likewise a complex
			// expr — materialise it too. Same pre-statement scoping as the existing overflow check.
			if ((op == Token::Mul || op == Token::AssignMul)
				&& (leftWidened || dynamic_cast<awst::SingleEvaluation const*>(left.get())))
			{
				auto smulVar = awst::makeVarExpression(
					"__smul_l_" + std::to_string(m_binOp.id()), left->wtype, m_loc);
				m_ctx.preEffects().push_back(
					awst::makeAssignmentStatement(smulVar, std::move(left), m_loc));
				left = smulVar;
			}
			if (isAddSubMul)
			{
				return buildSignedArithmetic(op, std::move(left), std::move(right), intType);
			}
			if (op == Token::Div || op == Token::AssignDiv
				|| op == Token::Mod || op == Token::AssignMod)
			{
				// Route through the SHARED signed div/mod helper — the same path the
				// compound `x/=b` uses (BigUIntMathHelpers::buildSignedModDiv), which owns
				// the abs-value arithmetic, the intN.min/-1 overflow guard, and the result
				// narrowing. It needs canonical 256-bit two's-complement operands: coerce
				// each to commonType (canonical at the common width — handles a narrower
				// signed divisor like int16 -32768, and literals), then lift to 256-bit.
				// (Was a second, near-identical inline impl, SolBinaryOperation::buildSignedDivMod.)
				unsigned bits = intType->numBits();
				auto* commonW = m_ctx.typeMapper.map(commonType);
				left = builder::TypeCoercion::coerceToCommonInt(
					std::move(left), m_binOp.leftExpression().annotation().type, commonW, m_loc);
				right = builder::TypeCoercion::coerceToCommonInt(
					std::move(right), m_binOp.rightExpression().annotation().type, commonW, m_loc);
				if (bits < 256)
				{
					left = builder::TypeCoercion::signExtendToUint256(std::move(left), bits, m_loc);
					right = builder::TypeCoercion::signExtendToUint256(std::move(right), bits, m_loc);
				}
				auto builderOp = (op == Token::Mod || op == Token::AssignMod)
					? eb::BuilderBinaryOp::Mod : eb::BuilderBinaryOp::FloorDiv;
				return eb::buildSignedModDiv(std::move(left), std::move(right),
					builderOp, bits, !m_scope.isUnchecked(), m_loc);
			}
			if (op == Token::Exp)
			{
				return buildSignedExp(std::move(left), std::move(right), intType);
			}
		}
	}

	// 5. Sol-eb builder dispatch
	if (auto result = trySolEbDispatch(left, right))
		return result;

	// 6. Fallback to buildBinaryOp
	auto built = m_ctx.buildBinaryOp(
		m_binOp.getOperator(), std::move(left), std::move(right), resultType, m_loc);

	// 7. bytesN shift truncation: `bytesN << k` goes through biguint ×2^k
	// (sol-eb/BinaryOpBuilder.cpp), but Solidity's bytesN semantics are
	// left-aligned in a 32-byte word — `bytes6(0x616263646566) << 24` must yield
	// `0x646566000000`, not the 9-byte biguint. Cast to bytes and take last N bytes.
	auto op = m_binOp.getOperator();
	bool isShift = (op == Token::SHL || op == Token::AssignShl
		|| op == Token::SHR || op == Token::AssignShr
		|| op == Token::SAR || op == Token::AssignSar);
	if (isShift && built)
	{
		if (auto const* fbType = dynamic_cast<FixedBytesType const*>(m_binOp.annotation().type))
		{
			unsigned n = fbType->numBytes();
			auto bytesT = awst::WType::bytesType();

			auto asBytes = awst::makeReinterpretCast(std::move(built), bytesT, m_loc);

			// Pad left to ≥N bytes, then extract last N (concat(bzero(N),b) ensures len≥N).
			auto padded = awst::makeLeftPad(asBytes, n, m_loc);
			std::string varName = "__bytes_shift_" + std::to_string(awst::NameGen::next("SolBinaryOperation.shCounter"));
			auto var = awst::makeVarExpression(varName, bytesT, m_loc);
			m_ctx.preEffects().push_back(
				awst::makeAssignmentStatement(var, std::move(padded), m_loc));

			auto lenCall = awst::makeLen(var, m_loc);

			auto nConst = awst::makeIntegerConstant(n, m_loc);
			auto start = awst::makeUInt64BinOp(
				std::move(lenCall), awst::UInt64BinaryOperator::Sub, std::move(nConst), m_loc);

			auto extr = awst::makeExtract3(
				var, std::move(start), awst::makeIntegerConstant(n, m_loc),
				m_loc, bytesT);

			return awst::makeReinterpretCast(std::move(extr), resultType, m_loc); // retype to bytes[N]
		}
	}

	return built;
}

std::shared_ptr<awst::Expression> SolBinaryOperation::buildSignedArithmetic(
	Token _op,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	IntegerType const* _intType)
{
	// Delegate to the shared sol-eb signed-arithmetic helper (also used by SolIntegerBuilder for the
	// compound `x op= d` path, which previously mis-lowered signed assignment via the unsigned ops).
	auto op = (_op == Token::Sub || _op == Token::AssignSub) ? eb::BuilderBinaryOp::Sub
		: (_op == Token::Mul || _op == Token::AssignMul) ? eb::BuilderBinaryOp::Mult
		: eb::BuilderBinaryOp::Add;
	return eb::buildSignedArithmetic(m_ctx, m_scope.isUnchecked(), op,
		std::move(_left), std::move(_right), _intType->numBits(), m_loc);
}

std::shared_ptr<awst::Expression> SolBinaryOperation::buildSignedExp(
	std::shared_ptr<awst::Expression> _base,
	std::shared_ptr<awst::Expression> _exp,
	IntegerType const* _intType)
{
	unsigned bits = _intType->numBits();

	auto [pow2NStr, halfNStr] = builder::TypeCoercion::pow2NAndHalf(bits);

	auto makeBiguintConst = [&](std::string const& val) {
		auto c = awst::makeIntegerConstant(val, m_loc, awst::WType::biguintType());
		return c;
	};

	// Ensure base is biguint
	if (_base->wtype == awst::WType::uint64Type())
	{
		auto itob = awst::makeItob(std::move(_base), m_loc);
		_base = awst::makeAsBiguint(std::move(itob), m_loc);
	}

	// Mask base to N bits
	if (bits < 256)
	{
		auto mask = awst::makeBigUIntBinOp(std::move(_base), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);
		_base = std::move(mask);
	}

	// isNeg: base >= half
	auto baseNegCmp = awst::makeNumericCompare(_base, awst::NumericComparison::Lt, makeBiguintConst(halfNStr), m_loc);
	auto baseNeg = awst::makeNot(std::move(baseNegCmp), m_loc);

	// abs(base) = baseNeg ? (pow2N - base) : base
	auto negBase = awst::makeBigUIntBinOp(makeBiguintConst(pow2NStr), awst::BigUIntBinaryOperator::Sub, _base, m_loc);
	auto absBase = awst::makeConditional(
		baseNeg, std::move(negBase), _base, awst::WType::biguintType(), m_loc);

	// Ensure exp is biguint
	if (_exp->wtype == awst::WType::uint64Type())
	{
		auto itob = awst::makeItob(std::move(_exp), m_loc);
		_exp = awst::makeAsBiguint(std::move(itob), m_loc);
	}

	// Compute abs(base) ^ exp using the standard buildBinaryOp (unsigned exp)
	auto* resultType = awst::WType::biguintType();
	auto absResult = m_ctx.buildBinaryOp(
		Token::Exp, std::move(absBase), _exp, resultType, m_loc);

	// Unchecked sub-word: wrap the (possibly overflowing) magnitude mod 2^bits BEFORE the
	// negation below — `pow2N - absResult` underflows the biguint subtraction when the exp
	// overflows the type (e.g. int8 (-128)**3 = 2097152 > 256) and the AVM `b-` panics. The
	// positive branch wraps too. Checked keeps the raw value: the overflow assert below needs
	// it to detect out-of-range (and a passing assert guarantees absResult < pow2N anyway).
	if (m_scope.isUnchecked() && bits < 256)
		absResult = awst::makeBigUIntBinOp(std::move(absResult),
			awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);

	// Check overflow: absResult must fit in signed range
	// absResult < half for positive result, absResult <= half for negative result
	if (!m_scope.isUnchecked())
	{
		// expIsOdd: exp % 2 != 0
		auto two = makeBiguintConst("2");
		auto expMod2 = awst::makeBigUIntBinOp(_exp, awst::BigUIntBinaryOperator::Mod, std::move(two), m_loc);
		auto expIsOdd = awst::makeNumericCompare(std::move(expMod2), awst::NumericComparison::Ne, makeBiguintConst("0"), m_loc);

		// resultNeg = baseNeg && expIsOdd
		auto resultNeg = awst::makeBoolBinOp(baseNeg, awst::BinaryBooleanOperator::And, std::move(expIsOdd), m_loc);

		// If resultNeg: absResult <= half, else: absResult < half
		auto ltHalf = awst::makeNumericCompare(absResult, awst::NumericComparison::Lt, makeBiguintConst(halfNStr), m_loc);

		auto halfLtRes = awst::makeNumericCompare(makeBiguintConst(halfNStr), awst::NumericComparison::Lt, absResult, m_loc);
		auto leHalf = awst::makeNot(std::move(halfLtRes), m_loc);

		auto rangeOk = awst::makeConditional(
			std::move(resultNeg), std::move(leHalf), std::move(ltHalf),
			awst::WType::boolType(), m_loc);

		auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(rangeOk), m_loc, "signed exp overflow"), m_loc);
		m_ctx.preEffects().push_back(std::move(assertStmt));
	}

	// Negate result when base was negative and exp is odd: (pow2N - absResult) mod pow2N
	auto expMod2_2 = awst::makeBigUIntBinOp(_exp, awst::BigUIntBinaryOperator::Mod, makeBiguintConst("2"), m_loc);
	auto expOdd2 = awst::makeNumericCompare(std::move(expMod2_2), awst::NumericComparison::Ne, makeBiguintConst("0"), m_loc);
	auto shouldNeg = awst::makeBoolBinOp(baseNeg, awst::BinaryBooleanOperator::And, std::move(expOdd2), m_loc);

	auto negResult = awst::makeBigUIntBinOp(makeBiguintConst(pow2NStr), awst::BigUIntBinaryOperator::Sub, absResult, m_loc);
	auto negMod = awst::makeBigUIntBinOp(std::move(negResult), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);

	// absResult == 0 → don't negate
	auto resZero = awst::makeNumericCompare(absResult, awst::NumericComparison::Eq, makeBiguintConst("0"), m_loc);
	auto notZero = awst::makeNot(std::move(resZero), m_loc);
	auto doNeg = awst::makeBoolBinOp(std::move(shouldNeg), awst::BinaryBooleanOperator::And, std::move(notZero), m_loc);

	auto result = awst::makeConditional(
		std::move(doNeg), std::move(negMod), std::move(absResult),
		awst::WType::biguintType(), m_loc);
	// A sub-word signed value's native WType is uint64 (canonical 64-bit two's-complement); narrow
	// the biguint exp result back so it composes as a SUB-expression with surrounding uint64 ops —
	// `b ^ (a**3)` for int8 else hands a biguint to a UInt64BinaryOperation (puya: "expected
	// uint64"). Whole-return coerces, a subexpr does not. >64-bit (int128/int256) stays biguint.
	// implicitNumericCast (NOT makeBiguintToUInt64): the exp result is sign-extended to 256 bits
	// (32 bytes), and a bare btoi reverts on >8 bytes — implicitNumericCast takes the low 8 bytes
	// (the 64-bit two's-complement), as the signed-shift narrow does.
	if (bits <= 64)
		return builder::TypeCoercion::implicitNumericCast(
			std::move(result), awst::WType::uint64Type(), m_loc);
	return result;
}

} // namespace puyasol::builder::sol_ast

/// @file SolBinaryOperation.cpp
/// Migrated from BinaryOperationBuilder.cpp visit() method.

#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-ast/exprs/SolBinaryOperation.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/sol-eb/BuilderOps.h"
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

	std::string subroutineId;
	auto it = m_ctx.freeFunctionById.find(userFunc->id());
	if (it != m_ctx.freeFunctionById.end())
		subroutineId = it->second;
	else
	{
		auto const* libContract = userFunc->annotation().contract;
		if (libContract && libContract->isLibrary())
		{
			std::string qualifiedName = libContract->name() + "." + userFunc->name();
			auto libIt = m_ctx.libraryFunctionIds.find(qualifiedName);
			if (libIt != m_ctx.libraryFunctionIds.end())
				subroutineId = libIt->second;
		}
		if (subroutineId.empty())
			subroutineId = m_ctx.sourceFile + "." + userFunc->name();
	}

	auto left = buildExpr(m_binOp.leftExpression());
	auto right = buildExpr(m_binOp.rightExpression());
	auto* resultType = m_ctx.typeMapper.map(m_binOp.annotation().type);

	auto call = awst::makeSubroutineCall(awst::SubroutineID{subroutineId}, resultType, m_loc);
	awst::pushCallArg(call->args, userFunc->parameters()[0]->name(), std::move(left));
	awst::pushCallArg(call->args, userFunc->parameters()[1]->name(), std::move(right));
	return call;
}

std::shared_ptr<awst::Expression> SolBinaryOperation::tryConstantFold()
{
	if (auto const* ratType = dynamic_cast<RationalNumberType const*>(
			m_binOp.annotation().type))
	{
		if (!ratType->isFractional())
		{
			auto* resultType = m_ctx.typeMapper.map(m_binOp.annotation().type);
			auto val = ratType->literalValue(nullptr);
			// literalValue() returns u256 (two's complement for negatives).
			// If value exceeds uint64, promote to biguint to preserve full
			// 256-bit representation (needed for sign extension in biguint contexts).
			static const solidity::u256 uint64Max("18446744073709551615");
			if (resultType == awst::WType::uint64Type() && val > uint64Max)
				resultType = awst::WType::biguintType();
			auto e = awst::makeIntegerConstant(val.str(), m_loc, resultType);
			return e;
		}
	}
	return nullptr;
}

std::shared_ptr<awst::Expression> SolBinaryOperation::trySolEbDispatch(
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right)
{
	auto solOp = m_binOp.getOperator();
	auto* leftSolType = m_binOp.leftExpression().annotation().type;
	auto* rightSolType = m_binOp.rightExpression().annotation().type;

	// Use the common type for arithmetic so overflow checks use the correct
	// bit width (e.g., uint8 + uint16 should check uint16 overflow, not uint8).
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
		auto result = leftBuilder->compare(*rightBuilder, cmpOp, m_loc);
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
		// For arithmetic ops, use a builder based on the common type so overflow
		// checks use the correct bit width (e.g., uint8 + uint16 → uint16 overflow).
		auto* arithBuilder = leftBuilder.get();
		std::unique_ptr<eb::InstanceBuilder> commonBuilder;
		if (commonSolType && commonSolType != leftSolType)
		{
			commonBuilder = m_ctx.builderForInstance(commonSolType, _left);
			if (commonBuilder)
				arithBuilder = commonBuilder.get();
		}
		auto result = arithBuilder->binary_op(*rightBuilder, builderOp, m_loc);
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

	// 3. Build operands
	auto left = buildExpr(m_binOp.leftExpression());
	auto right = buildExpr(m_binOp.rightExpression());
	auto* resultType = m_ctx.typeMapper.map(m_binOp.annotation().type);

	// 4. Signed integer arithmetic — wrap mod 2^N + overflow detection
	// Must come before sol-eb dispatch which doesn't handle signed wrapping.
	auto const* commonType = m_binOp.annotation().commonType;
	if (auto const* intType = dynamic_cast<IntegerType const*>(commonType))
	{
		if (intType->isSigned())
		{
			auto op = m_binOp.getOperator();
			// The signed handlers reference each operand multiple times (sign/
			// overflow/range checks alongside the result). A side-effecting
			// operand — e.g. `a() + b()` — would otherwise execute once per
			// reference (verified: `a() + b()` ran a()/b() ~4× each). Wrap each
			// non-trivial operand in a SingleEvaluation so the backend evaluates
			// it once and reuses the cached value across every reference. Pure
			// leaves (vars/constants) are idempotent, so skip them to keep the
			// common `x + y` / `x + 1` codegen byte-identical.
			auto isLeafOperand = [](awst::Expression const* e) {
				return dynamic_cast<awst::VarExpression const*>(e)
					|| dynamic_cast<awst::IntegerConstant const*>(e)
					|| dynamic_cast<awst::BoolConstant const*>(e)
					|| dynamic_cast<awst::BytesConstant const*>(e)
					|| dynamic_cast<awst::StringConstant const*>(e)
					|| dynamic_cast<awst::AddressConstant const*>(e);
			};
			auto wrapEval = [&](std::shared_ptr<awst::Expression> v)
				-> std::shared_ptr<awst::Expression> {
				if (isLeafOperand(v.get())) return v;
				static int s_signedOpEvalId = 0;
				auto const* wt = v->wtype;
				return awst::makeSingleEvaluation(std::move(v), wt, ++s_signedOpEvalId, m_loc);
			};
			left = wrapEval(std::move(left));
			right = wrapEval(std::move(right));
			if (op == Token::Add || op == Token::AssignAdd
				|| op == Token::Sub || op == Token::AssignSub
				|| op == Token::Mul || op == Token::AssignMul)
			{
				return buildSignedArithmetic(op, std::move(left), std::move(right), intType);
			}
			if (op == Token::Div || op == Token::AssignDiv
				|| op == Token::Mod || op == Token::AssignMod)
			{
				return buildSignedDivMod(op, std::move(left), std::move(right), intType);
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

	// 7. bytesN shift truncation. `bytesN << k` and `bytesN >> k` lower
	// through buildBinaryOp's biguint multiply-by-2^k / divide-by-2^k path
	// (see sol-eb/BinaryOpBuilder.cpp). The result is biguint with no
	// width bound, but Solidity's bytesN shift semantics treat the value
	// as left-aligned in a 32-byte word: `bytes6 = 0x616263646566` × 2^24
	// must produce `0x646566000000` (low 6 bytes after shift), not the
	// 9-byte biguint `0x616263646566000000`. Cast back to bytes and take
	// the low N bytes (right-aligned) when the declared result type is
	// FixedBytesType.
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

			// biguint → bytes (raw BE encoding, variable length)
			auto asBytes = awst::makeReinterpretCast(std::move(built), bytesT, m_loc);

			// Pad on the left to ensure we always have at least N bytes,
			// then take the LAST N bytes via substring3(b, len(b)-N, len(b)).
			// concat(bzero(N), b) gives len ≥ N regardless of biguint width.
			auto padded = awst::makeLeftPad(asBytes, n, m_loc);
			// Pin the padded result to a local so we can read len() once.
			static int shCounter = 0;
			std::string varName = "__bytes_shift_" + std::to_string(shCounter++);
			auto var = awst::makeVarExpression(varName, bytesT, m_loc);
			m_ctx.prePendingStatements.push_back(
				awst::makeAssignmentStatement(var, std::move(padded), m_loc));

			auto lenCall = awst::makeLen(var, m_loc);

			auto nConst = awst::makeIntegerConstant(n, m_loc);
			auto start = awst::makeUInt64BinOp(
				std::move(lenCall), awst::UInt64BinaryOperator::Sub, std::move(nConst), m_loc);

			auto extr = awst::makeExtract3(
				var, std::move(start), awst::makeIntegerConstant(n, m_loc),
				m_loc, bytesT);

			// Re-type to bytes[N].
			return awst::makeReinterpretCast(std::move(extr), resultType, m_loc);
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
	unsigned bits = _intType->numBits();
	bool isBiguint = (bits > 64);

	// Compute 2^N and 2^(N-1) as string constants
	// Note: u256 can't hold 2^256 (it overflows to 0), so special-case it.
	std::string pow2NStr, halfNStr;
	if (bits == 256)
	{
		pow2NStr = kPow2_256;
		halfNStr = "57896044618658097711785492504343953926634992332820282019728792003956564819968";
	}
	else
	{
		solidity::u256 pow2N = solidity::u256(1) << bits;
		solidity::u256 halfN = solidity::u256(1) << (bits - 1);
		std::ostringstream pow2NOss, halfNOss;
		pow2NOss << pow2N;
		halfNOss << halfN;
		pow2NStr = pow2NOss.str();
		halfNStr = halfNOss.str();
	}

	auto makeConst = [&](std::string const& val) -> std::shared_ptr<awst::Expression> {
		auto c = awst::makeIntegerConstant(val, m_loc, isBiguint ? awst::WType::biguintType() : awst::WType::uint64Type());
		return c;
	};

	auto makeBiguintConst = [&](std::string const& val) -> std::shared_ptr<awst::Expression> {
		auto c = awst::makeIntegerConstant(val, m_loc, awst::WType::biguintType());
		return c;
	};

	// (operand coercion to biguint happens in Step 1 below)

	// Ensure both operands are biguint (needed for mod 2^N wrapping)
	auto ensureBiguint = [&](std::shared_ptr<awst::Expression> expr)
		-> std::shared_ptr<awst::Expression> {
		if (expr->wtype == awst::WType::biguintType())
			return expr;
		// `itob` is uint64-only. account / fixed-bytes / dynamic bytes are
		// already big-endian byte buffers — reinterpret-cast directly through
		// `bytes` to `biguint` (no truncation). Same fix as in
		// AssemblyBuilder::ensureBiguint (commit `01332f363`); this lambda is
		// SolBinaryOperation's parallel coercion path for signed-arith ops.
		if (expr->wtype == awst::WType::accountType()
			|| (expr->wtype && expr->wtype->kind() == awst::WTypeKind::Bytes))
		{
			auto bytesExpr = expr->wtype == awst::WType::accountType()
				? awst::makeAsBytes(std::move(expr), m_loc)
				: std::move(expr);
			return awst::makeAsBiguint(std::move(bytesExpr), m_loc);
		}
		auto itob = awst::makeItob(std::move(expr), m_loc);
		return awst::makeAsBiguint(std::move(itob), m_loc);
	};

	_left = ensureBiguint(std::move(_left));
	_right = ensureBiguint(std::move(_right));

	// Mask operands to N bits — uint64 two's complement values may be larger
	// than 2^N (e.g., int8(-2) is uint64(2^64-2)). Modular arithmetic requires
	// operands in [0, 2^N) range.
	if (bits < 256)
	{
		auto maskOp = [&](std::shared_ptr<awst::Expression> val)
			-> std::shared_ptr<awst::Expression> {
			auto mod = awst::makeBigUIntBinOp(std::move(val), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);
			return mod;
		};
		_left = maskOp(std::move(_left));
		_right = maskOp(std::move(_right));
	}

	// Step 1: Perform unsigned operation (all in biguint)
	// For subtraction: compute (a + 2^N - b) instead of (a - b) to avoid negative biguint.
	std::shared_ptr<awst::Expression> rawResult;
	bool isSub = (_op == Token::Sub || _op == Token::AssignSub);

	if (isSub)
	{
		// (a + 2^N - b) — always non-negative since a < 2^N and b < 2^N
		auto aPlusPow = awst::makeBigUIntBinOp(_left, awst::BigUIntBinaryOperator::Add, makeBiguintConst(pow2NStr), m_loc);

		auto subB = awst::makeBigUIntBinOp(std::move(aPlusPow), awst::BigUIntBinaryOperator::Sub, _right, m_loc);
		rawResult = std::move(subB);
	}
	else
	{
		auto bigOp = (_op == Token::Mul || _op == Token::AssignMul)
			? awst::BigUIntBinaryOperator::Mult
			: awst::BigUIntBinaryOperator::Add;
		auto binOp = awst::makeBigUIntBinOp(_left, bigOp, _right, m_loc);
		rawResult = std::move(binOp);
	}

	// Step 2: Wrap mod 2^N (always needed for signed — two's complement semantics)
	{
		auto wrapOp = awst::makeBigUIntBinOp(std::move(rawResult), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);
		rawResult = std::move(wrapOp);
	}

	// Step 3: Signed overflow check (skip in unchecked blocks)
	if (!m_scope.isUnchecked())
	{
		// Signed overflow for add: both same sign, result different sign
		// Signed overflow for sub: different signs, result sign != a's sign
		// Signed overflow for mul: result doesn't round-trip (complex, use range check)
		//
		// General approach: check result is in [0, 2^(N-1)) or [2^N - 2^(N-1), 2^N)
		// i.e., the two's complement value is in [-2^(N-1), 2^(N-1)-1]
		// For add/sub, use the sign-based check. For mul, use range check.
		//
		// For add: overflow iff a_neg == b_neg && a_neg != result_neg
		// For sub: overflow iff a_neg != b_neg && a_neg != result_neg

		// Coerce a value to biguint if needed (same itob-vs-reinterpret rule
		// as the outer ensureBiguint lambda).
		auto toBiguint = [&](std::shared_ptr<awst::Expression> const& val)
			-> std::shared_ptr<awst::Expression> {
			if (val->wtype == awst::WType::biguintType())
				return val;
			if (val->wtype == awst::WType::accountType()
				|| (val->wtype && val->wtype->kind() == awst::WTypeKind::Bytes))
			{
				auto bytesExpr = val->wtype == awst::WType::accountType()
					? awst::makeAsBytes(val, m_loc)
					: val;
				return awst::makeAsBiguint(std::move(bytesExpr), m_loc);
			}
			auto itob = awst::makeItob(val, m_loc);
			return awst::makeAsBiguint(std::move(itob), m_loc);
		};

		// isNeg: val >= half  ↔  NOT (val < half)
		auto isNeg = [&](std::shared_ptr<awst::Expression> const& val)
			-> std::shared_ptr<awst::Expression> {
			auto cmp = awst::makeNumericCompare(toBiguint(val), awst::NumericComparison::Lt, makeBiguintConst(halfNStr), m_loc);
			auto notExpr = awst::makeNot(std::move(cmp), m_loc);
			return notExpr;
		};

		std::shared_ptr<awst::Expression> overflowCond;

		if (_op == Token::Add || _op == Token::AssignAdd)
		{
			// Overflow iff: a_neg == b_neg && a_neg != result_neg
			// assert: a_neg != b_neg || a_neg == result_neg
			auto aNeg = isNeg(_left);
			auto bNeg = isNeg(_right);
			auto rNeg = isNeg(rawResult);

			// a_neg != b_neg (different signs → no overflow possible)
			auto diffSigns = awst::makeNumericCompare(aNeg, awst::NumericComparison::Ne, bNeg, m_loc);

			// a_neg == result_neg (same sign as input → no overflow)
			auto sameSignResult = awst::makeNumericCompare(aNeg, awst::NumericComparison::Eq, rNeg, m_loc);

			// OR: either different input signs or result has same sign as a
			auto noOverflow = awst::makeBoolBinOp(std::move(diffSigns), awst::BinaryBooleanOperator::Or, std::move(sameSignResult), m_loc);
			overflowCond = std::move(noOverflow);
		}
		else if (_op == Token::Sub || _op == Token::AssignSub)
		{
			// Overflow iff: a_neg != b_neg && a_neg != result_neg
			// assert: a_neg == b_neg || a_neg == result_neg
			auto aNeg = isNeg(_left);
			auto bNeg = isNeg(_right);
			auto rNeg = isNeg(rawResult);

			auto sameSigns = awst::makeNumericCompare(aNeg, awst::NumericComparison::Eq, bNeg, m_loc);

			auto sameSignResult = awst::makeNumericCompare(aNeg, awst::NumericComparison::Eq, rNeg, m_loc);

			auto noOverflow = awst::makeBoolBinOp(std::move(sameSigns), awst::BinaryBooleanOperator::Or, std::move(sameSignResult), m_loc);
			overflowCond = std::move(noOverflow);
		}
		else // mul
		{
			// Signed multiplication overflow detection:
			// The raw (unwrapped) product is exact in biguint. Compute abs values
			// of operands, multiply, and check the result fits in signed range.
			// abs(a) = a < half ? a : pow2N - a
			auto absVal = [&](std::shared_ptr<awst::Expression> const& val)
				-> std::shared_ptr<awst::Expression> {
				auto neg = isNeg(val); // val >= half
				// pow2N - val
				auto negated = awst::makeBigUIntBinOp(makeBiguintConst(pow2NStr), awst::BigUIntBinaryOperator::Sub, val, m_loc);
				// neg ? (pow2N - val) : val
				return awst::makeConditional(
					std::move(neg), std::move(negated), val,
					awst::WType::biguintType(), m_loc);
			};

			auto absA = absVal(_left);
			auto absB = absVal(_right);

			// absProduct = absA * absB (exact, no overflow in biguint)
			auto absProduct = awst::makeBigUIntBinOp(std::move(absA), awst::BigUIntBinaryOperator::Mult, std::move(absB), m_loc);

			// Check: absProduct <= half (INT_MAX + 1 for same-sign, INT_MAX for diff-sign)
			// Conservative: absProduct < pow2N/2 handles most cases.
			// Special: -1 * MIN_INT and MIN_INT * -1 overflow (absProduct == half).
			// Same sign → result positive → absProduct must be < half
			// Different sign → result negative → absProduct must be <= half
			auto aNeg2 = isNeg(_left);
			auto bNeg2 = isNeg(_right);
			auto sameSign = awst::makeNumericCompare(aNeg2, awst::NumericComparison::Eq, bNeg2, m_loc);

			// If same sign: absProduct < half (result must be positive, < INT_MAX+1)
			auto ltHalf = awst::makeNumericCompare(absProduct, awst::NumericComparison::Lt, makeBiguintConst(halfNStr), m_loc);

			// If different sign: absProduct <= half (result must be negative, >= -half)
			// absProduct <= half  ↔  NOT (absProduct > half)  ↔  NOT (half < absProduct)
			auto halfLtProd = awst::makeNumericCompare(makeBiguintConst(halfNStr), awst::NumericComparison::Lt, absProduct, m_loc);
			auto leHalf = awst::makeNot(std::move(halfLtProd), m_loc);

			// sameSign ? (absProduct < half) : (absProduct <= half)
			auto rangeCheck = awst::makeConditional(
				std::move(sameSign), std::move(ltHalf), std::move(leHalf),
				awst::WType::boolType(), m_loc);

			// Also handle b == 0 (no overflow, result is 0)
			auto bZero = awst::makeNumericCompare(_right, awst::NumericComparison::Eq, makeBiguintConst("0"), m_loc);

			auto noOverflow = awst::makeBoolBinOp(std::move(bZero), awst::BinaryBooleanOperator::Or, std::move(rangeCheck), m_loc);

			overflowCond = std::move(noOverflow);
		}

		if (overflowCond)
		{
			auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(overflowCond), m_loc, "signed arithmetic overflow"), m_loc);
			m_ctx.prePendingStatements.push_back(std::move(assertStmt));
		}
	}

	// Step 4: Truncate back to uint64 for ≤64-bit types
	if (!isBiguint)
	{
		// rawResult is biguint from the mod — convert back to uint64
		// biguint → uint64: take the low 8 bytes.
		return awst::makeBiguintToUInt64(std::move(rawResult), m_loc);
	}

	// Step 5: Canonicalise a sub-256 signed result to 256-bit two's complement.
	// The `mod 2^N` wrap above leaves a NEGATIVE int<N> (64 < N < 256) in N-bit
	// two's-complement form (2^N - X, sign bit at N-1, bit 255 clear). But the
	// rest of the pipeline treats the canonical signed form as 256-bit: signed
	// ordering compares XOR with 2^255 (SolIntegerBuilder), so a value whose sign
	// bit sits at N-1 reads as POSITIVE. Without this, an INLINE consumer of a
	// computed negative — the V4 `getAmount0/1Delta(int128)` doing `liquidity < 0`
	// on a subtracted/derived delta — takes the wrong branch and `uint128(liquidity)`
	// becomes ~2^128 (the observed garbage remove-amount). Idempotent with the
	// signExtendToUint256 already applied at assignment/return/cast sites; no-op
	// for non-negative results and for int256 (bits == 256).
	if (bits < 256)
		return TypeCoercion::signExtendToUint256(std::move(rawResult), bits, m_loc);

	return rawResult;
}

std::shared_ptr<awst::Expression> SolBinaryOperation::buildSignedExp(
	std::shared_ptr<awst::Expression> _base,
	std::shared_ptr<awst::Expression> _exp,
	IntegerType const* _intType)
{
	unsigned bits = _intType->numBits();

	std::string pow2NStr, halfNStr;
	if (bits == 256)
	{
		pow2NStr = kPow2_256;
		halfNStr = "57896044618658097711785492504343953926634992332820282019728792003956564819968";
	}
	else
	{
		solidity::u256 pow2N = solidity::u256(1) << bits;
		solidity::u256 halfN = solidity::u256(1) << (bits - 1);
		std::ostringstream oss1, oss2;
		oss1 << pow2N; pow2NStr = oss1.str();
		oss2 << halfN; halfNStr = oss2.str();
	}

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
		m_ctx.prePendingStatements.push_back(std::move(assertStmt));
	}

	// If base was negative and exp is odd, negate result: (pow2N - absResult) mod pow2N
	auto expMod2_2 = awst::makeBigUIntBinOp(_exp, awst::BigUIntBinaryOperator::Mod, makeBiguintConst("2"), m_loc);
	auto expOdd2 = awst::makeNumericCompare(std::move(expMod2_2), awst::NumericComparison::Ne, makeBiguintConst("0"), m_loc);
	auto shouldNeg = awst::makeBoolBinOp(baseNeg, awst::BinaryBooleanOperator::And, std::move(expOdd2), m_loc);

	auto negResult = awst::makeBigUIntBinOp(makeBiguintConst(pow2NStr), awst::BigUIntBinaryOperator::Sub, absResult, m_loc);
	auto negMod = awst::makeBigUIntBinOp(std::move(negResult), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);

	// absResult == 0 → don't negate
	auto resZero = awst::makeNumericCompare(absResult, awst::NumericComparison::Eq, makeBiguintConst("0"), m_loc);
	auto notZero = awst::makeNot(std::move(resZero), m_loc);
	auto doNeg = awst::makeBoolBinOp(std::move(shouldNeg), awst::BinaryBooleanOperator::And, std::move(notZero), m_loc);

	return awst::makeConditional(
		std::move(doNeg), std::move(negMod), std::move(absResult),
		awst::WType::biguintType(), m_loc);
}

std::shared_ptr<awst::Expression> SolBinaryOperation::buildSignedDivMod(
	Token _op,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	IntegerType const* _intType)
{
	unsigned bits = _intType->numBits();
	bool isDiv = (_op == Token::Div || _op == Token::AssignDiv);

	std::string pow2NStr, halfNStr;
	if (bits == 256)
	{
		pow2NStr = kPow2_256;
		halfNStr = "57896044618658097711785492504343953926634992332820282019728792003956564819968";
	}
	else
	{
		solidity::u256 pow2N = solidity::u256(1) << bits;
		solidity::u256 halfN = solidity::u256(1) << (bits - 1);
		std::ostringstream oss1, oss2;
		oss1 << pow2N; pow2NStr = oss1.str();
		oss2 << halfN; halfNStr = oss2.str();
	}

	auto makeBiguintConst = [&](std::string const& val) {
		auto c = awst::makeIntegerConstant(val, m_loc, awst::WType::biguintType());
		return c;
	};

	// Ensure both operands are biguint (mirrors buildSignedArithmetic's
	// ensureBiguint — same account/bytes-vs-itob branching).
	auto ensureBiguint = [&](std::shared_ptr<awst::Expression> expr)
		-> std::shared_ptr<awst::Expression> {
		if (expr->wtype == awst::WType::biguintType())
			return expr;
		if (expr->wtype == awst::WType::accountType()
			|| (expr->wtype && expr->wtype->kind() == awst::WTypeKind::Bytes))
		{
			auto bytesExpr = expr->wtype == awst::WType::accountType()
				? awst::makeAsBytes(std::move(expr), m_loc)
				: std::move(expr);
			return awst::makeAsBiguint(std::move(bytesExpr), m_loc);
		}
		auto itob = awst::makeItob(std::move(expr), m_loc);
		return awst::makeAsBiguint(std::move(itob), m_loc);
	};

	_left = ensureBiguint(std::move(_left));
	_right = ensureBiguint(std::move(_right));

	// Mask to N bits
	if (bits < 256)
	{
		auto maskOp = [&](std::shared_ptr<awst::Expression> val) {
			auto mod = awst::makeBigUIntBinOp(std::move(val), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);
			return mod;
		};
		_left = maskOp(std::move(_left));
		_right = maskOp(std::move(_right));
	}

	// isNeg: val >= half
	auto isNeg = [&](std::shared_ptr<awst::Expression> const& val)
		-> std::shared_ptr<awst::Expression> {
		auto cmp = awst::makeNumericCompare(val, awst::NumericComparison::Lt, makeBiguintConst(halfNStr), m_loc);
		auto notExpr = awst::makeNot(std::move(cmp), m_loc);
		return notExpr;
	};

	// abs(val) = val < half ? val : (pow2N - val)
	auto absVal = [&](std::shared_ptr<awst::Expression> const& val) {
		auto neg = isNeg(val);
		auto negated = awst::makeBigUIntBinOp(makeBiguintConst(pow2NStr), awst::BigUIntBinaryOperator::Sub, val, m_loc);
		return awst::makeConditional(
			std::move(neg), std::move(negated), val,
			awst::WType::biguintType(), m_loc);
	};

	// Checked: assert(y != 0)
	{
		auto bZero = awst::makeNumericCompare(_right, awst::NumericComparison::Ne, makeBiguintConst("0"), m_loc);

		auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(bZero), m_loc, "division by zero"), m_loc);
		m_ctx.prePendingStatements.push_back(std::move(assertStmt));
	}

	// Checked div: assert NOT (x == minVal && y == -1)
	// minVal = half (= 2^(N-1)), -1 = pow2N - 1
	if (isDiv && !m_scope.isUnchecked())
	{
		std::ostringstream minusOneOss;
		if (bits == 256)
			minusOneOss << "115792089237316195423570985008687907853269984665640564039457584007913129639935";
		else
		{
			solidity::u256 minusOne = (solidity::u256(1) << bits) - 1;
			minusOneOss << minusOne;
		}

		auto xIsMin = awst::makeNumericCompare(_left, awst::NumericComparison::Eq, makeBiguintConst(halfNStr), m_loc);

		auto yIsNeg1 = awst::makeNumericCompare(_right, awst::NumericComparison::Eq, makeBiguintConst(minusOneOss.str()), m_loc);

		auto bothTrue = awst::makeBoolBinOp(std::move(xIsMin), awst::BinaryBooleanOperator::And, std::move(yIsNeg1), m_loc);

		auto notBoth = awst::makeNot(std::move(bothTrue), m_loc);

		auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(notBoth), m_loc, "signed division overflow"), m_loc);
		m_ctx.prePendingStatements.push_back(std::move(assertStmt));
	}

	auto absA = absVal(_left);
	auto absB = absVal(_right);

	// Compute unsigned div/mod on absolute values
	auto unsignedResult = awst::makeBigUIntBinOp(std::move(absA), isDiv ? awst::BigUIntBinaryOperator::FloorDiv
	                           : awst::BigUIntBinaryOperator::Mod, std::move(absB), m_loc);

	// Determine result sign:
	// Division: result negative if signs differ
	// Modulo: result has sign of dividend (a)
	std::shared_ptr<awst::Expression> shouldNegate;
	if (isDiv)
	{
		// signs differ: a_neg XOR b_neg  ↔  a_neg != b_neg
		auto aNeg = isNeg(_left);
		auto bNeg = isNeg(_right);
		auto differ = awst::makeNumericCompare(std::move(aNeg), awst::NumericComparison::Ne, std::move(bNeg), m_loc);
		shouldNegate = std::move(differ);
	}
	else
	{
		// mod: negate if a is negative
		shouldNegate = isNeg(_left);
	}

	// Negate: (pow2N - result) mod pow2N
	auto negResult = awst::makeBigUIntBinOp(makeBiguintConst(pow2NStr), awst::BigUIntBinaryOperator::Sub, unsignedResult, m_loc);

	auto negMod = awst::makeBigUIntBinOp(std::move(negResult), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);

	// Also need result == 0 check: don't negate 0
	auto resultIsZero = awst::makeNumericCompare(unsignedResult, awst::NumericComparison::Eq, makeBiguintConst("0"), m_loc);

	auto notExpr = awst::makeNot(std::move(resultIsZero), m_loc);
	auto needNeg = awst::makeBoolBinOp(std::move(shouldNegate), awst::BinaryBooleanOperator::And, std::move(notExpr), m_loc);

	// shouldNegate && result != 0 ? negated : unsigned
	auto finalResult = awst::makeConditional(
		std::move(needNeg), std::move(negMod), std::move(unsignedResult),
		awst::WType::biguintType(), m_loc);

	// Convert back to uint64 for ≤64-bit types
	if (bits <= 64)
	{
		return awst::makeBiguintToUInt64(std::move(finalResult), m_loc);
	}

	return finalResult;
}

} // namespace puyasol::builder::sol_ast

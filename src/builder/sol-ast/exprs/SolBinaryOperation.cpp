/// @file SolBinaryOperation.cpp
/// Migrated from BinaryOperationBuilder.cpp visit() method.

#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-ast/exprs/SolBinaryOperation.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/sol-eb/BuilderOps.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolutil/Numeric.h>

#include <sstream>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;
using Token = solidity::frontend::Token;

SolBinaryOperation::SolBinaryOperation(
	eb::BuilderContext& _ctx,
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
		auto const* scope = userFunc->scope();
		auto const* libContract = dynamic_cast<ContractDefinition const*>(scope);
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

	auto call = std::make_shared<awst::SubroutineCallExpression>();
	call->sourceLocation = m_loc;
	call->wtype = resultType;
	call->target = awst::SubroutineID{subroutineId};

	awst::CallArg argA;
	argA.name = userFunc->parameters()[0]->name();
	argA.value = std::move(left);
	call->args.push_back(std::move(argA));

	awst::CallArg argB;
	argB.name = userFunc->parameters()[1]->name();
	argB.value = std::move(right);
	call->args.push_back(std::move(argB));

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
			auto padN = awst::makeIntrinsicCall("bzero", bytesT, m_loc);
			padN->stackArgs.push_back(awst::makeIntegerConstant(std::to_string(n), m_loc));

			auto padded = awst::makeIntrinsicCall("concat", bytesT, m_loc);
			padded->stackArgs.push_back(std::move(padN));
			padded->stackArgs.push_back(asBytes);
			// Pin the padded result to a local so we can read len() once.
			static int shCounter = 0;
			std::string varName = "__bytes_shift_" + std::to_string(shCounter++);
			auto var = awst::makeVarExpression(varName, bytesT, m_loc);
			m_ctx.prePendingStatements.push_back(
				awst::makeAssignmentStatement(var, std::move(padded), m_loc));

			auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), m_loc);
			lenCall->stackArgs.push_back(var);

			auto nConst = awst::makeIntegerConstant(std::to_string(n), m_loc);
			auto start = awst::makeUInt64BinOp(
				std::move(lenCall), awst::UInt64BinaryOperator::Sub, std::move(nConst), m_loc);

			auto extr = awst::makeIntrinsicCall("extract3", bytesT, m_loc);
			extr->stackArgs.push_back(var);
			extr->stackArgs.push_back(std::move(start));
			extr->stackArgs.push_back(awst::makeIntegerConstant(std::to_string(n), m_loc));

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
		auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), m_loc);
		itob->stackArgs.push_back(std::move(expr));
		auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), m_loc);
		return cast;
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
		auto binOp = std::make_shared<awst::BigUIntBinaryOperation>();
		binOp->sourceLocation = m_loc;
		binOp->wtype = awst::WType::biguintType();
		binOp->left = _left;
		binOp->right = _right;
		if (_op == Token::Mul || _op == Token::AssignMul)
			binOp->op = awst::BigUIntBinaryOperator::Mult;
		else
			binOp->op = awst::BigUIntBinaryOperator::Add;
		rawResult = std::move(binOp);
	}

	// Step 2: Wrap mod 2^N (always needed for signed — two's complement semantics)
	{
		auto wrapOp = awst::makeBigUIntBinOp(std::move(rawResult), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);
		rawResult = std::move(wrapOp);
	}

	// Step 3: Signed overflow check (skip in unchecked blocks)
	if (!m_ctx.inUncheckedBlock)
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

		// Coerce a value to biguint if needed
		auto toBiguint = [&](std::shared_ptr<awst::Expression> const& val)
			-> std::shared_ptr<awst::Expression> {
			if (val->wtype == awst::WType::biguintType())
				return val;
			auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), m_loc);
			itob->stackArgs.push_back(val);
			auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), m_loc);
			return cast;
		};

		// isNeg: val >= half  ↔  NOT (val < half)
		auto isNeg = [&](std::shared_ptr<awst::Expression> const& val)
			-> std::shared_ptr<awst::Expression> {
			auto cmp = awst::makeNumericCompare(toBiguint(val), awst::NumericComparison::Lt, makeBiguintConst(halfNStr), m_loc);
			auto notExpr = std::make_shared<awst::Not>();
			notExpr->sourceLocation = m_loc;
			notExpr->wtype = awst::WType::boolType();
			notExpr->expr = std::move(cmp);
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
			auto noOverflow = std::make_shared<awst::BooleanBinaryOperation>();
			noOverflow->sourceLocation = m_loc;
			noOverflow->wtype = awst::WType::boolType();
			noOverflow->left = std::move(diffSigns);
			noOverflow->op = awst::BinaryBooleanOperator::Or;
			noOverflow->right = std::move(sameSignResult);
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

			auto noOverflow = std::make_shared<awst::BooleanBinaryOperation>();
			noOverflow->sourceLocation = m_loc;
			noOverflow->wtype = awst::WType::boolType();
			noOverflow->left = std::move(sameSigns);
			noOverflow->op = awst::BinaryBooleanOperator::Or;
			noOverflow->right = std::move(sameSignResult);
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
				auto cond = std::make_shared<awst::ConditionalExpression>();
				cond->sourceLocation = m_loc;
				cond->wtype = awst::WType::biguintType();
				cond->condition = std::move(neg);
				cond->trueExpr = std::move(negated);
				cond->falseExpr = val;
				return cond;
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
			auto leHalf = std::make_shared<awst::Not>();
			leHalf->sourceLocation = m_loc;
			leHalf->wtype = awst::WType::boolType();
			leHalf->expr = std::move(halfLtProd);

			// sameSign ? (absProduct < half) : (absProduct <= half)
			auto rangeCheck = std::make_shared<awst::ConditionalExpression>();
			rangeCheck->sourceLocation = m_loc;
			rangeCheck->wtype = awst::WType::boolType();
			rangeCheck->condition = std::move(sameSign);
			rangeCheck->trueExpr = std::move(ltHalf);
			rangeCheck->falseExpr = std::move(leHalf);

			// Also handle b == 0 (no overflow, result is 0)
			auto bZero = awst::makeNumericCompare(_right, awst::NumericComparison::Eq, makeBiguintConst("0"), m_loc);

			auto noOverflow = std::make_shared<awst::BooleanBinaryOperation>();
			noOverflow->sourceLocation = m_loc;
			noOverflow->wtype = awst::WType::boolType();
			noOverflow->left = std::move(bZero);
			noOverflow->op = awst::BinaryBooleanOperator::Or;
			noOverflow->right = std::move(rangeCheck);

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
		auto cast = awst::makeReinterpretCast(std::move(rawResult), awst::WType::bytesType(), m_loc);

		// concat(bzero(8), bytes) then extract last 8 bytes → btoi
		auto eight = awst::makeIntegerConstant("8", m_loc);

		auto bz = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), m_loc);
		bz->stackArgs.push_back(eight);

		auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), m_loc);
		cat->stackArgs.push_back(std::move(bz));
		cat->stackArgs.push_back(std::move(cast));

		auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), m_loc);
		lenCall->stackArgs.push_back(cat);

		auto eight2 = awst::makeIntegerConstant("8", m_loc);

		auto start = awst::makeIntrinsicCall("-", awst::WType::uint64Type(), m_loc);
		start->stackArgs.push_back(std::move(lenCall));
		start->stackArgs.push_back(eight2);

		auto extract = awst::makeIntrinsicCall("extract_uint64", awst::WType::uint64Type(), m_loc);
		extract->stackArgs.push_back(cat);
		extract->stackArgs.push_back(std::move(start));

		return extract;
	}

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
		auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), m_loc);
		itob->stackArgs.push_back(std::move(_base));
		auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), m_loc);
		_base = std::move(cast);
	}

	// Mask base to N bits
	if (bits < 256)
	{
		auto mask = awst::makeBigUIntBinOp(std::move(_base), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);
		_base = std::move(mask);
	}

	// isNeg: base >= half
	auto baseNegCmp = awst::makeNumericCompare(_base, awst::NumericComparison::Lt, makeBiguintConst(halfNStr), m_loc);
	auto baseNeg = std::make_shared<awst::Not>();
	baseNeg->sourceLocation = m_loc;
	baseNeg->wtype = awst::WType::boolType();
	baseNeg->expr = std::move(baseNegCmp);

	// abs(base) = baseNeg ? (pow2N - base) : base
	auto negBase = awst::makeBigUIntBinOp(makeBiguintConst(pow2NStr), awst::BigUIntBinaryOperator::Sub, _base, m_loc);
	auto absBase = std::make_shared<awst::ConditionalExpression>();
	absBase->sourceLocation = m_loc;
	absBase->wtype = awst::WType::biguintType();
	absBase->condition = baseNeg;
	absBase->trueExpr = std::move(negBase);
	absBase->falseExpr = _base;

	// Ensure exp is biguint
	if (_exp->wtype == awst::WType::uint64Type())
	{
		auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), m_loc);
		itob->stackArgs.push_back(std::move(_exp));
		auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), m_loc);
		_exp = std::move(cast);
	}

	// Compute abs(base) ^ exp using the standard buildBinaryOp (unsigned exp)
	auto* resultType = awst::WType::biguintType();
	auto absResult = m_ctx.buildBinaryOp(
		Token::Exp, std::move(absBase), _exp, resultType, m_loc);

	// Check overflow: absResult must fit in signed range
	// absResult < half for positive result, absResult <= half for negative result
	if (!m_ctx.inUncheckedBlock)
	{
		// expIsOdd: exp % 2 != 0
		auto two = makeBiguintConst("2");
		auto expMod2 = awst::makeBigUIntBinOp(_exp, awst::BigUIntBinaryOperator::Mod, std::move(two), m_loc);
		auto expIsOdd = awst::makeNumericCompare(std::move(expMod2), awst::NumericComparison::Ne, makeBiguintConst("0"), m_loc);

		// resultNeg = baseNeg && expIsOdd
		auto resultNeg = std::make_shared<awst::BooleanBinaryOperation>();
		resultNeg->sourceLocation = m_loc;
		resultNeg->wtype = awst::WType::boolType();
		resultNeg->left = baseNeg;
		resultNeg->op = awst::BinaryBooleanOperator::And;
		resultNeg->right = std::move(expIsOdd);

		// If resultNeg: absResult <= half, else: absResult < half
		auto ltHalf = awst::makeNumericCompare(absResult, awst::NumericComparison::Lt, makeBiguintConst(halfNStr), m_loc);

		auto halfLtRes = awst::makeNumericCompare(makeBiguintConst(halfNStr), awst::NumericComparison::Lt, absResult, m_loc);
		auto leHalf = std::make_shared<awst::Not>();
		leHalf->sourceLocation = m_loc;
		leHalf->wtype = awst::WType::boolType();
		leHalf->expr = std::move(halfLtRes);

		auto rangeOk = std::make_shared<awst::ConditionalExpression>();
		rangeOk->sourceLocation = m_loc;
		rangeOk->wtype = awst::WType::boolType();
		rangeOk->condition = std::move(resultNeg);
		rangeOk->trueExpr = std::move(leHalf);
		rangeOk->falseExpr = std::move(ltHalf);

		auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(rangeOk), m_loc, "signed exp overflow"), m_loc);
		m_ctx.prePendingStatements.push_back(std::move(assertStmt));
	}

	// If base was negative and exp is odd, negate result: (pow2N - absResult) mod pow2N
	auto expMod2_2 = awst::makeBigUIntBinOp(_exp, awst::BigUIntBinaryOperator::Mod, makeBiguintConst("2"), m_loc);
	auto expOdd2 = awst::makeNumericCompare(std::move(expMod2_2), awst::NumericComparison::Ne, makeBiguintConst("0"), m_loc);
	auto shouldNeg = std::make_shared<awst::BooleanBinaryOperation>();
	shouldNeg->sourceLocation = m_loc;
	shouldNeg->wtype = awst::WType::boolType();
	shouldNeg->left = baseNeg;
	shouldNeg->op = awst::BinaryBooleanOperator::And;
	shouldNeg->right = std::move(expOdd2);

	auto negResult = awst::makeBigUIntBinOp(makeBiguintConst(pow2NStr), awst::BigUIntBinaryOperator::Sub, absResult, m_loc);
	auto negMod = awst::makeBigUIntBinOp(std::move(negResult), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);

	// absResult == 0 → don't negate
	auto resZero = awst::makeNumericCompare(absResult, awst::NumericComparison::Eq, makeBiguintConst("0"), m_loc);
	auto notZero = std::make_shared<awst::Not>();
	notZero->sourceLocation = m_loc;
	notZero->wtype = awst::WType::boolType();
	notZero->expr = std::move(resZero);
	auto doNeg = std::make_shared<awst::BooleanBinaryOperation>();
	doNeg->sourceLocation = m_loc;
	doNeg->wtype = awst::WType::boolType();
	doNeg->left = std::move(shouldNeg);
	doNeg->op = awst::BinaryBooleanOperator::And;
	doNeg->right = std::move(notZero);

	auto finalResult = std::make_shared<awst::ConditionalExpression>();
	finalResult->sourceLocation = m_loc;
	finalResult->wtype = awst::WType::biguintType();
	finalResult->condition = std::move(doNeg);
	finalResult->trueExpr = std::move(negMod);
	finalResult->falseExpr = std::move(absResult);

	return finalResult;
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

	// Ensure both operands are biguint
	auto ensureBiguint = [&](std::shared_ptr<awst::Expression> expr)
		-> std::shared_ptr<awst::Expression> {
		if (expr->wtype == awst::WType::biguintType())
			return expr;
		auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), m_loc);
		itob->stackArgs.push_back(std::move(expr));
		auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), m_loc);
		return cast;
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
		auto notExpr = std::make_shared<awst::Not>();
		notExpr->sourceLocation = m_loc;
		notExpr->wtype = awst::WType::boolType();
		notExpr->expr = std::move(cmp);
		return notExpr;
	};

	// abs(val) = val < half ? val : (pow2N - val)
	auto absVal = [&](std::shared_ptr<awst::Expression> const& val) {
		auto neg = isNeg(val);
		auto negated = awst::makeBigUIntBinOp(makeBiguintConst(pow2NStr), awst::BigUIntBinaryOperator::Sub, val, m_loc);
		auto cond = std::make_shared<awst::ConditionalExpression>();
		cond->sourceLocation = m_loc;
		cond->wtype = awst::WType::biguintType();
		cond->condition = std::move(neg);
		cond->trueExpr = std::move(negated);
		cond->falseExpr = val;
		return cond;
	};

	// Checked: assert(y != 0)
	{
		auto bZero = awst::makeNumericCompare(_right, awst::NumericComparison::Ne, makeBiguintConst("0"), m_loc);

		auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(bZero), m_loc, "division by zero"), m_loc);
		m_ctx.prePendingStatements.push_back(std::move(assertStmt));
	}

	// Checked div: assert NOT (x == minVal && y == -1)
	// minVal = half (= 2^(N-1)), -1 = pow2N - 1
	if (isDiv && !m_ctx.inUncheckedBlock)
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

		auto bothTrue = std::make_shared<awst::BooleanBinaryOperation>();
		bothTrue->sourceLocation = m_loc;
		bothTrue->wtype = awst::WType::boolType();
		bothTrue->left = std::move(xIsMin);
		bothTrue->op = awst::BinaryBooleanOperator::And;
		bothTrue->right = std::move(yIsNeg1);

		auto notBoth = std::make_shared<awst::Not>();
		notBoth->sourceLocation = m_loc;
		notBoth->wtype = awst::WType::boolType();
		notBoth->expr = std::move(bothTrue);

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

	auto needNeg = std::make_shared<awst::BooleanBinaryOperation>();
	needNeg->sourceLocation = m_loc;
	needNeg->wtype = awst::WType::boolType();
	needNeg->left = std::move(shouldNegate);
	needNeg->op = awst::BinaryBooleanOperator::And;
	needNeg->right = std::make_shared<awst::Not>();
	dynamic_cast<awst::Not*>(needNeg->right.get())->sourceLocation = m_loc;
	dynamic_cast<awst::Not*>(needNeg->right.get())->wtype = awst::WType::boolType();
	dynamic_cast<awst::Not*>(needNeg->right.get())->expr = std::move(resultIsZero);

	// shouldNegate && result != 0 ? negated : unsigned
	auto finalResult = std::make_shared<awst::ConditionalExpression>();
	finalResult->sourceLocation = m_loc;
	finalResult->wtype = awst::WType::biguintType();
	finalResult->condition = std::move(needNeg);
	finalResult->trueExpr = std::move(negMod);
	finalResult->falseExpr = std::move(unsignedResult);

	// Convert back to uint64 for ≤64-bit types
	if (bits <= 64)
	{
		auto castBytes = awst::makeReinterpretCast(std::move(finalResult), awst::WType::bytesType(), m_loc);

		auto eight = awst::makeIntegerConstant("8", m_loc);
		auto bz = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), m_loc);
		bz->stackArgs.push_back(eight);
		auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), m_loc);
		cat->stackArgs.push_back(std::move(bz));
		cat->stackArgs.push_back(std::move(castBytes));
		auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), m_loc);
		lenCall->stackArgs.push_back(cat);
		auto eight2 = awst::makeIntegerConstant("8", m_loc);
		auto start = awst::makeIntrinsicCall("-", awst::WType::uint64Type(), m_loc);
		start->stackArgs.push_back(std::move(lenCall));
		start->stackArgs.push_back(eight2);
		auto extract = awst::makeIntrinsicCall("extract_uint64", awst::WType::uint64Type(), m_loc);
		extract->stackArgs.push_back(cat);
		extract->stackArgs.push_back(std::move(start));
		return extract;
	}

	return finalResult;
}

} // namespace puyasol::builder::sol_ast

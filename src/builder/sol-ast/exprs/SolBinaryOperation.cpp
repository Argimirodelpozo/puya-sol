/// @file SolBinaryOperation.cpp — migrated from BinaryOperationBuilder.cpp.

#include "builder/sol-types/SolcConstFold.h"
#include "awst/NameGen.h"
#include "builder/sol-ast/EffectScan.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-ast/exprs/SolBinaryOperation.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/sol-eb/BuilderOps.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>

#include "builder/itxn/CallResolver.h"

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
	return eb::CallResolver::buildOperatorCall(m_ctx, *userFunc,
		{&m_binOp.leftExpression(), &m_binOp.rightExpression()}, m_loc);
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

	eb::ContractContext::LoweredValue<std::shared_ptr<awst::Expression>> rhs{
		std::move(right), std::move(rhsD)};
	eb::ContractContext::LoweredValue<std::shared_ptr<awst::Expression>> skipped{
		awst::makeBoolConstant(op == Token::Or, m_loc), {}};
	return op == Token::And
		? m_ctx.emitConditional(std::move(left), std::move(rhs), std::move(skipped),
			awst::WType::boolType(), m_loc)
		: m_ctx.emitConditional(std::move(left), std::move(skipped), std::move(rhs),
			awst::WType::boolType(), m_loc);
}

namespace
{

std::optional<eb::BuilderComparisonOp> comparisonOpFor(Token solOp)
{
	switch (solOp)
	{
	case Token::Equal:              return eb::BuilderComparisonOp::Eq;
	case Token::NotEqual:           return eb::BuilderComparisonOp::Ne;
	case Token::LessThan:           return eb::BuilderComparisonOp::Lt;
	case Token::LessThanOrEqual:    return eb::BuilderComparisonOp::Lte;
	case Token::GreaterThan:        return eb::BuilderComparisonOp::Gt;
	case Token::GreaterThanOrEqual: return eb::BuilderComparisonOp::Gte;
	default: return std::nullopt;
	}
}

std::optional<eb::BuilderBinaryOp> binaryOpFor(Token solOp)
{
	switch (solOp)
	{
	case Token::Add: case Token::AssignAdd: return eb::BuilderBinaryOp::Add;
	case Token::Sub: case Token::AssignSub: return eb::BuilderBinaryOp::Sub;
	case Token::Mul: case Token::AssignMul: return eb::BuilderBinaryOp::Mult;
	case Token::Div: case Token::AssignDiv: return eb::BuilderBinaryOp::FloorDiv;
	case Token::Mod: case Token::AssignMod: return eb::BuilderBinaryOp::Mod;
	case Token::Exp: return eb::BuilderBinaryOp::Pow;
	case Token::SHL: case Token::AssignShl: return eb::BuilderBinaryOp::LShift;
	case Token::SHR: case Token::SAR: case Token::AssignShr: case Token::AssignSar:
		return eb::BuilderBinaryOp::RShift;
	case Token::BitOr: case Token::AssignBitOr: return eb::BuilderBinaryOp::BitOr;
	case Token::BitXor: case Token::AssignBitXor: return eb::BuilderBinaryOp::BitXor;
	case Token::BitAnd: case Token::AssignBitAnd: return eb::BuilderBinaryOp::BitAnd;
	default: return std::nullopt;
	}
}

} // anonymous namespace

std::shared_ptr<awst::Expression> SolBinaryOperation::trySolEbDispatch(
	std::shared_ptr<awst::Expression> left,
	std::shared_ptr<awst::Expression> right)
{
	auto const op = m_binOp.getOperator();
	auto const comparison = comparisonOpFor(op);
	auto const binary = binaryOpFor(op);
	if (!comparison && !binary) return nullptr;
	auto const* leftType = m_binOp.leftExpression().annotation().type;
	auto const* rightType = m_binOp.rightExpression().annotation().type;
	auto const* common = m_binOp.annotation().commonType;
	bool const independentRight = op == Token::Exp || TokenTraits::isShiftOp(op);

	// Solc supplies the operation's common type. Shifts and exponentiation
	// convert only the base to it: their RHS keeps its own mobile type.
	if (dynamic_cast<IntegerType const*>(common))
	{
		auto convert = [&](auto& value, Type const*& type, Type const* destination) {
			TypeCoercion::assertImplicitlyConvertible(type, destination, m_loc, "binop common-type");
			if (type != destination || comparison)
				value = TypeCoercion::coerceToCommonInt(
					std::move(value), type, m_ctx.typeMapper.map(destination), m_loc);
			type = destination;
		};
		convert(left, leftType, common);
		convert(right, rightType, independentRight ? rightType->mobileType() : common);
	}
	else if (binary && common)
	{
		leftType = common;
		// A bytesN shift also takes an integer RHS, including numeric literals.
		if (independentRight) rightType = rightType->mobileType();
	}

	auto lhs = m_ctx.builderForInstance(leftType, left);
	auto rhs = m_ctx.builderForInstance(rightType, right);
	if (!lhs || !rhs) return nullptr;
	auto result = comparison ? lhs->compare(*rhs, *comparison, m_loc)
		: lhs->binary_op(*rhs, *binary, m_loc);
	return result ? result->resolve() : nullptr;
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
	bool const staticNeed = builder::EffectScan::requiresSequencing(m_binOp.leftExpression(), m_ctx)
		|| builder::EffectScan::requiresSequencing(m_binOp.rightExpression(), m_ctx);
	if (m_ctx.viaIRSequencing)
	{
		// The earlier value must be consumed before later queued/inline writes.
		bool const pin = !rd.empty() || !ld.post.empty() || staticNeed;
		left = m_ctx.emitSequencedOperand(std::move(ld), std::move(left), pin, m_loc);
		m_ctx.restoreOperandDeltas(std::move(rd));
	}
	else
	{
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

	// 4. Typed operation dispatch
	if (auto result = trySolEbDispatch(left, right))
		return result;

	// 5. Fallback to buildBinaryOp
	auto built = m_ctx.buildBinaryOp(
		m_binOp.getOperator(), std::move(left), std::move(right), resultType, m_loc);

	// 6. bytesN shift truncation: `bytesN << k` goes through biguint ×2^k
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


} // namespace puyasol::builder::sol_ast

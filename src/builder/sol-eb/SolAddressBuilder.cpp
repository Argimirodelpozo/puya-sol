/// @file SolAddressBuilder.cpp
/// Solidity address/contract type builder.

#include "builder/sol-eb/SolAddressBuilder.h"

namespace puyasol::builder::eb
{

namespace {

/// Two on-chain app-address encodings:
///   - Convention: `\x00*24 + itob(app_id)` — stored addresses; SolExternalCall recovers id.
///   - Hash: `sha512_256("appID"+itob(id))` — what AVM emits for CurrentApplicationAddress
///     and Txn.Sender when called by an app. Non-invertible; compare-only.
///
/// `addr == address(this)` fails naively (convention vs hash). detectAppIdIntrinsic
/// recognises the hash-form AST and returns the underlying app_id so compare() can
/// OR in a convention-form check.
std::shared_ptr<awst::Expression> detectAppIdIntrinsic(
	awst::Expression const* expr, awst::SourceLocation const& loc)
{
	auto const* ic = dynamic_cast<awst::IntrinsicCall const*>(expr);
	if (!ic || ic->immediates.empty())
		return nullptr;
	auto const* imm = std::get_if<std::string>(&ic->immediates[0]);
	if (!imm)
		return nullptr;
	if (ic->opCode == "global" && *imm == "CurrentApplicationAddress")
	{
		auto idCall = awst::makeGlobal(std::string("CurrentApplicationID"), awst::WType::uint64Type(), loc);
		return idCall;
	}
	if (ic->opCode == "txn" && *imm == "Sender")
	{
		// AVM v6+: CallerApplicationID (not a txn field). Returns 0 for user-account callers
		// → convention-form falls to zero address (matches only if the user stored zero).
		auto idCall = awst::makeGlobal(std::string("CallerApplicationID"), awst::WType::uint64Type(), loc);
		return idCall;
	}
	return nullptr;
}

/// Builds `\x00*24 + itob(app_id)` as an accountType expression.
std::shared_ptr<awst::Expression> makeConventionFormAddress(
	std::shared_ptr<awst::Expression> appId, awst::SourceLocation const& loc)
{
	auto itob = awst::makeItob(std::move(appId), loc);
	auto pad = awst::makeLeftPad(std::move(itob), 24, loc);
	return awst::makeAsAccount(std::move(pad), loc);
}

std::shared_ptr<awst::Expression> makeBytesEq(
	std::shared_ptr<awst::Expression> a, std::shared_ptr<awst::Expression> b,
	awst::EqualityComparison op, awst::SourceLocation const& loc)
{
	return awst::makeBytesComparison(std::move(a), op, std::move(b), loc);
}


} // namespace

std::unique_ptr<InstanceBuilder> SolAddressBuilder::compare(
	InstanceBuilder& _other, BuilderComparisonOp _op,
	awst::SourceLocation const& _loc)
{
	if (_op != BuilderComparisonOp::Eq && _op != BuilderComparisonOp::Ne)
		return nullptr;

	auto* otherWType = _other.wtype();
	bool otherIsAccount = otherWType == awst::WType::accountType();
	bool otherIsBytes = otherWType && otherWType->kind() == awst::WTypeKind::Bytes;
	if (!otherIsAccount && !otherIsBytes)
		return nullptr;

	auto lhs = resolve();
	auto rhs = _other.resolve();

	auto coerceToBytes = [&](std::shared_ptr<awst::Expression>& expr) {
		if (expr->wtype != awst::WType::bytesType()
			&& expr->wtype != awst::WType::accountType())
		{
			auto cast = awst::makeAsBytes(std::move(expr), _loc);
			expr = std::move(cast);
		}
	};
	if (lhs->wtype != rhs->wtype)
	{
		coerceToBytes(lhs);
		coerceToBytes(rhs);
	}

	// Hash/convention bridge:
	//   addr == address(this)  →  (addr == hash-form) || (addr == \x00*24+id)
	//   msg.sender == addr     →  (sender == addr)   || (\x00*24+caller_id == addr)
	// The OR arm handles apps whose address was stored convention-form. != is negated equality.
	// detectAppIdIntrinsic matches only literal intrinsic shapes; arithmetic/state-read
	// addresses fall through to plain bytes comparison.
	auto leftAppId = detectAppIdIntrinsic(lhs.get(), _loc);
	auto rightAppId = detectAppIdIntrinsic(rhs.get(), _loc);
	if ((leftAppId || rightAppId) && !(leftAppId && rightAppId))
	{
		// Compute the side decision BEFORE moving appId: std::move nullifies the ptr,
		// so `leftAppId ? ...` after move always takes the rightAppId branch.
		bool leftIsIntrinsic = static_cast<bool>(leftAppId);
		auto appId = leftIsIntrinsic ? std::move(leftAppId) : std::move(rightAppId);
		auto& storedSlot = leftIsIntrinsic ? rhs : lhs;
		auto& intrinSlot = leftIsIntrinsic ? lhs : rhs;

		// Second copy for the !=0 guard (both copies are identical IntrinsicCall → same TEAL).
		auto appIdForCompare = detectAppIdIntrinsic(intrinSlot.get(), _loc);

		// storedSlot feeds two BytesComparisons: pin so a call-valued side
		// evaluates once (T2; a shared shared_ptr still re-emits per parent).
		storedSlot = awst::makeEvalOnce(std::move(storedSlot), _loc);
		auto convention = makeConventionFormAddress(std::move(appId), _loc);
		auto direct = makeBytesEq(
			std::move(intrinSlot), storedSlot, awst::EqualityComparison::Eq, _loc);
		auto conventional = makeBytesEq(
			std::move(convention), std::move(storedSlot), awst::EqualityComparison::Eq, _loc);

		// Gate convention arm on appId!=0: CallerApplicationID is 0 for user-account callers;
		// without guard, conv-form collapses to zero address and `msg.sender==address(0)`
		// spuriously succeeds. CurrentApplicationID is always >0 (guard is a no-op there).
		auto zeroId = awst::makeZero(_loc);
		auto appIdNonZero = awst::makeNumericCompare(
			std::move(appIdForCompare), awst::NumericComparison::Ne,
			std::move(zeroId), _loc);
		auto guardedConv = awst::makeBoolBinOp(
			std::move(appIdNonZero), awst::BinaryBooleanOperator::And,
			std::move(conventional), _loc);

		std::shared_ptr<awst::Expression> result = awst::makeBoolBinOp(
			std::move(direct), awst::BinaryBooleanOperator::Or,
			std::move(guardedConv), _loc);
		if (_op == BuilderComparisonOp::Ne)
			result = awst::makeNot(std::move(result), _loc);

		return std::make_unique<SolAddressBuilder>(m_ctx, m_solType, std::move(result));
	}

	auto e = makeBytesEq(
		std::move(lhs), std::move(rhs),
		(_op == BuilderComparisonOp::Eq)
			? awst::EqualityComparison::Eq
			: awst::EqualityComparison::Ne,
		_loc);
	return std::make_unique<SolAddressBuilder>(m_ctx, m_solType, std::move(e));
}

std::unique_ptr<InstanceBuilder> SolAddressBuilder::bool_eval(
	awst::SourceLocation const& _loc, bool _negate)
{
	auto zero = awst::makeBytesConstant(
		std::vector<uint8_t>(32, 0), _loc, awst::BytesEncoding::Base16,
		awst::WType::accountType());

	auto e = awst::makeBytesComparison(resolve(),
		_negate ? awst::EqualityComparison::Eq : awst::EqualityComparison::Ne,
		std::move(zero), _loc);
	return std::make_unique<SolAddressBuilder>(m_ctx, m_solType, std::move(e));
}

} // namespace puyasol::builder::eb

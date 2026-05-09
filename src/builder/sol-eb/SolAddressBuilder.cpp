/// @file SolAddressBuilder.cpp
/// Solidity address/contract type builder.

#include "builder/sol-eb/SolAddressBuilder.h"

namespace puyasol::builder::eb
{

namespace {

/// puya-sol uses two on-chain encodings for app addresses:
///
///   * Convention form: `\x00*24 + itob(app_id)` — used for stored
///     addresses so SolExternalCall::addressToAppId can recover the
///     app id by reading the trailing 8 bytes for inner-call dispatch.
///   * Hash form: `sha512_256("appID" + itob(app_id))` — what the AVM
///     itself emits for `address(this)` (global CurrentApplicationAddress)
///     and for `Txn.Sender` when an app-account is the caller. There is
///     no inverse hash on AVM, so a hash-form address can't be turned
///     back into an app id at runtime — it can only be compared.
///
/// A naïve `addr == address(this)` comparison fails: one side is the
/// convention form (whatever the caller stored), the other is the hash
/// form. Same problem with `msg.sender == addr`. The contract author
/// thinks both sides are "address of app X" and expects equality.
///
/// detectAppIdIntrinsic looks for the AST shape of these hash-form
/// intrinsics and returns an expression for the underlying app id, so
/// the comparison can be augmented with a convention-form check (see
/// makeConventionFormAddress + SolAddressBuilder::compare below).
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
		auto idCall = awst::makeIntrinsicCall("global", awst::WType::uint64Type(), loc);
		idCall->immediates = {std::string("CurrentApplicationID")};
		return idCall;
	}
	if (ic->opCode == "txn" && *imm == "Sender")
	{
		// AVM v6+: caller's app id is exposed as `global CallerApplicationID`,
		// not as a txn field. It returns 0 when the call comes from a user
		// account, which (correctly) makes the convention-form check fall
		// to \x00*24 + itob(0) = the zero address — equal to a stored
		// address only when the user explicitly stored zero.
		auto idCall = awst::makeIntrinsicCall("global", awst::WType::uint64Type(), loc);
		idCall->immediates = {std::string("CallerApplicationID")};
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
	return awst::makeReinterpretCast(std::move(pad), awst::WType::accountType(), loc);
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
	// Address supports only Eq/Ne comparison (bytes-backed)
	if (_op != BuilderComparisonOp::Eq && _op != BuilderComparisonOp::Ne)
		return nullptr;

	// Accept other address/account types, or bytes-backed types
	auto* otherWType = _other.wtype();
	bool otherIsAccount = otherWType == awst::WType::accountType();
	bool otherIsBytes = otherWType && otherWType->kind() == awst::WTypeKind::Bytes;
	if (!otherIsAccount && !otherIsBytes)
		return nullptr;

	auto lhs = resolve();
	auto rhs = _other.resolve();

	// Coerce to same type for comparison
	auto coerceToBytes = [&](std::shared_ptr<awst::Expression>& expr) {
		if (expr->wtype != awst::WType::bytesType()
			&& expr->wtype != awst::WType::accountType())
		{
			auto cast = awst::makeReinterpretCast(std::move(expr), awst::WType::bytesType(), _loc);
			expr = std::move(cast);
		}
	};
	if (lhs->wtype != rhs->wtype)
	{
		coerceToBytes(lhs);
		coerceToBytes(rhs);
	}

	// Hash-form / convention-form address-equality bridge.
	//
	//   addr == address(this)  →  (addr == app-hash) || (addr == \x00*24+id)
	//   msg.sender == addr     →  (sender == addr)   || (\x00*24+caller_id == addr)
	//
	// The OR'd second arm lets the comparison succeed when the user
	// stored an app's address in convention form (which is what every
	// inter-app stored address in puya-sol uses). The first arm preserves
	// the natural semantics for non-app addresses and for the rare case
	// where an address is stored hash-form. !=  is the negation of the
	// same equality check, so we compute the equality and negate at the
	// end. See the detectAppIdIntrinsic comment block above for the
	// background on why two encodings exist.
	//
	// detectAppIdIntrinsic walks the AST for the literal intrinsic shape;
	// addresses derived from arithmetic / state reads / etc. fall through
	// to the regular bytes-equality path (which is what they always got).
	auto leftAppId = detectAppIdIntrinsic(lhs.get(), _loc);
	auto rightAppId = detectAppIdIntrinsic(rhs.get(), _loc);
	if ((leftAppId || rightAppId) && !(leftAppId && rightAppId))
	{
		// Pick the intrinsic side; the other side is the "stored address"
		// we're comparing against. CAREFUL: must compute the side decision
		// BEFORE moving the appId, because std::move nullifies the source
		// shared_ptr, and a subsequent `leftAppId ? ...` would always go
		// down the rightAppId branch (silently swapping the two sides).
		bool leftIsIntrinsic = static_cast<bool>(leftAppId);
		auto appId = leftIsIntrinsic ? std::move(leftAppId) : std::move(rightAppId);
		auto& storedSlot = leftIsIntrinsic ? rhs : lhs;
		auto& intrinSlot = leftIsIntrinsic ? lhs : rhs;

		// Build a second copy of the appId expression for the !=0 guard
		// (we'll move one copy into the convention builder, the other is
		// the operand of the comparison). Both copies are the same
		// IntrinsicCall structure so puya emits identical TEAL for them.
		auto appIdForCompare = detectAppIdIntrinsic(intrinSlot.get(), _loc);

		// Two equality checks share the stored-side expression. AWST
		// nodes are immutable post-construction, so re-using the same
		// shared_ptr in two parents is safe — puya consumes the tree
		// view, not the graph identity.
		auto convention = makeConventionFormAddress(std::move(appId), _loc);
		auto direct = makeBytesEq(
			std::move(intrinSlot), storedSlot, awst::EqualityComparison::Eq, _loc);
		auto conventional = makeBytesEq(
			std::move(convention), std::move(storedSlot), awst::EqualityComparison::Eq, _loc);

		// Gate the conventional arm on `appId != 0`. CallerApplicationID
		// is 0 for user-account callers; without the guard the conv-form
		// expression collapses to the zero address, making any
		// `msg.sender == address(0)` (or `addr == address(this)` where
		// `addr` happens to be the zero address) spuriously succeed.
		// CurrentApplicationID is always > 0 inside an app's program, so
		// the guard is a no-op there but free to keep for symmetry.
		auto zeroId = awst::makeIntegerConstant("0", _loc);
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
	// address is truthy if != zero_address (32 zero bytes)
	auto zero = awst::makeBytesConstant(
		std::vector<uint8_t>(32, 0), _loc, awst::BytesEncoding::Base16,
		awst::WType::accountType());

	auto e = awst::makeBytesComparison(resolve(),
		_negate ? awst::EqualityComparison::Eq : awst::EqualityComparison::Ne,
		std::move(zero), _loc);
	return std::make_unique<SolAddressBuilder>(m_ctx, m_solType, std::move(e));
}

} // namespace puyasol::builder::eb

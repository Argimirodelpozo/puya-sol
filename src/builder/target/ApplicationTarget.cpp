#include "builder/target/ApplicationTarget.h"
#include "builder/AwstShorthand.h"
#include "builder/target/TargetProfile.h"

namespace puyasol::builder
{

ApplicationTarget::Expr ApplicationTarget::canonicalId(Expr address, awst::SourceLocation const& loc)
{
	auto bytes = awst::makeEvalOnce(
		awst::makeLeftPadToN(awst::makeAsBytes(std::move(address), loc), 32, loc), loc);
	auto canonical = awst::makeBytesComparison(awst::makeExtract(bytes, 0, 24, loc),
		awst::EqualityComparison::Eq, awst::makeBzero(24, loc), loc);
	return awst::makeConditional(std::move(canonical), awst::makeWord32ToUInt64(bytes, loc),
		awst::makeZero(loc), awst::WType::uint64Type(), loc);
}

ApplicationTarget::Expr ApplicationTarget::resolve(
	TargetProfile const& profile, Expr address, awst::SourceLocation const& loc)
{
	if (address->wtype == awst::WType::applicationType())
		return awst::makeAsUInt64(std::move(address), loc);
	auto currentId = awst::makeGlobal("CurrentApplicationID", awst::WType::uint64Type(), loc);
	if (shorthand::isCurrentAppAddressGlobal(address.get())) return currentId;
	if (address->wtype == awst::WType::uint64Type())
		address = awst::makeItob(std::move(address), loc);
	Expr bytes = awst::makeLeftPadToN(awst::makeAsBytes(std::move(address), loc), 32, loc);
	Expr current = awst::makeGlobal("CurrentApplicationAddress", awst::WType::bytesType(), loc);
	// Solidity's EVM address namespace is 160 bits, including dirty Yul words.
	// ARC4 accounts preserve the complete native 32-byte identity.
	if (profile.contractAbi == ContractAbi::Evm)
	{
		bytes = awst::makeLeftPad(awst::makeExtractLastN(std::move(bytes), 20, loc), 12, loc);
		current = awst::makeLeftPad(awst::makeExtractLastN(std::move(current), 20, loc), 12, loc);
	}
	bytes = awst::makeEvalOnce(std::move(bytes), loc);
	auto self = awst::makeBytesComparison(bytes, awst::EqualityComparison::Eq, std::move(current), loc);
	return awst::makeConditional(std::move(self), std::move(currentId), canonicalId(bytes, loc),
		awst::WType::uint64Type(), loc);
}

ApplicationTarget::Expr ApplicationTarget::pointerId(
	TargetProfile const& profile, Expr address, awst::SourceLocation const& loc)
{
	if (address->wtype == awst::WType::applicationType())
		return awst::makeAsUInt64(std::move(address), loc);
	if (address->wtype == awst::WType::uint64Type()) address = awst::makeItob(std::move(address), loc);
	address = awst::makeLeftPadToN(awst::makeAsBytes(std::move(address), loc), 32, loc);
	if (profile.contractAbi == ContractAbi::Evm)
		address = awst::makeLeftPad(awst::makeExtractLastN(std::move(address), 20, loc), 12, loc);
	address = awst::makeEvalOnce(std::move(address), loc);
	auto id = awst::makeEvalOnce(resolve(profile, address, loc), loc);
	auto valid = awst::makeBoolBinOp(
		awst::makeNumericCompare(id, awst::NumericComparison::Ne, awst::makeZero(loc), loc),
		awst::BinaryBooleanOperator::Or,
		awst::makeBytesComparison(address, awst::EqualityComparison::Eq, awst::makeBzero(32, loc), loc), loc);
	static awst::WTuple resultType({awst::WType::uint64Type(), awst::WType::boolType()});
	auto pair = awst::makeTupleExpression(&resultType, loc);
	pair->items = {id, std::move(valid)};
	auto checked = std::make_shared<awst::CheckedMaybe>();
	checked->expr = std::move(pair);
	checked->wtype = awst::WType::uint64Type();
	checked->sourceLocation = loc;
	checked->comment = "function pointer target is not an application address";
	return checked;
}

ApplicationTarget::Expr ApplicationTarget::requireApplication(Expr id, awst::SourceLocation const& loc)
{
	id = awst::makeEvalOnce(awst::makeAsUInt64(std::move(id), loc), loc);
	static awst::WTuple resultType({awst::WType::uint64Type(), awst::WType::boolType()});
	auto pair = awst::makeTupleExpression(&resultType, loc);
	pair->items = {id, awst::makeNumericCompare(id, awst::NumericComparison::Ne, awst::makeZero(loc), loc)};
	auto checked = std::make_shared<awst::CheckedMaybe>();
	checked->expr = std::move(pair);
	checked->wtype = awst::WType::uint64Type();
	checked->sourceLocation = loc;
	checked->comment = "call target is not an application address";
	return awst::makeAsApplication(std::move(checked), loc);
}

} // namespace puyasol::builder

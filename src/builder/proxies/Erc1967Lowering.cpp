/// @file Erc1967Lowering.cpp
/// EIP-1967 proxy-slot recognition and native-update lowering (proxy.md §1).

#include "builder/proxies/Erc1967Lowering.h"

#include "Logger.h"

namespace puyasol::builder::proxies
{

namespace
{

// The three EIP-1967 slots, as the decimal spellings IntegerConstant carries.
// keccak256("eip1967.proxy.implementation") - 1
constexpr char const* IMPL_SLOT_DEC =
	"24440054405305269366569402256811496959409073762505157381672968839269610695612";
// keccak256("eip1967.proxy.admin") - 1
constexpr char const* ADMIN_SLOT_DEC =
	"81955473079516046949633743016697847541294818689821282749996681496272635257091";
// keccak256("eip1967.proxy.beacon") - 1
constexpr char const* BEACON_SLOT_DEC =
	"74152234768234802001998023604048924213078445070507226371336425913862612794704";

std::shared_ptr<awst::Expression> adminTarget(awst::SourceLocation const& _loc)
{
	return awst::makeAppStateExpression(
		awst::makeUtf8BytesConstant(Erc1967Lowering::ADMIN_KEY, _loc,
			awst::WType::stateKeyType()),
		awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> senderAsBiguint(awst::SourceLocation const& _loc)
{
	// msg.sender's stored form everywhere in this compiler is the raw
	// 32-byte account, so the gate compares the same representation any
	// Solidity-side admin write produced.
	return awst::makeAsBiguint(
		awst::makeReinterpretCast(
			awst::makeTxn("Sender", awst::WType::accountType(), _loc),
			awst::WType::bytesType(), _loc),
		_loc);
}

} // namespace

Erc1967Slot Erc1967Lowering::classify(awst::Expression const* _slotExpr)
{
	auto const* c = dynamic_cast<awst::IntegerConstant const*>(_slotExpr);
	if (!c)
		return Erc1967Slot::None;
	if (c->value == IMPL_SLOT_DEC)
		return Erc1967Slot::Implementation;
	if (c->value == ADMIN_SLOT_DEC)
		return Erc1967Slot::Admin;
	if (c->value == BEACON_SLOT_DEC)
		return Erc1967Slot::Beacon;
	return Erc1967Slot::None;
}

std::shared_ptr<awst::Expression> Erc1967Lowering::adminLoad(
	awst::SourceLocation const& _loc)
{
	return awst::makeStateGet(
		adminTarget(_loc),
		awst::makeBiguintConstant("0", _loc),
		awst::WType::biguintType(), _loc);
}

void Erc1967Lowering::adminStore(
	std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	_out.push_back(awst::makeAssignmentStatement(
		adminTarget(_loc), std::move(_value), _loc));
}

std::shared_ptr<awst::Expression> Erc1967Lowering::implementationLoad(
	awst::SourceLocation const& _loc)
{
	// This app IS the implementation: bytes24 ++ itob(app id), the same
	// contract-value convention address(this) uses.
	return awst::makeAsBiguint(
		awst::makeConcat(
			awst::makeBzero(24, _loc),
			awst::makeItob(
				awst::makeGlobal("CurrentApplicationID",
					awst::WType::uint64Type(), _loc),
				_loc),
			_loc),
		_loc);
}

std::shared_ptr<awst::Statement> Erc1967Lowering::trapStatement(
	Erc1967Slot _slot, bool _isStore, awst::SourceLocation const& _loc)
{
	std::string message;
	if (_slot == Erc1967Slot::Implementation)
		message =
			"ERC-1967 upgrade has no in-contract lowering: the AVM upgrade is a "
			"native UpdateApplication transaction submitted by the admin with "
			"the new compiled program (see proxy.md)";
	else
		message =
			"ERC-1967 beacon slot has no AVM equivalent (see proxy.md: beacon "
			"patterns need grouped native updates or a re-architecture)";
	return awst::makeExpressionStatement(
		awst::makeAssert(awst::makeFalse(_loc), _loc, std::move(message)), _loc);
}

awst::AppStorageDefinition Erc1967Lowering::adminStateDefinition(
	awst::SourceLocation const& _loc)
{
	awst::AppStorageDefinition def;
	def.memberName = ADMIN_KEY;
	def.sourceLocation = _loc;
	def.storageKind = awst::AppStorageKind::AppGlobal;
	def.storageWType = awst::WType::biguintType();
	def.key = awst::makeUtf8BytesConstant(ADMIN_KEY, _loc,
		awst::WType::stateKeyType());
	return def;
}

awst::ContractMethod Erc1967Lowering::updateGateMethod(
	std::string const& _cref, awst::SourceLocation const& _loc)
{
	awst::ContractMethod method;
	method.sourceLocation = _loc;
	method.cref = _cref;
	method.memberName = "__erc1967_update";
	method.returnType = awst::WType::voidType();

	auto body = awst::makeBlock(_loc);
	auto isAdmin = awst::makeNumericCompare(
		adminLoad(_loc), awst::NumericComparison::Eq,
		senderAsBiguint(_loc), _loc);
	body->body.push_back(awst::makeExpressionStatement(
		awst::makeAssert(std::move(isAdmin), _loc,
			"ERC-1967: update sender is not the proxy admin"),
		_loc));
	body->body.push_back(awst::makeReturnStatement(nullptr, _loc));
	method.body = std::move(body);

	awst::ARC4BareMethodConfig config;
	config.sourceLocation = _loc;
	config.allowedCompletionTypes = {4}; // UpdateApplication only
	config.create = 3;                   // never on create
	method.arc4MethodConfig = config;
	return method;
}

} // namespace puyasol::builder::proxies

/// @file Erc1967Lowering.cpp
/// EIP-1967 proxy-slot recognition and native-update lowering (proxy.md §1).

#include "builder/proxies/Erc1967Lowering.h"

#include "awst/Visit.h"
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

// The same three slots as lowercase hex (no 0x) — the 32-byte BytesConstant
// spelling a Solidity-level `bytes32 constant` takes outside assembly.
constexpr char const* IMPL_SLOT_HEX =
	"360894a13ba1a3210667c828492db98dca3e2076cc3735a920a3ca505d382bbc";
constexpr char const* ADMIN_SLOT_HEX =
	"b53127684a568b3173ae13b9f8a6016e243e63b6e8ee1178d6a717850b5d6103";
constexpr char const* BEACON_SLOT_HEX =
	"a3f0ad74e5423aebfd80d3ef4346578335a9a72aeaee59ff6cb3582b35133d50";

std::shared_ptr<awst::Expression> adminTarget(awst::SourceLocation const& _loc)
{
	return awst::makeAppStateExpression(
		awst::makeUtf8BytesConstant(Erc1967Lowering::ADMIN_KEY, _loc,
			awst::WType::stateKeyType()),
		awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> ownIdentityBytes(awst::SourceLocation const& _loc)
{
	// bytes24 ++ itob(app id): the contract-value convention address(this)
	// uses; the identity model means the "implementation address" IS the app.
	return awst::makeConcat(
		awst::makeBzero(24, _loc),
		awst::makeItob(
			awst::makeGlobal("CurrentApplicationID",
				awst::WType::uint64Type(), _loc),
			_loc),
		_loc);
}

std::shared_ptr<awst::Statement> upgradedEventStatement(
	awst::SourceLocation const& _loc)
{
	// ARC-28 Upgraded(address), the EIP-1967 event signature: the update gate
	// runs inside the UpdateApplication txn, so this marks each native
	// upgrade for indexers exactly where EVM's upgradeTo would have emitted.
	// Statics: WTypes must outlive the AWST; mirrors TypeMapper's address
	// mapping (ARC4StaticArray<byte,32> aliased "address").
	static awst::ARC4UIntN const byte8(8);
	static awst::ARC4StaticArray const addrType(&byte8, 32, "address");
	static awst::ARC4Struct const upgradedType(
		"Upgraded", {{"implementation", &addrType}}, true);
	auto impl = awst::makeReinterpretCast(
		ownIdentityBytes(_loc), awst::WType::accountType(), _loc);
	auto value = awst::makeNewStruct(&upgradedType, _loc);
	value->values["implementation"] =
		awst::makeARC4Encode(std::move(impl), &addrType, _loc);
	return awst::makeExpressionStatement(
		awst::makeEmit("Upgraded(address)", std::move(value), _loc), _loc);
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

Erc1967Slot Erc1967Lowering::classifyValue(awst::Expression const* _expr)
{
	if (auto slot = classify(_expr); slot != Erc1967Slot::None)
		return slot;
	auto const* b = dynamic_cast<awst::BytesConstant const*>(_expr);
	if (!b || b->value.size() != 32)
		return Erc1967Slot::None;
	static char const* hexDigits = "0123456789abcdef";
	std::string hex;
	hex.reserve(64);
	for (uint8_t byte: b->value)
	{
		hex.push_back(hexDigits[byte >> 4]);
		hex.push_back(hexDigits[byte & 0xf]);
	}
	if (hex == IMPL_SLOT_HEX)
		return Erc1967Slot::Implementation;
	if (hex == ADMIN_SLOT_HEX)
		return Erc1967Slot::Admin;
	if (hex == BEACON_SLOT_HEX)
		return Erc1967Slot::Beacon;
	return Erc1967Slot::None;
}

char const* Erc1967Lowering::slotName(Erc1967Slot _slot)
{
	switch (_slot)
	{
	case Erc1967Slot::Admin: return "admin";
	case Erc1967Slot::Implementation: return "implementation";
	case Erc1967Slot::Beacon: return "beacon";
	case Erc1967Slot::None: break;
	}
	return "none";
}

namespace
{

void warnEscapedSlot(
	awst::Expression const& _expression, std::set<Erc1967Slot>& _warned)
{
	auto slot = Erc1967Lowering::classifyValue(&_expression);
	if (slot == Erc1967Slot::None || !_warned.insert(slot).second)
		return;
	Logger::instance().warning(
		std::string("ERC-1967 ") + Erc1967Lowering::slotName(slot)
		+ " slot constant escapes into a runtime context (function argument, "
		"memory, or arithmetic) that puya-sol cannot classify — storage "
		"reads/writes through a DERIVED slot value are NOT lowered to the "
		"native proxy model and will split from it (e.g. OZ "
		"StorageSlot.getAddressSlot(SLOT).value). Restructure to sload/sstore "
		"directly on the slot constant (see proxy.md)",
		_expression.sourceLocation);
}

} // namespace

void Erc1967Lowering::warnEscapedSlotConstants(
	awst::ContractMethod const& _method, std::set<Erc1967Slot>& _warned)
{
	awst::visitExpressions(_method, [&](awst::Expression const& e) {
		warnEscapedSlot(e, _warned);
	});
}

void Erc1967Lowering::warnEscapedSlotConstants(
	awst::Statement const& _root, std::set<Erc1967Slot>& _warned)
{
	awst::visitExpressions(_root, [&](awst::Expression const& e) {
		warnEscapedSlot(e, _warned);
	});
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
	// This app IS the implementation.
	return awst::makeAsBiguint(ownIdentityBytes(_loc), _loc);
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
	auto adminVar = [&] {
		return awst::makeVarExpression(
			"__erc1967_gate_admin", awst::WType::biguintType(), _loc);
	};
	auto okVar = [&] {
		return awst::makeVarExpression(
			"__erc1967_gate_ok", awst::WType::boolType(), _loc);
	};
	body->body.push_back(awst::makeAssignmentStatement(
		adminVar(), adminLoad(_loc), _loc));
	// Account-form admin: the raw 32-byte account equals the sender.
	body->body.push_back(awst::makeAssignmentStatement(
		okVar(),
		awst::makeNumericCompare(adminVar(), awst::NumericComparison::Eq,
			senderAsBiguint(_loc), _loc),
		_loc));
	// Contract-form admin (bytes24 ++ app id — the ProxyAdmin topology): the
	// stored word can never equal a sender account (app escrows are sha512_256
	// digests), so match the sender against that application's ESCROW address
	// instead — the Txn.Sender an admin app's inner UpdateApplication carries.
	// Gated on the value fitting uint64; a real account with 24 leading zero
	// bytes is unconstructible. A missing app reads exists=false (never trust
	// the value arm: app_params_get pushes uint64 0 for it) — fail closed.
	{
		auto isAppForm = awst::makeBoolBinOp(
			awst::makeNumericCompare(adminVar(), awst::NumericComparison::Ne,
				awst::makeBiguintConstant("0", _loc), _loc),
			awst::BinaryBooleanOperator::And,
			awst::makeNumericCompare(adminVar(), awst::NumericComparison::Lte,
				awst::makeIntegerConstant("18446744073709551615", _loc,
					awst::WType::biguintType()), _loc),
			_loc);
		auto cond = awst::makeBoolBinOp(
			awst::makeNot(okVar(), _loc), awst::BinaryBooleanOperator::And,
			std::move(isAppForm), _loc);

		auto thenBlk = awst::makeBlock(_loc);
		// Statics: WTypes must outlive the AWST (same pattern as the event).
		static awst::WTuple const appAddrTuple(
			{awst::WType::bytesType(), awst::WType::boolType()});
		auto appId = awst::makeWord32ToUInt64(
			awst::makeLeftPadToN(
				awst::makeAsBytes(adminVar(), _loc), 32, _loc),
			_loc);
		auto tupleVar = [&] {
			return awst::makeVarExpression(
				"__erc1967_gate_app", &appAddrTuple, _loc);
		};
		thenBlk->body.push_back(awst::makeAssignmentStatement(
			tupleVar(),
			awst::makeAppParamsGet(
				"AppAddress", std::move(appId), &appAddrTuple, _loc),
			_loc));
		auto exists = awst::makeTupleItem(
			tupleVar(), 1, awst::WType::boolType(), _loc);
		auto escrow = awst::makeAsBiguint(
			awst::makeTupleItem(tupleVar(), 0, awst::WType::bytesType(), _loc),
			_loc);
		thenBlk->body.push_back(awst::makeAssignmentStatement(
			okVar(),
			awst::makeBoolBinOp(
				std::move(exists), awst::BinaryBooleanOperator::And,
				awst::makeNumericCompare(std::move(escrow),
					awst::NumericComparison::Eq, senderAsBiguint(_loc), _loc),
				_loc),
			_loc));
		body->body.push_back(awst::makeIfElse(
			std::move(cond), std::move(thenBlk), nullptr, _loc));
	}
	body->body.push_back(awst::makeExpressionStatement(
		awst::makeAssert(okVar(), _loc,
			"ERC-1967: update sender is not the proxy admin"),
		_loc));
	body->body.push_back(upgradedEventStatement(_loc));
	body->body.push_back(awst::makeReturnStatement(nullptr, _loc));
	method.body = std::move(body);

	// ABI (not bare) on purpose: puya aggregates ARC-56 events only from
	// ABI methods (arc56.py filters isinstance ARC4ABIMethod), so bare would
	// silently drop the Upgraded registration — and a declared method also
	// surfaces the update surface in the app spec for ARC-56-aware tooling.
	// The ceremony gains one field: the update txn carries the method
	// selector in ApplicationArgs[0]; a BARE update is rejected (fail-closed).
	awst::ARC4ABIMethodConfig config;
	config.sourceLocation = _loc;
	config.allowedCompletionTypes = {4}; // UpdateApplication only
	config.create = 3;                   // never on create
	config.name = "__erc1967_update";
	method.arc4MethodConfig = config;
	return method;
}

} // namespace puyasol::builder::proxies

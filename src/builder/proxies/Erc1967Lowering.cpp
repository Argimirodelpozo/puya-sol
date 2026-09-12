/// @file Erc1967Lowering.cpp
/// EIP-1967 proxy-slot recognition and native-update lowering (proxy.md §1).

#include "builder/proxies/Erc1967Lowering.h"

#include "builder/BuildArtifacts.h"
#include "builder/sol-types/TypeCoercion.h"
#include "awst/Visit.h"
#include "Logger.h"

#include <libsolutil/Numeric.h>
#include <array>

namespace puyasol::builder::proxies
{

namespace
{

// One canonical numeric word per keccak256("eip1967.proxy.<name>") - 1.
struct SlotDescriptor
{
	Erc1967Slot kind;
	char const* name;
	solidity::u256 word;
};
std::array<SlotDescriptor, 3> const slots{{
	{Erc1967Slot::Implementation, "implementation",
		solidity::u256("0x360894a13ba1a3210667c828492db98dca3e2076cc3735a920a3ca505d382bbc")},
	{Erc1967Slot::Admin, "admin",
		solidity::u256("0xb53127684a568b3173ae13b9f8a6016e243e63b6e8ee1178d6a717850b5d6103")},
	{Erc1967Slot::Beacon, "beacon",
		solidity::u256("0xa3f0ad74e5423aebfd80d3ef4346578335a9a72aeaee59ff6cb3582b35133d50")},
}};

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
		awst::makeEmit(std::move(value), _loc), _loc);
}

std::shared_ptr<awst::Expression> senderAsBiguint(awst::SourceLocation const& _loc)
{
	// Native escrow authorization deliberately retains the complete sender,
	// independently of the Solidity logical-address profile.
	return awst::makeAsBiguint(
		awst::makeReinterpretCast(
			awst::makeTxn("Sender", awst::WType::accountType(), _loc),
			awst::WType::bytesType(), _loc),
		_loc);
}

} // namespace

std::shared_ptr<awst::Statement> Erc1967Lowering::upgradedEvent(
	awst::SourceLocation const& _loc)
{
	return upgradedEventStatement(_loc);
}



std::shared_ptr<awst::Block> Erc1967Lowering::utilsFoldBody(
	UtilsFold _fold,
	awst::WType const* _returnType,
	std::vector<awst::SubroutineArgument> const& _args,
	BuildArtifacts& _artifacts,
	awst::SourceLocation const& _loc)
{
	auto body = awst::makeBlock(_loc);
	auto returnDefault = [&] {
		body->body.push_back(awst::makeReturnStatement(
			_returnType == awst::WType::voidType()
				? nullptr
				: TypeCoercion::makeDefaultValue(_returnType, _loc),
			_loc));
	};
	switch (_fold)
	{
	case UtilsFold::ImplementationLoad:
		body->body.push_back(awst::makeReturnStatement(
			awst::makeReinterpretCast(
				ownIdentityBytes(_loc), _returnType, _loc),
			_loc));
		break;
	case UtilsFold::AdminLoad:
		_artifacts.noteErc1967AdminUse();
		body->body.push_back(awst::makeReturnStatement(
			awst::makeReinterpretCast(
				awst::makeLeftPadToN(
					awst::makeAsBytes(adminLoad(_loc), _loc), 32, _loc),
				_returnType, _loc),
			_loc));
		break;
	case UtilsFold::AdminStore:
	{
		_artifacts.noteErc1967AdminUse();
		auto const& arg = _args.at(0); // Signature validated by ProxyFacts.
		auto value = awst::makeAsBiguint(awst::makeAsBytes(
			awst::makeVarExpression(arg.name, arg.wtype, _loc), _loc), _loc);
		adminStore(std::move(value), _loc, body->body);
		returnDefault();
		break;
	}
	case UtilsFold::TrapImplementation:
	case UtilsFold::TrapBeacon:
		// assert(false) is terminal to puya — no return after it.
		body->body.push_back(trapStatement(
			_fold == UtilsFold::TrapImplementation
				? Erc1967Slot::Implementation
				: Erc1967Slot::Beacon,
			_loc));
		break;
	case UtilsFold::None:
		returnDefault();
		break;
	}
	return body;
}

Erc1967Slot Erc1967Lowering::classify(awst::Expression const* _slotExpr)
{
	if (auto const* value = dynamic_cast<awst::IntegerConstant const*>(_slotExpr))
		for (auto const& slot: slots)
			if (value->value == slot.word.str()) return slot.kind;
	return Erc1967Slot::None;
}

Erc1967Slot Erc1967Lowering::classifyValue(awst::Expression const* _expr)
{
	if (auto slot = classify(_expr); slot != Erc1967Slot::None) return slot;
	if (auto const* bytes = dynamic_cast<awst::BytesConstant const*>(_expr);
		bytes && bytes->value.size() == 32)
	{
		auto word = solidity::fromBigEndian<solidity::u256>(bytes->value);
		for (auto const& slot: slots)
			if (word == slot.word) return slot.kind;
	}
	return Erc1967Slot::None;
}

char const* Erc1967Lowering::slotName(Erc1967Slot _slot)
{
	for (auto const& slot: slots)
		if (slot.kind == _slot) return slot.name;
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
		+ " slot constant survives in runtime data. This conservative warning "
		"does not prove a storage-model split: returning a proxiable UUID is valid. "
		"If this value reaches a derived storage access, that access is not "
		"adapted to the native proxy model. Use direct constant sload/sstore or "
		"explicitly annotated supported dependencies (see proxy.md).",
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
	Erc1967Slot _slot, awst::SourceLocation const& _loc)
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
	std::string const& _cref, std::shared_ptr<awst::Expression> _logicalSender,
	awst::SourceLocation const& _loc)
{
	auto method = awst::ContractMethod(
		_cref, "__erc1967_update", awst::WType::voidType(), {}, _loc);

	auto body = method.body;
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
	// Account-form admin uses the same identity as source msg.sender. Small
	// zero-padded words belong to the distinct native application-id namespace.
	body->body.push_back(awst::makeAssignmentStatement(
		okVar(),
		awst::makeBoolBinOp(
			awst::makeNumericCompare(adminVar(), awst::NumericComparison::Gt,
				awst::makeBiguintConstant("18446744073709551615", _loc), _loc),
			awst::BinaryBooleanOperator::And,
			awst::makeNumericCompare(adminVar(), awst::NumericComparison::Eq,
				awst::makeAsBiguint(awst::makeAsBytes(std::move(_logicalSender), _loc), _loc), _loc), _loc),
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

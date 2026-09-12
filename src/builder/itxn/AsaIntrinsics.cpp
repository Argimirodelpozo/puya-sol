/// @file AsaIntrinsics.cpp
/// AVM stdlib library intercept: lowers the ordinary Solidity declarations in
/// `libs/AVM.sol` to AVM-native AWST.

#include "builder/itxn/AsaIntrinsics.h"
#include "awst/NameGen.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-ast/CallOperands.h"
#include "builder/ProgramAnalysis.h"
#include "Logger.h"
// Uses solc AST/Type definitions directly; the hub headers only
// forward-declare them now.
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

#include <map>

namespace puyasol::builder::eb
{

using namespace solidity::frontend;

namespace
{

/// `global CurrentApplicationAddress` as account-typed expr.
std::shared_ptr<awst::Expression> currentAppAddress(awst::SourceLocation const& _loc)
{
	auto addr = awst::makeGlobal(std::string("CurrentApplicationAddress"), awst::WType::accountType(), _loc);
	return addr;
}

/// Coerce string→bytes (strings are bytes at the AVM level).
std::shared_ptr<awst::Expression> stringToBytes(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc)
{
	if (_expr->wtype == awst::WType::bytesType())
		return _expr;
	return awst::makeAsBytes(std::move(_expr), _loc);
}

/// Extract field 0 from an asset_holding_get / asset_params_get tuple.
std::shared_ptr<awst::Expression> tupleFirst(
	std::shared_ptr<awst::Expression> _tuple,
	awst::WType const* _firstType,
	awst::SourceLocation const& _loc)
{
	auto out = awst::makeTupleItem(std::move(_tuple), 0, _firstType, _loc);
	return out;
}

/// `asset_params_get <field>`, returning field 0 only.
/// _firstType: uint64 for numeric fields, bytes for string fields.
std::shared_ptr<awst::Expression> assetParamFirst(
	ContractContext& _ctx,
	std::string _field,
	std::shared_ptr<awst::Expression> _assetId,
	awst::WType const* _firstType,
	awst::SourceLocation const& _loc)
{
	auto* tupleType = _ctx.typeMapper.createType<awst::WTuple>(
		std::vector<awst::WType const*>{_firstType, awst::WType::boolType()});
	auto paramsGet = awst::makeAssetParamsGet(
		std::move(_field), std::move(_assetId), tupleType, _loc);
	return tupleFirst(std::move(paramsGet), _firstType, _loc);
}

/// Arity gate shared by every intrinsic: logs `_message` and answers false
/// (the caller returns nullptr) unless exactly `_n` args were supplied.
bool expectArgs(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	size_t _n,
	std::string const& _message,
	awst::SourceLocation const& _loc)
{
	if (_args.size() == _n)
		return true;
	Logger::instance().error(_message, _loc);
	return false;
}

/// `<Lib>.<method> expects N arg[s]`, plus ` (<argNames>)` when given.
std::string arityMessage(
	std::string const& _lib, std::string const& _method, size_t _n, char const* _argNames)
{
	std::string message = _lib + "." + _method + " expects " + std::to_string(_n)
		+ (_n == 1 ? " arg" : " args");
	if (_argNames)
		message += std::string(" (") + _argNames + ")";
	return message;
}

/// `<method>` → a named txn/global field of the given wtype.
struct FieldRow
{
	char const* method;
	char const* field;
	awst::WType const* (*wtype)();
};

template <size_t N>
FieldRow const* findField(FieldRow const (&_rows)[N], std::string const& _method)
{
	for (auto const& row: _rows)
		if (_method == row.method)
			return &row;
	return nullptr;
}

/// One WInnerTransactionFields / WInnerTransaction instance per txn type,
/// shared by every ASA handler (serialized by name, never by identity).
awst::WType const* itxnFieldsType(int _txnType)
{
	static std::map<int, awst::WInnerTransactionFields> s_types;
	return &s_types.try_emplace(_txnType, _txnType).first->second;
}

awst::WType const* itxnType(int _txnType)
{
	static std::map<int, awst::WInnerTransaction> s_types;
	return &s_types.try_emplace(_txnType, _txnType).first->second;
}

/// Build one `_txnType` inner transaction (TypeEnum + zero Fee preset, `_fill`
/// sets the rest) and submit it as a pre-effect.
template <typename Fill>
void submitItxn(
	ContractContext& _ctx, int _txnType, awst::SourceLocation const& _loc, Fill&& _fill)
{
	auto create = awst::makeCreateInnerTransaction(itxnFieldsType(_txnType), _loc);
	create->fields["TypeEnum"] = awst::makeIntegerConstant(std::to_string(_txnType), _loc);
	create->fields["Fee"] = awst::makeZero(_loc);
	_fill(*create);
	auto submit = awst::makeSubmitInnerTransaction(itxnType(_txnType), _loc);
	submit->itxns.push_back(std::move(create));
	_ctx.preEffects().push_back(awst::makeExpressionStatement(std::move(submit), _loc));
}

} // namespace

std::string AsaIntrinsics::facadeLibrary(FunctionDefinition const& function)
{
	auto const* owner = function.annotation().contract;
	if (!owner || !owner->isLibrary() || function.sourceUnitName() != "libs/AVM.sol"
		|| function.visibility() != Visibility::Internal)
		return {};
	// These are target capabilities, not an alternative Solidity type parser.
	// solc supplies the signature, return types and mutability of the exact
	// declaration; unrelated libraries and unsupported overloads stay ordinary.
	static std::map<std::string, std::set<std::string>> const signatures{
		{"AVM", {
			"asaCreate(uint64,uint8,string,string):uint64:nonpayable",
			"asaCreate(uint64,uint8,string,string,bool):uint64:nonpayable",
			"asaDestroy(uint64)::nonpayable", "asaOptIn(uint64)::nonpayable",
			"asaFreeze(uint64,address,bool)::nonpayable",
			"asaTransfer(uint64,address,address,uint256)::nonpayable",
			"asaBalance(address,uint64):uint256:view", "asaTotalSupply(uint64):uint256:view",
			"asaDecimals(uint64):uint8:view", "asaUnitName(uint64):string:view", "asaName(uint64):string:view"}},
		{"Crypto", {"sha512_256(bytes):bytes32:pure", "sha3_256(bytes):bytes32:pure",
			"ed25519Verify(bytes,bytes,bytes):bool:pure", "falconVerify(bytes,bytes,bytes):bool:pure",
			"vrfVerify(bytes,bytes,bytes):bytes,bool:pure"}},
		{"Group", {"size():uint64:view", "index():uint64:view",
			"txnSender(uint64):address:view", "txnReceiver(uint64):address:view",
			"txnAmount(uint64):uint64:view", "txnAssetReceiver(uint64):address:view",
			"txnAssetAmount(uint64):uint64:view", "txnAssetId(uint64):uint64:view",
			"txnApplicationId(uint64):uint64:view", "txnFee(uint64):uint64:view", "txnType(uint64):uint64:view"}},
		{"Txn", {"sender():address:view", "fee():uint64:view", "firstValid():uint64:view",
			"lastValid():uint64:view", "note():bytes:view", "lease():bytes32:view",
			"typeEnum():uint64:view", "groupIndex():uint64:view", "txnId():bytes32:view",
			"rekeyTo():address:view", "applicationId():uint64:view", "onCompletion():uint64:view",
			"numAppArgs():uint64:view", "appArg(uint64):bytes:view"}},
		{"Global", {"currentApplicationId():uint64:view", "currentApplicationAddress():address:view",
			"creatorAddress():address:view", "groupId():bytes32:view", "latestTimestamp():uint64:view",
			"round():uint64:view", "opcodeBudget():uint64:view", "callerApplicationId():uint64:view",
			"minBalance(address):uint64:view", "balance(address):uint64:view"}},
		{"Bits", {"bitlen(uint256):uint256:pure"}},
		{"Scratch", {"store(uint64,uint64)::nonpayable", "loadSelf(uint64):uint64:view",
			"load(uint64,uint64):uint64:view", "storeBytes(uint64,bytes)::nonpayable",
			"loadBytesSelf(uint64):bytes:view", "loadBytes(uint64,uint64):bytes:view"}},
	};
	auto found = signatures.find(owner->name());
	if (found == signatures.end()) return {};
	auto const* type = function.functionType(true);
	if (!type || !type->interfaceFunctionType()) return {};
	std::string signature = type->externalSignature() + ":";
	for (auto const* result: type->returnParameterTypes())
	{
		auto const* external = result->interfaceType(false).get();
		if (!external) return {};
		if (signature.back() != ':') signature += ',';
		signature += external->signatureInExternalFunction(false);
	}
	signature += ":" + stateMutabilityToString(function.stateMutability());
	return found->second.count(signature) ? owner->name() : std::string{};
}

std::optional<std::shared_ptr<awst::Expression>> AsaIntrinsics::tryHandleCall(
	ContractContext& _ctx,
	MemberAccess const& _memberAccess,
	FunctionCall const& _call,
	awst::SourceLocation const& _loc)
{
	auto const* function = dynamic_cast<FunctionDefinition const*>(
		_memberAccess.annotation().referencedDeclaration);
	auto const& known = _ctx.typeMapper.analysis().avmIntrinsics;
	auto found = function ? known.find(function->id()) : known.end();
	if (found == known.end())
		return std::nullopt;
	auto const& lib = found->second;
	auto const& method = function->name();
	auto const* type = dynamic_cast<FunctionType const*>(_call.expression().annotation().type);
	bool bound = type && type->hasBoundFirstArgument();
	std::shared_ptr<awst::Expression> receiver;
	auto bindReceiver = [&] {
		receiver = sol_ast::CallOperands::evaluate(_ctx, _memberAccess.expression(), _loc);
	};
	if (bound && _ctx.viaIRSequencing) bindReceiver();
	auto args = sol_ast::CallOperands::build(_ctx, _call, _loc);
	if (bound)
	{
		if (!_ctx.viaIRSequencing) bindReceiver();
		args.insert(args.begin(), std::move(receiver));
	}

	if (lib == "AVM")
	{
		if (method == "asaCreate") return handleAsaCreate(_ctx, args, _loc);
		if (method == "asaDestroy") return handleAsaDestroy(_ctx, args, _loc);
		if (method == "asaOptIn") return handleAsaOptIn(_ctx, args, _loc);
		if (method == "asaFreeze") return handleAsaFreeze(_ctx, args, _loc);
		if (method == "asaBalance") return handleAsaBalance(_ctx, args, _loc);
		if (method == "asaTransfer") return handleAsaTransfer(_ctx, args, _loc);
		if (auto param = dispatchAsaParam(_ctx, method, args, _loc))
			return param;
	}
	else if (lib == "Crypto")
		return dispatchCrypto(_ctx, method, args, _loc);
	else if (lib == "Group")
		return dispatchGroup(_ctx, method, args, _loc);
	else if (lib == "Txn")
		return dispatchTxn(_ctx, method, args, _loc);
	else if (lib == "Global")
		return dispatchGlobal(_ctx, method, args, _loc);
	else if (lib == "Bits")
		return dispatchBits(_ctx, method, args, _loc);
	else if (lib == "Scratch")
		return dispatchScratch(_ctx, method, args, _loc);

	Logger::instance().warning(
		"unknown AVM stdlib intrinsic '" + lib + "." + method + "'", _loc);
	return std::nullopt;
}

std::optional<std::shared_ptr<awst::Expression>> AsaIntrinsics::dispatchBits(
	ContractContext& _ctx,
	std::string const& _method,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	(void)_ctx;
	if (_method != "bitlen")
		return std::nullopt;
	if (!expectArgs(_args, 1, "Bits.bitlen expects 1 arg", _loc))
		return nullptr;
	auto bitlen = awst::makeIntrinsicCall(
		"bitlen", awst::WType::uint64Type(), _loc);
	bitlen->stackArgs.push_back(std::move(_args.front()));
	return TypeCoercion::implicitNumericCast(std::move(bitlen), awst::WType::biguintType(), _loc);
}

// AVM scratch (AVM.sol Scratch): store→stores, loadSelf→loads, load→gloadss.
// gloadss requires gidx < GroupIndex (AVM assertion); uint64-valued. The bytes
// variants reuse the same ops (stores/loads/gloadss accept `any`), coercing
// only the slot / group index to uint64.
std::optional<std::shared_ptr<awst::Expression>> AsaIntrinsics::dispatchScratch(
	ContractContext& _ctx,
	std::string const& _method,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	(void)_ctx;
	struct Op
	{
		char const* method;
		char const* op;
		awst::WType const* (*result)();
		size_t argc;
		char const* argNames;
		bool rawValue;   // last arg is the stored bytes: no uint64 coercion
	};
	static Op const s_ops[] = {
		{"store", "stores", &awst::WType::voidType, 2, "slot, value", false},
		{"loadSelf", "loads", &awst::WType::uint64Type, 1, "slot", false},
		{"load", "gloadss", &awst::WType::uint64Type, 2, "groupIndex, slot", false},
		{"storeBytes", "stores", &awst::WType::voidType, 2, "slot, value", true},
		{"loadBytesSelf", "loads", &awst::WType::bytesType, 1, "slot", false},
		{"loadBytes", "gloadss", &awst::WType::bytesType, 2, "groupIndex, slot", false},
	};
	for (auto const& op: s_ops)
	{
		if (_method != op.method)
			continue;
		if (!expectArgs(_args, op.argc, arityMessage("Scratch", _method, op.argc, op.argNames), _loc))
			return nullptr;
		auto ic = awst::makeIntrinsicCall(op.op, op.result(), _loc);
		for (size_t i = 0; i < op.argc; ++i)
		{
			auto arg = std::move(_args[i]);
			if (!(op.rawValue && i + 1 == op.argc))
				arg = TypeCoercion::implicitNumericCast(std::move(arg), awst::WType::uint64Type(), _loc);
			ic->stackArgs.push_back(std::move(arg));
		}
		return std::shared_ptr<awst::Expression>(std::move(ic));
	}

	Logger::instance().warning("unknown Scratch." + _method, _loc);
	return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> AsaIntrinsics::handleAsaCreate(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (_args.size() != 4 && _args.size() != 5)
	{
		Logger::instance().error(
			"AVM.asaCreate expects 4 or 5 args (total, decimals, name, symbol[, defaultFrozen])", _loc);
		return nullptr;
	}

	auto total = std::move(_args[0]);
	auto decimals = std::move(_args[1]);
	auto name = stringToBytes(std::move(_args[2]), _loc);
	auto symbol = stringToBytes(std::move(_args[3]), _loc);
	// Optional 5th arg: default_frozen (bool). Omitted = unfrozen (4-arg AERC20 path unchanged).
	std::shared_ptr<awst::Expression> defaultFrozen;
	if (_args.size() == 5)
		defaultFrozen = std::move(_args[4]);

	submitItxn(_ctx, 3, _loc, [&](awst::CreateInnerTransaction& create) {
		create.fields["ConfigAssetTotal"] = std::move(total);
		create.fields["ConfigAssetDecimals"] = std::move(decimals);
		create.fields["ConfigAssetUnitName"] = std::move(symbol);
		create.fields["ConfigAssetName"] = std::move(name);
		create.fields["ConfigAssetManager"] = currentAppAddress(_loc);
		create.fields["ConfigAssetReserve"] = currentAppAddress(_loc);
		create.fields["ConfigAssetClawback"] = currentAppAddress(_loc);
		create.fields["ConfigAssetFreeze"] = currentAppAddress(_loc);
		if (defaultFrozen)
			create.fields["ConfigAssetDefaultFrozen"] = std::move(defaultFrozen);
	});

	// Stash CreatedAssetID in a temp — subsequent itxn submissions clobber itxn fields.
	auto createdAsaCall = awst::makeItxn(
		"CreatedAssetID", awst::WType::uint64Type(), _loc);

	std::string tmpName = "__new_asa_id_" + std::to_string(awst::NameGen::next("AsaIntrinsics.s_counter"));
	auto tmpTarget = awst::makeVarExpression(tmpName, awst::WType::uint64Type(), _loc);
	auto assign = awst::makeAssignmentStatement(tmpTarget, std::move(createdAsaCall), _loc);
	_ctx.preEffects().push_back(std::move(assign));

	return awst::makeVarExpression(tmpName, awst::WType::uint64Type(), _loc);
}

std::shared_ptr<awst::Expression> AsaIntrinsics::handleAsaBalance(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (!expectArgs(_args, 2, "AVM.asaBalance expects 2 args (holder, assetId)", _loc))
		return nullptr;

	auto holder = std::move(_args[0]);
	auto assetId = std::move(_args[1]);

	auto* tupleType = _ctx.typeMapper.createType<awst::WTuple>(
		std::vector<awst::WType const*>{
			awst::WType::uint64Type(), awst::WType::boolType()});
	auto holdingGet = awst::makeIntrinsicCall("asset_holding_get", tupleType, _loc);
	holdingGet->immediates = {std::string("AssetBalance")};
	holdingGet->stackArgs.push_back(std::move(holder));
	holdingGet->stackArgs.push_back(std::move(assetId));

	auto balanceU64 = tupleFirst(std::move(holdingGet), awst::WType::uint64Type(), _loc);
	return TypeCoercion::implicitNumericCast(std::move(balanceU64), awst::WType::biguintType(), _loc);
}

// asset_params_get readers: which field, and how its first tuple item
// (uint64 for numeric fields, bytes for names) surfaces to Solidity —
// AssetDecimals fits uint8, so its uint64 stays as is.
std::optional<std::shared_ptr<awst::Expression>> AsaIntrinsics::dispatchAsaParam(
	ContractContext& _ctx,
	std::string const& _method,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	enum class Surface { BigUInt, UInt64, String };
	struct Param
	{
		char const* method;
		char const* field;
		Surface surface;
	};
	static Param const s_params[] = {
		{"asaTotalSupply", "AssetTotal", Surface::BigUInt},
		{"asaDecimals", "AssetDecimals", Surface::UInt64},
		{"asaUnitName", "AssetUnitName", Surface::String},
		{"asaName", "AssetName", Surface::String},
	};
	for (auto const& param: s_params)
	{
		if (_method != param.method)
			continue;
		if (!expectArgs(_args, 1, arityMessage("AVM", _method, 1, "assetId"), _loc))
			return nullptr;
		bool const isString = param.surface == Surface::String;
		auto value = assetParamFirst(
			_ctx, param.field, std::move(_args[0]),
			isString ? awst::WType::bytesType() : awst::WType::uint64Type(), _loc);
		switch (param.surface)
		{
		case Surface::BigUInt:
			return TypeCoercion::implicitNumericCast(std::move(value), awst::WType::biguintType(), _loc);
		case Surface::UInt64:
			return value;
		case Surface::String:
			return std::shared_ptr<awst::Expression>(
				awst::makeReinterpretCast(std::move(value), awst::WType::stringType(), _loc));
		}
	}
	return std::nullopt;
}

std::shared_ptr<awst::Expression> AsaIntrinsics::handleAsaTransfer(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (!expectArgs(_args, 4, "AVM.asaTransfer expects 4 args (assetId, from, to, amount)", _loc))
		return nullptr;

	auto assetId = std::move(_args[0]);
	auto from = std::move(_args[1]);
	auto to = std::move(_args[2]);
	// AVM.asaTransfer declares `uint256 amount`; assert it fits in the
	// uint64 AssetAmount field instead of silently sending `amount mod 2^64`.
	auto amount = builder::TypeCoercion::checkedAmountToUint64(
		_ctx.preEffects(), std::move(_args[3]), _loc);

	submitItxn(_ctx, 4, _loc, [&](awst::CreateInnerTransaction& create) {
		create.fields["XferAsset"] = std::move(assetId);
		create.fields["AssetSender"] = std::move(from);
		create.fields["AssetReceiver"] = std::move(to);
		create.fields["AssetAmount"] = std::move(amount);
	});

	auto vc = awst::makeVoidConstant(_loc);
	return vc;
}


// ═══════════════════════════════════════════════════════════════════════
// ASA: opt-in, destroy, freeze
// ═══════════════════════════════════════════════════════════════════════

std::shared_ptr<awst::Expression> AsaIntrinsics::handleAsaOptIn(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (!expectArgs(_args, 1, "AVM.asaOptIn expects 1 arg (assetId)", _loc))
		return nullptr;
	auto assetId = std::move(_args[0]);

	// axfer 0 units to self = standard ASA opt-in.
	submitItxn(_ctx, 4, _loc, [&](awst::CreateInnerTransaction& create) {
		create.fields["XferAsset"] = std::move(assetId);
		create.fields["AssetReceiver"] = currentAppAddress(_loc);
		create.fields["AssetAmount"] = awst::makeZero(_loc);
	});
	return awst::makeVoidConstant(_loc);
}

std::shared_ptr<awst::Expression> AsaIntrinsics::handleAsaDestroy(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (!expectArgs(_args, 1, "AVM.asaDestroy expects 1 arg (assetId)", _loc))
		return nullptr;
	auto assetId = std::move(_args[0]);

	// acfg with ConfigAsset set and no other config fields = destroy.
	submitItxn(_ctx, 3, _loc, [&](awst::CreateInnerTransaction& create) {
		create.fields["ConfigAsset"] = std::move(assetId);
	});
	return awst::makeVoidConstant(_loc);
}

std::shared_ptr<awst::Expression> AsaIntrinsics::handleAsaFreeze(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (!expectArgs(_args, 3, "AVM.asaFreeze expects 3 args (assetId, holder, frozen)", _loc))
		return nullptr;
	auto assetId = std::move(_args[0]);
	auto holder = std::move(_args[1]);
	auto frozen = std::move(_args[2]);

	// afrz (TypeEnum = 5)
	submitItxn(_ctx, 5, _loc, [&](awst::CreateInnerTransaction& create) {
		create.fields["FreezeAsset"] = std::move(assetId);
		create.fields["FreezeAssetAccount"] = std::move(holder);
		create.fields["FreezeAssetFrozen"] = std::move(frozen);
	});
	return awst::makeVoidConstant(_loc);
}

// ═══════════════════════════════════════════════════════════════════════
// Crypto / Group / Txn / Global dispatchers
// ═══════════════════════════════════════════════════════════════════════

std::optional<std::shared_ptr<awst::Expression>> AsaIntrinsics::dispatchCrypto(
	ContractContext& _ctx,
	std::string const& _method,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	enum class Result { Bytes, Bool, BytesBoolTuple };
	struct Op
	{
		char const* method;
		char const* op;
		size_t argc;
		Result result;
		char const* immediate;
	};
	static Op const s_ops[] = {
		{"sha512_256", "sha512_256", 1, Result::Bytes, nullptr},
		{"sha3_256", "sha3_256", 1, Result::Bytes, nullptr},
		{"ed25519Verify", "ed25519verify_bare", 3, Result::Bool, nullptr},
		{"falconVerify", "falcon_verify", 3, Result::Bool, nullptr},
		{"vrfVerify", "vrf_verify", 3, Result::BytesBoolTuple, "VrfAlgorand"},
	};
	for (auto const& op: s_ops)
	{
		if (_method != op.method)
			continue;
		if (!expectArgs(_args, op.argc, arityMessage("Crypto", _method, op.argc, nullptr), _loc))
			return nullptr;
		awst::WType const* resultType = nullptr;
		switch (op.result)
		{
		case Result::Bytes: resultType = awst::WType::bytesType(); break;
		case Result::Bool: resultType = awst::WType::boolType(); break;
		case Result::BytesBoolTuple:
			resultType = _ctx.typeMapper.createType<awst::WTuple>(
				std::vector<awst::WType const*>{awst::WType::bytesType(), awst::WType::boolType()});
			break;
		}
		auto call = awst::makeIntrinsicCall(op.op, resultType, _loc);
		if (op.immediate)
			call->immediates = {std::string(op.immediate)};
		for (size_t i = 0; i < op.argc; ++i)
			call->stackArgs.push_back(stringToBytes(std::move(_args[i]), _loc));
		return std::shared_ptr<awst::Expression>(call);
	}
	Logger::instance().warning("unknown Crypto." + _method, _loc);
	return std::nullopt;
}

std::optional<std::shared_ptr<awst::Expression>> AsaIntrinsics::dispatchGroup(
	ContractContext& _ctx,
	std::string const& _method,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	(void)_ctx;
	if (_method == "size")
		return std::shared_ptr<awst::Expression>(awst::makeGlobal(std::string("GroupSize"), awst::WType::uint64Type(), _loc));
	if (_method == "index")
		return std::shared_ptr<awst::Expression>(awst::makeTxn(std::string("GroupIndex"), awst::WType::uint64Type(), _loc));
	static FieldRow const s_gtxns[] = {
		{"txnSender", "Sender", &awst::WType::accountType},
		{"txnReceiver", "Receiver", &awst::WType::accountType},
		{"txnAmount", "Amount", &awst::WType::uint64Type},
		{"txnAssetReceiver", "AssetReceiver", &awst::WType::accountType},
		{"txnAssetAmount", "AssetAmount", &awst::WType::uint64Type},
		{"txnAssetId", "XferAsset", &awst::WType::uint64Type},
		{"txnApplicationId", "ApplicationID", &awst::WType::uint64Type},
		{"txnFee", "Fee", &awst::WType::uint64Type},
		{"txnType", "TypeEnum", &awst::WType::uint64Type},
	};
	if (auto const* row = findField(s_gtxns, _method))
	{
		if (!expectArgs(_args, 1, "Group." + _method + " expects 1 arg (idx)", _loc))
			return nullptr;
		return std::shared_ptr<awst::Expression>(awst::makeGtxns(
			row->field, TypeCoercion::implicitNumericCast(std::move(_args[0]), awst::WType::uint64Type(), _loc), row->wtype(), _loc));
	}

	Logger::instance().warning("unknown Group." + _method, _loc);
	return std::nullopt;
}

std::optional<std::shared_ptr<awst::Expression>> AsaIntrinsics::dispatchTxn(
	ContractContext& _ctx,
	std::string const& _method,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	(void)_ctx;
	static FieldRow const s_txn[] = {
		{"sender", "Sender", &awst::WType::accountType},
		{"fee", "Fee", &awst::WType::uint64Type},
		{"firstValid", "FirstValid", &awst::WType::uint64Type},
		{"lastValid", "LastValid", &awst::WType::uint64Type},
		{"note", "Note", &awst::WType::bytesType},
		{"lease", "Lease", &awst::WType::bytesType},
		{"typeEnum", "TypeEnum", &awst::WType::uint64Type},
		{"groupIndex", "GroupIndex", &awst::WType::uint64Type},
		{"txnId", "TxID", &awst::WType::bytesType},
		{"rekeyTo", "RekeyTo", &awst::WType::accountType},
		{"applicationId", "ApplicationID", &awst::WType::uint64Type},
		{"onCompletion", "OnCompletion", &awst::WType::uint64Type},
		{"numAppArgs", "NumAppArgs", &awst::WType::uint64Type},
	};
	if (auto const* row = findField(s_txn, _method))
	{
		if (!expectArgs(_args, 0, "Txn." + _method + " expects 0 args", _loc))
			return nullptr;
		return std::shared_ptr<awst::Expression>(awst::makeTxn(row->field, row->wtype(), _loc));
	}
	if (_method == "appArg")
	{
		if (!expectArgs(_args, 1, "Txn.appArg expects 1 arg (idx)", _loc))
			return nullptr;
		auto call = awst::makeIntrinsicCall("txnas", awst::WType::bytesType(), _loc);
		call->immediates = {std::string("ApplicationArgs")};
		call->stackArgs.push_back(TypeCoercion::implicitNumericCast(std::move(_args[0]), awst::WType::uint64Type(), _loc));
		return std::shared_ptr<awst::Expression>(call);
	}

	Logger::instance().warning("unknown Txn." + _method, _loc);
	return std::nullopt;
}

std::optional<std::shared_ptr<awst::Expression>> AsaIntrinsics::dispatchGlobal(
	ContractContext& _ctx,
	std::string const& _method,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	(void)_ctx;
	static FieldRow const s_global[] = {
		{"currentApplicationId", "CurrentApplicationID", &awst::WType::uint64Type},
		{"currentApplicationAddress", "CurrentApplicationAddress", &awst::WType::accountType},
		{"creatorAddress", "CreatorAddress", &awst::WType::accountType},
		{"groupId", "GroupID", &awst::WType::bytesType},
		{"latestTimestamp", "LatestTimestamp", &awst::WType::uint64Type},
		{"round", "Round", &awst::WType::uint64Type},
		{"opcodeBudget", "OpcodeBudget", &awst::WType::uint64Type},
		{"callerApplicationId", "CallerApplicationID", &awst::WType::uint64Type},
	};
	if (auto const* row = findField(s_global, _method))
	{
		if (!expectArgs(_args, 0, "Global." + _method + " expects 0 args", _loc))
			return nullptr;
		return std::shared_ptr<awst::Expression>(awst::makeGlobal(row->field, row->wtype(), _loc));
	}
	// Account-keyed uint64 opcodes.
	static std::pair<char const*, char const*> const s_accountOps[] = {
		{"minBalance", "min_balance"}, {"balance", "balance"},
	};
	for (auto const& [method, op]: s_accountOps)
	{
		if (_method != method)
			continue;
		if (!expectArgs(_args, 1, "Global." + _method + " expects 1 arg (account)", _loc))
			return nullptr;
		auto call = awst::makeIntrinsicCall(op, awst::WType::uint64Type(), _loc);
		call->stackArgs.push_back(std::move(_args[0]));
		return std::shared_ptr<awst::Expression>(call);
	}

	Logger::instance().warning("unknown Global." + _method, _loc);
	return std::nullopt;
}

} // namespace puyasol::builder::eb

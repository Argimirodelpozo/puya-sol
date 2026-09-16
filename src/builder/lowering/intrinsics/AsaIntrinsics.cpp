/// @file AsaIntrinsics.cpp
/// AVM stdlib library intercept: lowers the ordinary Solidity declarations in
/// `libs/AVM.sol` to AVM-native AWST.

#include "builder/lowering/intrinsics/AsaIntrinsics.h"
#include "awst/NameGen.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/TypeCoercion.h"
#include "builder/eb/CallOperands.h"
#include "builder/context/ProgramAnalysis.h"
#include "Logger.h"
// Uses solc AST/Type definitions directly; the hub headers only
// forward-declare them now.
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

#include <map>

namespace puyasol::builder::eb
{

using namespace solidity::frontend;

enum class IntrinsicKind { Create, Destroy, OptIn, Freeze, Balance, Transfer,
	AssetParam, Crypto, Scratch, Bitlen, Txn, Global, Gtxn, Opcode };
struct Intrinsic
{
	char const* library;
	char const* signature;
	IntrinsicKind kind;
	char const* opcode = nullptr;
	awst::WType const* (*result)() = nullptr;
	char const* immediate = nullptr;
};

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

// The declaration allowlist and backend operation live together. Solidity
// arity and surface types come from the validated solc declaration, not a
// second hand-maintained dispatch table.
Intrinsic const* intrinsicFor(FunctionDefinition const& function)
{
	auto const* owner = function.annotation().contract;
	if (!owner || !owner->isLibrary() || function.sourceUnitName() != "libs/AVM.sol"
		|| function.visibility() != Visibility::Internal) return nullptr;
	static Intrinsic const intrinsics[] = {
		{"AVM", "asaCreate(uint64,uint8,string,string):uint64:nonpayable", IntrinsicKind::Create},
		{"AVM", "asaCreate(uint64,uint8,string,string,bool):uint64:nonpayable", IntrinsicKind::Create},
		{"AVM", "asaDestroy(uint64)::nonpayable", IntrinsicKind::Destroy},
		{"AVM", "asaOptIn(uint64)::nonpayable", IntrinsicKind::OptIn},
		{"AVM", "asaFreeze(uint64,address,bool)::nonpayable", IntrinsicKind::Freeze},
		{"AVM", "asaTransfer(uint64,address,address,uint256)::nonpayable", IntrinsicKind::Transfer},
		{"AVM", "asaBalance(address,uint64):uint256:view", IntrinsicKind::Balance},
		{"AVM", "asaTotalSupply(uint64):uint256:view", IntrinsicKind::AssetParam, "AssetTotal", &awst::WType::uint64Type},
		{"AVM", "asaDecimals(uint64):uint8:view", IntrinsicKind::AssetParam, "AssetDecimals", &awst::WType::uint64Type},
		{"AVM", "asaUnitName(uint64):string:view", IntrinsicKind::AssetParam, "AssetUnitName", &awst::WType::bytesType},
		{"AVM", "asaName(uint64):string:view", IntrinsicKind::AssetParam, "AssetName", &awst::WType::bytesType},
		{"Crypto", "sha512_256(bytes):bytes32:pure", IntrinsicKind::Crypto, "sha512_256", &awst::WType::bytesType},
		{"Crypto", "sha3_256(bytes):bytes32:pure", IntrinsicKind::Crypto, "sha3_256", &awst::WType::bytesType},
		{"Crypto", "ed25519Verify(bytes,bytes,bytes):bool:pure", IntrinsicKind::Crypto, "ed25519verify_bare", &awst::WType::boolType},
		{"Crypto", "falconVerify(bytes,bytes,bytes):bool:pure", IntrinsicKind::Crypto, "falcon_verify", &awst::WType::boolType},
		{"Crypto", "vrfVerify(bytes,bytes,bytes):bytes,bool:pure", IntrinsicKind::Crypto, "vrf_verify", nullptr, "VrfAlgorand"},
		{"Group", "size():uint64:view", IntrinsicKind::Global, "GroupSize", &awst::WType::uint64Type},
		{"Group", "index():uint64:view", IntrinsicKind::Txn, "GroupIndex", &awst::WType::uint64Type},
		{"Group", "txnSender(uint64):address:view", IntrinsicKind::Gtxn, "Sender", &awst::WType::accountType},
		{"Group", "txnReceiver(uint64):address:view", IntrinsicKind::Gtxn, "Receiver", &awst::WType::accountType},
		{"Group", "txnAmount(uint64):uint64:view", IntrinsicKind::Gtxn, "Amount", &awst::WType::uint64Type},
		{"Group", "txnAssetReceiver(uint64):address:view", IntrinsicKind::Gtxn, "AssetReceiver", &awst::WType::accountType},
		{"Group", "txnAssetAmount(uint64):uint64:view", IntrinsicKind::Gtxn, "AssetAmount", &awst::WType::uint64Type},
		{"Group", "txnAssetId(uint64):uint64:view", IntrinsicKind::Gtxn, "XferAsset", &awst::WType::uint64Type},
		{"Group", "txnApplicationId(uint64):uint64:view", IntrinsicKind::Gtxn, "ApplicationID", &awst::WType::uint64Type},
		{"Group", "txnFee(uint64):uint64:view", IntrinsicKind::Gtxn, "Fee", &awst::WType::uint64Type},
		{"Group", "txnType(uint64):uint64:view", IntrinsicKind::Gtxn, "TypeEnum", &awst::WType::uint64Type},
		{"Txn", "sender():address:view", IntrinsicKind::Txn, "Sender", &awst::WType::accountType},
		{"Txn", "fee():uint64:view", IntrinsicKind::Txn, "Fee", &awst::WType::uint64Type},
		{"Txn", "firstValid():uint64:view", IntrinsicKind::Txn, "FirstValid", &awst::WType::uint64Type},
		{"Txn", "lastValid():uint64:view", IntrinsicKind::Txn, "LastValid", &awst::WType::uint64Type},
		{"Txn", "note():bytes:view", IntrinsicKind::Txn, "Note", &awst::WType::bytesType},
		{"Txn", "lease():bytes32:view", IntrinsicKind::Txn, "Lease", &awst::WType::bytesType},
		{"Txn", "typeEnum():uint64:view", IntrinsicKind::Txn, "TypeEnum", &awst::WType::uint64Type},
		{"Txn", "groupIndex():uint64:view", IntrinsicKind::Txn, "GroupIndex", &awst::WType::uint64Type},
		{"Txn", "txnId():bytes32:view", IntrinsicKind::Txn, "TxID", &awst::WType::bytesType},
		{"Txn", "rekeyTo():address:view", IntrinsicKind::Txn, "RekeyTo", &awst::WType::accountType},
		{"Txn", "applicationId():uint64:view", IntrinsicKind::Txn, "ApplicationID", &awst::WType::uint64Type},
		{"Txn", "onCompletion():uint64:view", IntrinsicKind::Txn, "OnCompletion", &awst::WType::uint64Type},
		{"Txn", "numAppArgs():uint64:view", IntrinsicKind::Txn, "NumAppArgs", &awst::WType::uint64Type},
		{"Txn", "appArg(uint64):bytes:view", IntrinsicKind::Opcode, "txnas", &awst::WType::bytesType, "ApplicationArgs"},
		{"Global", "currentApplicationId():uint64:view", IntrinsicKind::Global, "CurrentApplicationID", &awst::WType::uint64Type},
		{"Global", "currentApplicationAddress():address:view", IntrinsicKind::Global, "CurrentApplicationAddress", &awst::WType::accountType},
		{"Global", "creatorAddress():address:view", IntrinsicKind::Global, "CreatorAddress", &awst::WType::accountType},
		{"Global", "groupId():bytes32:view", IntrinsicKind::Global, "GroupID", &awst::WType::bytesType},
		{"Global", "latestTimestamp():uint64:view", IntrinsicKind::Global, "LatestTimestamp", &awst::WType::uint64Type},
		{"Global", "round():uint64:view", IntrinsicKind::Global, "Round", &awst::WType::uint64Type},
		{"Global", "opcodeBudget():uint64:view", IntrinsicKind::Global, "OpcodeBudget", &awst::WType::uint64Type},
		{"Global", "callerApplicationId():uint64:view", IntrinsicKind::Global, "CallerApplicationID", &awst::WType::uint64Type},
		{"Global", "minBalance(address):uint64:view", IntrinsicKind::Opcode, "min_balance", &awst::WType::uint64Type},
		{"Global", "balance(address):uint64:view", IntrinsicKind::Opcode, "balance", &awst::WType::uint64Type},
		{"Bits", "bitlen(uint256):uint256:pure", IntrinsicKind::Bitlen, "bitlen", &awst::WType::uint64Type},
		{"Scratch", "store(uint64,uint64)::nonpayable", IntrinsicKind::Scratch, "stores", &awst::WType::voidType},
		{"Scratch", "loadSelf(uint64):uint64:view", IntrinsicKind::Scratch, "loads", &awst::WType::uint64Type},
		{"Scratch", "load(uint64,uint64):uint64:view", IntrinsicKind::Scratch, "gloadss", &awst::WType::uint64Type},
		{"Scratch", "storeBytes(uint64,bytes)::nonpayable", IntrinsicKind::Scratch, "stores", &awst::WType::voidType},
		{"Scratch", "loadBytesSelf(uint64):bytes:view", IntrinsicKind::Scratch, "loads", &awst::WType::bytesType},
		{"Scratch", "loadBytes(uint64,uint64):bytes:view", IntrinsicKind::Scratch, "gloadss", &awst::WType::bytesType},
	};
	auto const* type = function.functionType(true);
	if (!type || !type->interfaceFunctionType()) return nullptr;
	std::string signature = type->externalSignature() + ":";
	for (auto const* result: type->returnParameterTypes())
	{
		auto const* external = result->interfaceType(false).get();
		if (!external) return nullptr;
		if (signature.back() != ':') signature += ',';
		signature += external->signatureInExternalFunction(false);
	}
	signature += ":" + stateMutabilityToString(function.stateMutability());
	for (auto const& intrinsic: intrinsics)
		if (owner->name() == intrinsic.library && signature == intrinsic.signature) return &intrinsic;
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

Intrinsic const* AsaIntrinsics::descriptor(FunctionDefinition const& function)
{
	return intrinsicFor(function);
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
	auto const* intrinsic = found->second;
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

	assert(args.size() == function->parameters().size());
	using K = IntrinsicKind;
	switch (intrinsic->kind)
	{
	case K::Create: return handleAsaCreate(_ctx, args, _loc);
	case K::Destroy: return handleAsaDestroy(_ctx, args, _loc);
	case K::OptIn: return handleAsaOptIn(_ctx, args, _loc);
	case K::Freeze: return handleAsaFreeze(_ctx, args, _loc);
	case K::Balance: return handleAsaBalance(_ctx, args, _loc);
	case K::Transfer: return handleAsaTransfer(_ctx, args, _loc);
	case K::Txn: return std::shared_ptr<awst::Expression>(awst::makeTxn(intrinsic->opcode, intrinsic->result(), _loc));
	case K::Global: return std::shared_ptr<awst::Expression>(awst::makeGlobal(intrinsic->opcode, intrinsic->result(), _loc));
	case K::Gtxn:
		return std::shared_ptr<awst::Expression>(awst::makeGtxns(intrinsic->opcode,
			TypeCoercion::coerceScalar(std::move(args[0]), awst::WType::uint64Type(), _loc),
			intrinsic->result(), _loc));
	case K::AssetParam:
	{
		auto value = assetParamFirst(_ctx, intrinsic->opcode, std::move(args[0]), intrinsic->result(), _loc);
		auto const* target = _ctx.typeMapper.map(function->returnParameters().front()->type());
		if (target == awst::WType::stringType())
			return std::shared_ptr<awst::Expression>(awst::makeReinterpretCast(std::move(value), target, _loc));
		return TypeCoercion::coerceScalar(std::move(value), target, _loc);
	}
	default: break;
	}
	auto const* resultType = intrinsic->result ? intrinsic->result()
		: _ctx.typeMapper.createType<awst::WTuple>(
			std::vector<awst::WType const*>{awst::WType::bytesType(), awst::WType::boolType()});
	auto call = awst::makeIntrinsicCall(intrinsic->opcode, resultType, _loc);
	if (intrinsic->immediate) call->immediates = {std::string(intrinsic->immediate)};
	for (size_t i = 0; i < args.size(); ++i)
	{
		auto value = std::move(args[i]);
		if (intrinsic->kind == K::Crypto) value = stringToBytes(std::move(value), _loc);
		else if (intrinsic->kind == K::Scratch
			&& function->parameters()[i]->type()->isValueType())
			value = TypeCoercion::coerceScalar(std::move(value), awst::WType::uint64Type(), _loc);
		call->stackArgs.push_back(std::move(value));
	}
	if (intrinsic->kind == K::Bitlen)
		return TypeCoercion::coerceScalar(std::move(call), awst::WType::biguintType(), _loc);
	return std::shared_ptr<awst::Expression>(std::move(call));
}

// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> AsaIntrinsics::handleAsaCreate(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{

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
	return TypeCoercion::coerceScalar(std::move(balanceU64), awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> AsaIntrinsics::handleAsaTransfer(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{

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

} // namespace puyasol::builder::eb

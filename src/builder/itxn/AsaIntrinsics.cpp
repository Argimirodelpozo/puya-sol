/// @file AsaIntrinsics.cpp
/// AVM stdlib library intercept: lowers the ordinary Solidity declarations in
/// `libs/AVM.sol` to AVM-native AWST.

#include "builder/itxn/AsaIntrinsics.h"
#include "awst/NameGen.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"
// Uses solc AST/Type definitions directly; the hub headers only
// forward-declare them now.
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

using namespace solidity::frontend;

namespace
{

/// Return library name (AVM/Crypto/Group/Txn/Global/Scratch) if _memberAccess
/// resolves to an AVM stdlib library, else "". Works for both direct and
/// module-aliased (`import ... as Mod`) references via referencedDeclaration.
std::string getAvmStdlibLibraryName(MemberAccess const& _memberAccess)
{
	auto const* contractDef = dynamic_cast<ContractDefinition const*>(
		ASTNode::referencedDeclaration(_memberAccess.expression()));
	if (!contractDef || !contractDef->isLibrary())
		return "";
	std::string const& name = contractDef->name();
	if (name == "AVM" || name == "Crypto" || name == "Group"
		|| name == "Txn" || name == "Global" || name == "Scratch")
		return name;
	return "";
}

/// Promote a uint64-typed value to biguint via itob + reinterpret.
std::shared_ptr<awst::Expression> uint64ToBigUInt(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc)
{
	auto itob = awst::makeItob(std::move(_expr), _loc);
	return awst::makeAsBiguint(std::move(itob), _loc);
}

/// Truncate biguint to uint64; pass through if already uint64.
/// AVM big-int ops strip leading zeros (minimal encoding), so we left-pad
/// to 8 bytes before extracting — avoids "extraction start beyond length".
std::shared_ptr<awst::Expression> bigUIntToUint64(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc)
{
	if (_expr->wtype == awst::WType::uint64Type())
		return _expr;

	// Left-pad to 8 bytes (bitwise, not b+), take last 8, btoi.
	// `b+` strips to minimal encoding so extract3(24,8) overran for short values.
	auto asBytes = awst::makeAsBytes(std::move(_expr), _loc);
	auto low8 = awst::makeExtractLastN(
		awst::makeLeftPad(std::move(asBytes), 8, _loc), 8, _loc);
	return awst::makeBtoi(std::move(low8), _loc);
}

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

} // namespace

bool AsaIntrinsics::isBitsBitlenFacade(FunctionDefinition const& _function)
{
	auto const* owner = _function.annotation().contract;
	if (!owner || !owner->isLibrary() || owner->name() != "Bits"
		|| _function.sourceUnitName() != "libs/AVM.sol"
		|| _function.name() != "bitlen"
		|| _function.visibility() != Visibility::Internal
		|| _function.stateMutability() != StateMutability::Pure
		|| _function.parameters().size() != 1
		|| _function.returnParameters().size() != 1)
		return false;
	auto isUint256 = [](VariableDeclaration const& _parameter) {
		auto const* integer = dynamic_cast<IntegerType const*>(_parameter.type());
		return integer && !integer->isSigned() && integer->numBits() == 256;
	};
	return isUint256(*_function.parameters().front())
		&& isUint256(*_function.returnParameters().front());
}

std::optional<std::shared_ptr<awst::Expression>> AsaIntrinsics::tryHandleCall(
	ContractContext& _ctx,
	MemberAccess const& _memberAccess,
	FunctionCall const& _call,
	awst::SourceLocation const& _loc)
{
	auto const* function = dynamic_cast<FunctionDefinition const*>(
		_memberAccess.annotation().referencedDeclaration);
	bool const isBitsBitlen = function && isBitsBitlenFacade(*function);
	std::string lib = isBitsBitlen
		? std::string("Bits")
		: getAvmStdlibLibraryName(_memberAccess);
	if (lib.empty())
		return std::nullopt;

	std::string method = _memberAccess.memberName();

	std::vector<std::shared_ptr<awst::Expression>> args;
	for (auto const& arg: _call.arguments())
		args.push_back(_ctx.buildExpr(*arg));
	// `using Bits for uint256; value.bitlen()` supplies the attached value as
	// the member-access base rather than as an explicit FunctionCall argument.
	if (isBitsBitlen && args.empty())
		args.push_back(_ctx.buildExpr(_memberAccess.expression()));

	if (lib == "AVM")
	{
		if (method == "asaCreate") return handleAsaCreate(_ctx, args, _loc);
		if (method == "asaDestroy") return handleAsaDestroy(_ctx, args, _loc);
		if (method == "asaOptIn") return handleAsaOptIn(_ctx, args, _loc);
		if (method == "asaFreeze") return handleAsaFreeze(_ctx, args, _loc);
		if (method == "asaBalance") return handleAsaBalance(_ctx, args, _loc);
		if (method == "asaTotalSupply") return handleAsaTotalSupply(_ctx, args, _loc);
		if (method == "asaDecimals") return handleAsaDecimals(_ctx, args, _loc);
		if (method == "asaUnitName") return handleAsaUnitName(_ctx, args, _loc);
		if (method == "asaName") return handleAsaName(_ctx, args, _loc);
		if (method == "asaTransfer") return handleAsaTransfer(_ctx, args, _loc);
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
	if (_args.size() != 1)
	{
		Logger::instance().error("Bits.bitlen expects 1 arg", _loc);
		return nullptr;
	}
	auto bitlen = awst::makeIntrinsicCall(
		"bitlen", awst::WType::uint64Type(), _loc);
	bitlen->stackArgs.push_back(std::move(_args.front()));
	return uint64ToBigUInt(std::move(bitlen), _loc);
}

// AVM scratch (AVM.sol Scratch): store→stores, loadSelf→loads, load→gloadss.
// gloadss requires gidx < GroupIndex (AVM assertion); uint64-valued.
std::optional<std::shared_ptr<awst::Expression>> AsaIntrinsics::dispatchScratch(
	ContractContext& _ctx,
	std::string const& _method,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	(void)_ctx;
	if (_method == "store")
	{
		if (_args.size() != 2)
		{
			Logger::instance().error("Scratch.store expects 2 args (slot, value)", _loc);
			return nullptr;
		}
		auto ic = awst::makeIntrinsicCall("stores", awst::WType::voidType(), _loc);
		ic->stackArgs.push_back(bigUIntToUint64(std::move(_args[0]), _loc)); // slot
		ic->stackArgs.push_back(bigUIntToUint64(std::move(_args[1]), _loc)); // value
		return std::shared_ptr<awst::Expression>(std::move(ic));
	}
	if (_method == "loadSelf")
	{
		if (_args.size() != 1)
		{
			Logger::instance().error("Scratch.loadSelf expects 1 arg (slot)", _loc);
			return nullptr;
		}
		auto ic = awst::makeIntrinsicCall("loads", awst::WType::uint64Type(), _loc);
		ic->stackArgs.push_back(bigUIntToUint64(std::move(_args[0]), _loc)); // slot
		return std::shared_ptr<awst::Expression>(std::move(ic));
	}
	if (_method == "load")
	{
		if (_args.size() != 2)
		{
			Logger::instance().error("Scratch.load expects 2 args (groupIndex, slot)", _loc);
			return nullptr;
		}
		auto ic = awst::makeIntrinsicCall("gloadss", awst::WType::uint64Type(), _loc);
		ic->stackArgs.push_back(bigUIntToUint64(std::move(_args[0]), _loc)); // group index
		ic->stackArgs.push_back(bigUIntToUint64(std::move(_args[1]), _loc)); // slot
		return std::shared_ptr<awst::Expression>(std::move(ic));
	}

	// bytes variants: same ops (stores/loads/gloadss accept `any`), no uint64 coercion.
	if (_method == "storeBytes")
	{
		if (_args.size() != 2)
		{
			Logger::instance().error("Scratch.storeBytes expects 2 args (slot, value)", _loc);
			return nullptr;
		}
		auto ic = awst::makeIntrinsicCall("stores", awst::WType::voidType(), _loc);
		ic->stackArgs.push_back(bigUIntToUint64(std::move(_args[0]), _loc)); // slot
		ic->stackArgs.push_back(std::move(_args[1]));                        // value (bytes)
		return std::shared_ptr<awst::Expression>(std::move(ic));
	}
	if (_method == "loadBytesSelf")
	{
		if (_args.size() != 1)
		{
			Logger::instance().error("Scratch.loadBytesSelf expects 1 arg (slot)", _loc);
			return nullptr;
		}
		auto ic = awst::makeIntrinsicCall("loads", awst::WType::bytesType(), _loc);
		ic->stackArgs.push_back(bigUIntToUint64(std::move(_args[0]), _loc)); // slot
		return std::shared_ptr<awst::Expression>(std::move(ic));
	}
	if (_method == "loadBytes")
	{
		if (_args.size() != 2)
		{
			Logger::instance().error("Scratch.loadBytes expects 2 args (groupIndex, slot)", _loc);
			return nullptr;
		}
		auto ic = awst::makeIntrinsicCall("gloadss", awst::WType::bytesType(), _loc);
		ic->stackArgs.push_back(bigUIntToUint64(std::move(_args[0]), _loc)); // group index
		ic->stackArgs.push_back(bigUIntToUint64(std::move(_args[1]), _loc)); // slot
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

	static awst::WInnerTransactionFields s_acfgFieldsType(3);
	auto create = awst::makeCreateInnerTransaction(&s_acfgFieldsType, _loc);

	create->fields["TypeEnum"] = awst::makeIntegerConstant("3", _loc);
	create->fields["Fee"] = awst::makeZero(_loc);
	create->fields["ConfigAssetTotal"] = std::move(total);
	create->fields["ConfigAssetDecimals"] = std::move(decimals);
	create->fields["ConfigAssetUnitName"] = std::move(symbol);
	create->fields["ConfigAssetName"] = std::move(name);
	create->fields["ConfigAssetManager"] = currentAppAddress(_loc);
	create->fields["ConfigAssetReserve"] = currentAppAddress(_loc);
	create->fields["ConfigAssetClawback"] = currentAppAddress(_loc);
	create->fields["ConfigAssetFreeze"] = currentAppAddress(_loc);
	if (defaultFrozen)
		create->fields["ConfigAssetDefaultFrozen"] = std::move(defaultFrozen);

	static awst::WInnerTransaction s_acfgTxnType(3);
	auto submit = awst::makeSubmitInnerTransaction(&s_acfgTxnType, _loc);
	submit->itxns.push_back(std::move(create));

	auto submitStmt = awst::makeExpressionStatement(std::move(submit), _loc);
	_ctx.preEffects().push_back(std::move(submitStmt));

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
	if (_args.size() != 2)
	{
		Logger::instance().error("AVM.asaBalance expects 2 args (holder, assetId)", _loc);
		return nullptr;
	}

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
	return uint64ToBigUInt(std::move(balanceU64), _loc);
}

std::shared_ptr<awst::Expression> AsaIntrinsics::handleAsaTotalSupply(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (_args.size() != 1)
	{
		Logger::instance().error("AVM.asaTotalSupply expects 1 arg (assetId)", _loc);
		return nullptr;
	}

	auto totalU64 = assetParamFirst(
		_ctx, "AssetTotal", std::move(_args[0]), awst::WType::uint64Type(), _loc);
	return uint64ToBigUInt(std::move(totalU64), _loc);
}

std::shared_ptr<awst::Expression> AsaIntrinsics::handleAsaDecimals(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (_args.size() != 1)
	{
		Logger::instance().error("AVM.asaDecimals expects 1 arg (assetId)", _loc);
		return nullptr;
	}

	// AssetDecimals fits in uint8; the tuple-first uint64 is fine.
	return assetParamFirst(
		_ctx, "AssetDecimals", std::move(_args[0]),
		awst::WType::uint64Type(), _loc);
}

std::shared_ptr<awst::Expression> AsaIntrinsics::handleAsaUnitName(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (_args.size() != 1)
	{
		Logger::instance().error("AVM.asaUnitName expects 1 arg (assetId)", _loc);
		return nullptr;
	}

	auto bytes = assetParamFirst(
		_ctx, "AssetUnitName", std::move(_args[0]),
		awst::WType::bytesType(), _loc);
	return awst::makeReinterpretCast(std::move(bytes), awst::WType::stringType(), _loc);
}

std::shared_ptr<awst::Expression> AsaIntrinsics::handleAsaName(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (_args.size() != 1)
	{
		Logger::instance().error("AVM.asaName expects 1 arg (assetId)", _loc);
		return nullptr;
	}

	auto bytes = assetParamFirst(
		_ctx, "AssetName", std::move(_args[0]),
		awst::WType::bytesType(), _loc);
	return awst::makeReinterpretCast(std::move(bytes), awst::WType::stringType(), _loc);
}

std::shared_ptr<awst::Expression> AsaIntrinsics::handleAsaTransfer(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (_args.size() != 4)
	{
		Logger::instance().error("AVM.asaTransfer expects 4 args (assetId, from, to, amount)", _loc);
		return nullptr;
	}

	auto assetId = std::move(_args[0]);
	auto from = std::move(_args[1]);
	auto to = std::move(_args[2]);
	// AVM.asaTransfer declares `uint256 amount`; assert it fits in the
	// uint64 AssetAmount field instead of silently sending `amount mod 2^64`.
	auto amount = builder::TypeCoercion::checkedAmountToUint64(
		_ctx.preEffects(), std::move(_args[3]), _loc);

	static awst::WInnerTransactionFields s_axferFieldsType(4);
	auto create = awst::makeCreateInnerTransaction(&s_axferFieldsType, _loc);

	create->fields["TypeEnum"] = awst::makeIntegerConstant("4", _loc);
	create->fields["Fee"] = awst::makeZero(_loc);
	create->fields["XferAsset"] = std::move(assetId);
	create->fields["AssetSender"] = std::move(from);
	create->fields["AssetReceiver"] = std::move(to);
	create->fields["AssetAmount"] = std::move(amount);

	static awst::WInnerTransaction s_axferTxnType(4);
	auto submit = awst::makeSubmitInnerTransaction(&s_axferTxnType, _loc);
	submit->itxns.push_back(std::move(create));

	auto submitStmt = awst::makeExpressionStatement(std::move(submit), _loc);
	_ctx.preEffects().push_back(std::move(submitStmt));

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
	if (_args.size() != 1)
	{
		Logger::instance().error("AVM.asaOptIn expects 1 arg (assetId)", _loc);
		return nullptr;
	}
	auto assetId = std::move(_args[0]);

	// axfer 0 units to self = standard ASA opt-in.
	static awst::WInnerTransactionFields s_axferFields(4);
	auto create = awst::makeCreateInnerTransaction(&s_axferFields, _loc);
	create->fields["TypeEnum"] = awst::makeIntegerConstant("4", _loc);
	create->fields["Fee"] = awst::makeZero(_loc);
	create->fields["XferAsset"] = std::move(assetId);
	create->fields["AssetReceiver"] = currentAppAddress(_loc);
	create->fields["AssetAmount"] = awst::makeZero(_loc);

	static awst::WInnerTransaction s_axferTxn(4);
	auto submit = awst::makeSubmitInnerTransaction(&s_axferTxn, _loc);
	submit->itxns.push_back(std::move(create));
	_ctx.preEffects().push_back(awst::makeExpressionStatement(std::move(submit), _loc));
	return awst::makeVoidConstant(_loc);
}

std::shared_ptr<awst::Expression> AsaIntrinsics::handleAsaDestroy(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (_args.size() != 1)
	{
		Logger::instance().error("AVM.asaDestroy expects 1 arg (assetId)", _loc);
		return nullptr;
	}
	auto assetId = std::move(_args[0]);

	// acfg with ConfigAsset set and no other config fields = destroy.
	static awst::WInnerTransactionFields s_acfgFields(3);
	auto create = awst::makeCreateInnerTransaction(&s_acfgFields, _loc);
	create->fields["TypeEnum"] = awst::makeIntegerConstant("3", _loc);
	create->fields["Fee"] = awst::makeZero(_loc);
	create->fields["ConfigAsset"] = std::move(assetId);

	static awst::WInnerTransaction s_acfgTxn(3);
	auto submit = awst::makeSubmitInnerTransaction(&s_acfgTxn, _loc);
	submit->itxns.push_back(std::move(create));
	_ctx.preEffects().push_back(awst::makeExpressionStatement(std::move(submit), _loc));
	return awst::makeVoidConstant(_loc);
}

std::shared_ptr<awst::Expression> AsaIntrinsics::handleAsaFreeze(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (_args.size() != 3)
	{
		Logger::instance().error("AVM.asaFreeze expects 3 args (assetId, holder, frozen)", _loc);
		return nullptr;
	}
	auto assetId = std::move(_args[0]);
	auto holder = std::move(_args[1]);
	auto frozen = std::move(_args[2]);

	// afrz (TypeEnum = 5)
	static awst::WInnerTransactionFields s_afrzFields(5);
	auto create = awst::makeCreateInnerTransaction(&s_afrzFields, _loc);
	create->fields["TypeEnum"] = awst::makeIntegerConstant("5", _loc);
	create->fields["Fee"] = awst::makeZero(_loc);
	create->fields["FreezeAsset"] = std::move(assetId);
	create->fields["FreezeAssetAccount"] = std::move(holder);
	create->fields["FreezeAssetFrozen"] = std::move(frozen);

	static awst::WInnerTransaction s_afrzTxn(5);
	auto submit = awst::makeSubmitInnerTransaction(&s_afrzTxn, _loc);
	submit->itxns.push_back(std::move(create));
	_ctx.preEffects().push_back(awst::makeExpressionStatement(std::move(submit), _loc));
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
	auto bytesArg = [&](size_t i) {
		return stringToBytes(std::move(_args[i]), _loc);
	};

	if (_method == "sha512_256")
	{
		if (_args.size() != 1) { Logger::instance().error("Crypto.sha512_256 expects 1 arg", _loc); return nullptr; }
		auto call = awst::makeIntrinsicCall("sha512_256", awst::WType::bytesType(), _loc);
		call->stackArgs.push_back(bytesArg(0));
		return std::shared_ptr<awst::Expression>(call);
	}
	if (_method == "sha3_256")
	{
		if (_args.size() != 1) { Logger::instance().error("Crypto.sha3_256 expects 1 arg", _loc); return nullptr; }
		auto call = awst::makeIntrinsicCall("sha3_256", awst::WType::bytesType(), _loc);
		call->stackArgs.push_back(bytesArg(0));
		return std::shared_ptr<awst::Expression>(call);
	}
	if (_method == "ed25519Verify")
	{
		if (_args.size() != 3) { Logger::instance().error("Crypto.ed25519Verify expects 3 args", _loc); return nullptr; }
		auto call = awst::makeIntrinsicCall("ed25519verify_bare", awst::WType::boolType(), _loc);
		call->stackArgs.push_back(bytesArg(0));
		call->stackArgs.push_back(bytesArg(1));
		call->stackArgs.push_back(bytesArg(2));
		return std::shared_ptr<awst::Expression>(call);
	}
	if (_method == "falconVerify")
	{
		if (_args.size() != 3) { Logger::instance().error("Crypto.falconVerify expects 3 args", _loc); return nullptr; }
		auto call = awst::makeIntrinsicCall("falcon_verify", awst::WType::boolType(), _loc);
		call->stackArgs.push_back(bytesArg(0));
		call->stackArgs.push_back(bytesArg(1));
		call->stackArgs.push_back(bytesArg(2));
		return std::shared_ptr<awst::Expression>(call);
	}
	if (_method == "vrfVerify")
	{
		if (_args.size() != 3) { Logger::instance().error("Crypto.vrfVerify expects 3 args", _loc); return nullptr; }
		auto* tupleType = _ctx.typeMapper.createType<awst::WTuple>(
			std::vector<awst::WType const*>{awst::WType::bytesType(), awst::WType::boolType()});
		auto call = awst::makeIntrinsicCall("vrf_verify", tupleType, _loc);
		call->immediates = {std::string("VrfAlgorand")};
		call->stackArgs.push_back(bytesArg(0));
		call->stackArgs.push_back(bytesArg(1));
		call->stackArgs.push_back(bytesArg(2));
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
	auto gtxnsField = [&](std::string const& field, awst::WType const* wt) -> std::shared_ptr<awst::Expression> {
		if (_args.size() != 1) { Logger::instance().error("Group." + _method + " expects 1 arg (idx)", _loc); return nullptr; }
		return awst::makeGtxns(
			field, bigUIntToUint64(std::move(_args[0]), _loc), wt, _loc);
	};

	if (_method == "size")
		return std::shared_ptr<awst::Expression>(awst::makeGlobal(std::string("GroupSize"), awst::WType::uint64Type(), _loc));
	if (_method == "index")
		return std::shared_ptr<awst::Expression>(awst::makeTxn(std::string("GroupIndex"), awst::WType::uint64Type(), _loc));
	if (_method == "txnSender") return gtxnsField("Sender", awst::WType::accountType());
	if (_method == "txnReceiver") return gtxnsField("Receiver", awst::WType::accountType());
	if (_method == "txnAmount") return gtxnsField("Amount", awst::WType::uint64Type());
	if (_method == "txnAssetReceiver") return gtxnsField("AssetReceiver", awst::WType::accountType());
	if (_method == "txnAssetAmount") return gtxnsField("AssetAmount", awst::WType::uint64Type());
	if (_method == "txnAssetId") return gtxnsField("XferAsset", awst::WType::uint64Type());
	if (_method == "txnApplicationId") return gtxnsField("ApplicationID", awst::WType::uint64Type());
	if (_method == "txnFee") return gtxnsField("Fee", awst::WType::uint64Type());
	if (_method == "txnType") return gtxnsField("TypeEnum", awst::WType::uint64Type());

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
	auto txnField = [&](std::string const& field, awst::WType const* wt) -> std::shared_ptr<awst::Expression> {
		if (!_args.empty()) { Logger::instance().error("Txn." + _method + " expects 0 args", _loc); return nullptr; }
		return awst::makeTxn(field, wt, _loc);
	};

	if (_method == "sender") return txnField("Sender", awst::WType::accountType());
	if (_method == "fee") return txnField("Fee", awst::WType::uint64Type());
	if (_method == "firstValid") return txnField("FirstValid", awst::WType::uint64Type());
	if (_method == "lastValid") return txnField("LastValid", awst::WType::uint64Type());
	if (_method == "note") return txnField("Note", awst::WType::bytesType());
	if (_method == "lease") return txnField("Lease", awst::WType::bytesType());
	if (_method == "typeEnum") return txnField("TypeEnum", awst::WType::uint64Type());
	if (_method == "groupIndex") return txnField("GroupIndex", awst::WType::uint64Type());
	if (_method == "txnId") return txnField("TxID", awst::WType::bytesType());
	if (_method == "rekeyTo") return txnField("RekeyTo", awst::WType::accountType());
	if (_method == "applicationId") return txnField("ApplicationID", awst::WType::uint64Type());
	if (_method == "onCompletion") return txnField("OnCompletion", awst::WType::uint64Type());
	if (_method == "numAppArgs") return txnField("NumAppArgs", awst::WType::uint64Type());
	if (_method == "appArg")
	{
		if (_args.size() != 1) { Logger::instance().error("Txn.appArg expects 1 arg (idx)", _loc); return nullptr; }
		auto call = awst::makeIntrinsicCall("txnas", awst::WType::bytesType(), _loc);
		call->immediates = {std::string("ApplicationArgs")};
		call->stackArgs.push_back(bigUIntToUint64(std::move(_args[0]), _loc));
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
	auto globalField = [&](std::string const& field, awst::WType const* wt) -> std::shared_ptr<awst::Expression> {
		if (!_args.empty()) { Logger::instance().error("Global." + _method + " expects 0 args", _loc); return nullptr; }
		return awst::makeGlobal(field, wt, _loc);
	};
	auto accountStackCall = [&](std::string const& op) -> std::shared_ptr<awst::Expression> {
		if (_args.size() != 1) { Logger::instance().error("Global." + _method + " expects 1 arg (account)", _loc); return nullptr; }
		auto call = awst::makeIntrinsicCall(op, awst::WType::uint64Type(), _loc);
		call->stackArgs.push_back(std::move(_args[0]));
		return call;
	};

	if (_method == "currentApplicationId") return globalField("CurrentApplicationID", awst::WType::uint64Type());
	if (_method == "currentApplicationAddress") return globalField("CurrentApplicationAddress", awst::WType::accountType());
	if (_method == "creatorAddress") return globalField("CreatorAddress", awst::WType::accountType());
	if (_method == "groupId") return globalField("GroupID", awst::WType::bytesType());
	if (_method == "latestTimestamp") return globalField("LatestTimestamp", awst::WType::uint64Type());
	if (_method == "round") return globalField("Round", awst::WType::uint64Type());
	if (_method == "opcodeBudget") return globalField("OpcodeBudget", awst::WType::uint64Type());
	if (_method == "callerApplicationId") return globalField("CallerApplicationID", awst::WType::uint64Type());
	if (_method == "minBalance") return accountStackCall("min_balance");
	if (_method == "balance") return accountStackCall("balance");

	Logger::instance().warning("unknown Global." + _method, _loc);
	return std::nullopt;
}

} // namespace puyasol::builder::eb

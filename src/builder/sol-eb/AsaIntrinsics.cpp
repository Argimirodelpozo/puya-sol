/// @file AsaIntrinsics.cpp
/// AVM stdlib library intercept: turns `AVM.asaCreate / asaBalance /
/// asaTotalSupply / asaTransfer` calls into ASA-native AWST.

#include "builder/sol-eb/AsaIntrinsics.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

namespace puyasol::builder::eb
{

using namespace solidity::frontend;

namespace
{

/// True iff `_memberAccess.expression()` is an Identifier whose
/// referenced declaration is the `AVM` library (per the bundled
/// tokens/AVM.sol).
bool isAvmLibraryAccess(MemberAccess const& _memberAccess)
{
	auto const* baseId = dynamic_cast<Identifier const*>(&_memberAccess.expression());
	if (!baseId)
		return false;

	auto const* contractDef = dynamic_cast<ContractDefinition const*>(
		baseId->annotation().referencedDeclaration);
	if (!contractDef || !contractDef->isLibrary())
		return false;

	return contractDef->name() == "AVM";
}

/// Promote a uint64-typed value to biguint via itob + reinterpret.
std::shared_ptr<awst::Expression> uint64ToBigUInt(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc)
{
	auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
	itob->stackArgs.push_back(std::move(_expr));
	return awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), _loc);
}

/// Truncate a biguint to uint64 via btoi (taking the low 8 bytes).
std::shared_ptr<awst::Expression> bigUIntToUint64(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc)
{
	if (_expr->wtype == awst::WType::uint64Type())
		return _expr;

	// biguint → bytes → low-8-byte extract → btoi
	auto asBytes = awst::makeReinterpretCast(std::move(_expr), awst::WType::bytesType(), _loc);

	// We want the LAST 8 bytes (low-order in big-endian biguint), so use
	// `extract3(b, len(b)-8, 8)` via puya's `extract` intrinsic. Simpler:
	// pad-front and take the trailing 8. The puya runtime's `btoi` on a
	// biguint reinterpreted as bytes accepts arbitrary-length byteslices
	// and reads only the last 8 bytes — verify.
	auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
	btoi->stackArgs.push_back(std::move(asBytes));
	return btoi;
}

/// `global CurrentApplicationAddress` as account-typed expr.
std::shared_ptr<awst::Expression> currentAppAddress(awst::SourceLocation const& _loc)
{
	auto addr = awst::makeIntrinsicCall("global", awst::WType::accountType(), _loc);
	addr->immediates = {std::string("CurrentApplicationAddress")};
	return addr;
}

/// Coerce a `string` AWST expr to bytes (reinterpret — strings are bytes
/// at the AVM level).
std::shared_ptr<awst::Expression> stringToBytes(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc)
{
	if (_expr->wtype == awst::WType::bytesType())
		return _expr;
	return awst::makeReinterpretCast(std::move(_expr), awst::WType::bytesType(), _loc);
}

/// Read field 0 of a (value, exists) tuple returned by asset_holding_get
/// or asset_params_get.
std::shared_ptr<awst::Expression> tupleFirst(
	std::shared_ptr<awst::Expression> _tuple,
	awst::WType const* _firstType,
	awst::SourceLocation const& _loc)
{
	auto out = std::make_shared<awst::TupleItemExpression>();
	out->sourceLocation = _loc;
	out->wtype = _firstType;
	out->base = std::move(_tuple);
	out->index = 0;
	return out;
}

} // namespace

std::optional<std::shared_ptr<awst::Expression>> AsaIntrinsics::tryHandleCall(
	ContractContext& _ctx,
	MemberAccess const& _memberAccess,
	FunctionCall const& _call,
	awst::SourceLocation const& _loc)
{
	if (!isAvmLibraryAccess(_memberAccess))
		return std::nullopt;

	std::string method = _memberAccess.memberName();

	std::vector<std::shared_ptr<awst::Expression>> args;
	for (auto const& arg: _call.arguments())
		args.push_back(_ctx.buildExpr(*arg));

	if (method == "asaCreate")
		return handleAsaCreate(_ctx, args, _loc);
	if (method == "asaBalance")
		return handleAsaBalance(_ctx, args, _loc);
	if (method == "asaTotalSupply")
		return handleAsaTotalSupply(_ctx, args, _loc);
	if (method == "asaTransfer")
		return handleAsaTransfer(_ctx, args, _loc);

	Logger::instance().warning(
		"unknown AVM intrinsic 'AVM." + method + "'; falling through to library resolver", _loc);
	return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> AsaIntrinsics::handleAsaCreate(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (_args.size() != 4)
	{
		Logger::instance().error("AVM.asaCreate expects 4 args (total, decimals, name, symbol)", _loc);
		return nullptr;
	}

	auto total = std::move(_args[0]);
	auto decimals = std::move(_args[1]);
	auto name = stringToBytes(std::move(_args[2]), _loc);
	auto symbol = stringToBytes(std::move(_args[3]), _loc);

	static awst::WInnerTransactionFields s_acfgFieldsType(3);
	auto create = std::make_shared<awst::CreateInnerTransaction>();
	create->sourceLocation = _loc;
	create->wtype = &s_acfgFieldsType;

	create->fields["TypeEnum"] = awst::makeIntegerConstant("3", _loc);
	create->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
	create->fields["ConfigAssetTotal"] = std::move(total);
	create->fields["ConfigAssetDecimals"] = std::move(decimals);
	create->fields["ConfigAssetUnitName"] = std::move(symbol);
	create->fields["ConfigAssetName"] = std::move(name);
	create->fields["ConfigAssetManager"] = currentAppAddress(_loc);
	create->fields["ConfigAssetReserve"] = currentAppAddress(_loc);
	create->fields["ConfigAssetClawback"] = currentAppAddress(_loc);
	create->fields["ConfigAssetFreeze"] = currentAppAddress(_loc);

	static awst::WInnerTransaction s_acfgTxnType(3);
	auto submit = std::make_shared<awst::SubmitInnerTransaction>();
	submit->sourceLocation = _loc;
	submit->wtype = &s_acfgTxnType;
	submit->itxns.push_back(std::move(create));

	auto submitStmt = awst::makeExpressionStatement(std::move(submit), _loc);
	_ctx.prePendingStatements.push_back(std::move(submitStmt));

	// Read the new asset id from the just-submitted itxn context. Stash
	// in a temp local so subsequent itxn submissions don't clobber it.
	auto createdAsaCall = awst::makeIntrinsicCall("itxn", awst::WType::uint64Type(), _loc);
	createdAsaCall->immediates = {std::string("CreatedAssetID")};

	static int s_counter = 0;
	std::string tmpName = "__new_asa_id_" + std::to_string(s_counter++);
	auto tmpTarget = awst::makeVarExpression(tmpName, awst::WType::uint64Type(), _loc);
	auto assign = awst::makeAssignmentStatement(tmpTarget, std::move(createdAsaCall), _loc);
	_ctx.prePendingStatements.push_back(std::move(assign));

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

	auto assetId = std::move(_args[0]);

	auto* tupleType = _ctx.typeMapper.createType<awst::WTuple>(
		std::vector<awst::WType const*>{
			awst::WType::uint64Type(), awst::WType::boolType()});
	auto paramsGet = awst::makeIntrinsicCall("asset_params_get", tupleType, _loc);
	paramsGet->immediates = {std::string("AssetTotal")};
	paramsGet->stackArgs.push_back(std::move(assetId));

	auto totalU64 = tupleFirst(std::move(paramsGet), awst::WType::uint64Type(), _loc);
	return uint64ToBigUInt(std::move(totalU64), _loc);
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
	auto amount = bigUIntToUint64(std::move(_args[3]), _loc);

	static awst::WInnerTransactionFields s_axferFieldsType(4);
	auto create = std::make_shared<awst::CreateInnerTransaction>();
	create->sourceLocation = _loc;
	create->wtype = &s_axferFieldsType;

	create->fields["TypeEnum"] = awst::makeIntegerConstant("4", _loc);
	create->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
	create->fields["XferAsset"] = std::move(assetId);
	create->fields["AssetSender"] = std::move(from);
	create->fields["AssetReceiver"] = std::move(to);
	create->fields["AssetAmount"] = std::move(amount);

	static awst::WInnerTransaction s_axferTxnType(4);
	auto submit = std::make_shared<awst::SubmitInnerTransaction>();
	submit->sourceLocation = _loc;
	submit->wtype = &s_axferTxnType;
	submit->itxns.push_back(std::move(create));

	auto submitStmt = awst::makeExpressionStatement(std::move(submit), _loc);
	_ctx.prePendingStatements.push_back(std::move(submitStmt));

	auto vc = std::make_shared<awst::VoidConstant>();
	vc->sourceLocation = _loc;
	vc->wtype = awst::WType::voidType();
	return vc;
}

} // namespace puyasol::builder::eb

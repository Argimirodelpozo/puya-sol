/// @file SolAddressProperty.cpp
/// Address balance and explicitly adapted AVM code metadata.

#include "builder/ast/members/SolAddressProperty.h"
#include "builder/AwstShorthand.h"
#include "awst/NameGen.h"
#include "builder/target/EvmFeaturePolicy.h"
#include "builder/lowering/intrinsics/AppCodeSizeLowering.h"
#include "builder/lowering/itxn/InnerCallInternal.h"
#include "builder/target/ApplicationTarget.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/TypeCoercion.h"
#include "Logger.h"

namespace puyasol::builder::sol_ast
{

namespace
{

bool isConstantZeroAddress(solidity::frontend::Expression const& baseExpr)
{
	using namespace solidity::frontend;
	auto const* fc = dynamic_cast<solidity::frontend::FunctionCall const*>(&baseExpr);
	if (!fc || !fc->annotation().kind.set()
		|| *fc->annotation().kind != FunctionCallKind::TypeConversion
		|| fc->arguments().size() != 1)
		return false;
	// Solc's rational fact also covers constant expressions, not just literals.
	auto const* rational = dynamic_cast<RationalNumberType const*>(
		fc->arguments()[0]->annotation().type);
	return rational && rational->literalValue(nullptr) == 0;
}

std::shared_ptr<awst::Expression> readApprovalProgram(
	eb::ContractContext& ctx, std::shared_ptr<awst::Expression> appId,
	awst::SourceLocation const& loc)
{
	auto* tupleType = ctx.typeMapper.createType<awst::WTuple>(
		std::vector<awst::WType const*>{
			awst::WType::bytesType(), awst::WType::boolType()});
	auto appParamsGet = awst::makeAppParamsGet(
		"AppApprovalProgram", std::move(appId), tupleType, loc);

	// Stash (bytes, bool) into a fresh temp before extracting the bytes elem.
	// puya's TupleItemExpression miscompiles pop ordering for a raw
	// IntrinsicCall; VarExpression works (same pattern as SolNewExpression).
	// Unique names keep multiple reads in the same expression independent.
	std::string tmpName =
		"__app_program_result_" + std::to_string((awst::NameGen::next("SolAddressProperty.s_appProgramTmpCounter") + 1));
	auto tmpTarget = awst::makeVarExpression(tmpName, tupleType, loc);
	auto assign = awst::makeAssignmentStatement(tmpTarget, std::move(appParamsGet), loc);
	ctx.preEffects().push_back(std::move(assign));

	// Missing apps return a uint64 zero VALUE, not bytes: guard extraction.
	auto exists = awst::makeTupleItem(
		awst::makeVarExpression(tmpName, tupleType, loc), 1,
		awst::WType::boolType(), loc);
	auto item = awst::makeTupleItem(
		awst::makeVarExpression(tmpName, tupleType, loc), 0,
		awst::WType::bytesType(), loc);
	return awst::makeConditional(
		std::move(exists), std::move(item),
		awst::makeBytesConstant({}, loc),
		awst::WType::bytesType(), loc);
}

// `address(contractExpr).balance`: SolTypeConversion builds a fake
// (24-zero-pad ++ itob(app_id)) address; acct_params_get on it returns 0.
// Resolve to the real address via app_params_get AppAddress instead.
// Returns nullptr when the base is not a non-`this` contract-typed cast.
std::shared_ptr<awst::Expression> tryContractTypedBalance(
	eb::ContractContext& ctx, solidity::frontend::MemberAccess const& node,
	awst::SourceLocation const& loc)
{
	auto const* fc = dynamic_cast<solidity::frontend::FunctionCall const*>(&node.expression());
	if (!fc || *fc->annotation().kind != solidity::frontend::FunctionCallKind::TypeConversion
		|| fc->arguments().size() != 1)
		return nullptr;

	auto const* innerType = fc->arguments()[0]->annotation().type;
	bool isContractType = dynamic_cast<
		solidity::frontend::ContractType const*>(innerType) != nullptr;
	// Skip for `address(this)` — fallback emits
	// `global CurrentApplicationAddress` directly.
	bool isThis = false;
	if (auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(
		fc->arguments()[0].get()))
	{
		if (id->name() == "this")
			isThis = true;
	}
	if (!isContractType || isThis)
		return nullptr;

	auto appExpr = ctx.buildExpr(*fc->arguments()[0]);
	auto appIdUint = ApplicationTarget::requireApplication(
		ApplicationTarget::resolve(ctx.typeMapper.profile(), std::move(appExpr), loc), loc);
	auto* addrTupleType = ctx.typeMapper.createType<awst::WTuple>(
		std::vector<awst::WType const*>{
			awst::WType::bytesType(), awst::WType::boolType()});
	auto appParamsGet = awst::makeAppParamsGet(
		"AppAddress", std::move(appIdUint), addrTupleType, loc);

	// Counter-guarded so two `address(c).balance` in one expression
	// don't alias the same temp (second app_params_get would clobber).
	std::string addrTmp =
		"__app_balance_addr_" + std::to_string((awst::NameGen::next("SolAddressProperty.s_appBalanceTmpCounter") + 1));
	auto addrTmpTarget = awst::makeVarExpression(addrTmp, addrTupleType, loc);
	auto addrAssign = awst::makeAssignmentStatement(
		addrTmpTarget, std::move(appParamsGet), loc);
	ctx.preEffects().push_back(std::move(addrAssign));

	auto addrTupleRead = awst::makeVarExpression(addrTmp, addrTupleType, loc);
	auto addrBytesItem = awst::makeTupleItem(std::move(addrTupleRead), 0, awst::WType::bytesType(), loc);
	auto realAddr = awst::makeAsAccount(std::move(addrBytesItem), loc);

	auto* balTupleType = ctx.typeMapper.createType<awst::WTuple>(
		std::vector<awst::WType const*>{
			awst::WType::uint64Type(), awst::WType::boolType()});
	auto acctParams = awst::makeIntrinsicCall(
		"acct_params_get", balTupleType, loc);
	acctParams->immediates = {std::string("AcctBalance")};
	acctParams->stackArgs.push_back(std::move(realAddr));

	auto bal = awst::makeTupleItem(std::move(acctParams), 0, awst::WType::uint64Type(), loc);

	auto itobBal = awst::makeItob(std::move(bal), loc);
	return awst::makeAsBiguint(std::move(itobBal), loc);
}

std::shared_ptr<awst::Expression> buildAddressBalance(
	eb::ContractContext& ctx, Context&,
	solidity::frontend::MemberAccess const& node, awst::WType const*,
	awst::SourceLocation const& loc)
{
	// address.balance → acct_params_get AcctBalance → uint64 → biguint
	EvmFeaturePolicy::report(
		EvmFeature::AddressBalance, ctx.typeMapper.profile(), loc);

	if (auto contractBalance = tryContractTypedBalance(ctx, node, loc))
		return contractBalance;

	auto addrExpr = ctx.buildExpr(node.expression());

	auto* tupleType = ctx.typeMapper.createType<awst::WTuple>(
		std::vector<awst::WType const*>{
			awst::WType::uint64Type(), awst::WType::boolType()});
	auto acctParams = awst::makeIntrinsicCall("acct_params_get", tupleType, loc);
	acctParams->immediates = {std::string("AcctBalance")};
	acctParams->stackArgs.push_back(std::move(addrExpr));

	// Solidity balance is uint256 — promote uint64 → biguint
	auto balanceVal = awst::makeTupleItem(std::move(acctParams), 0, awst::WType::uint64Type(), loc);
	auto itob = awst::makeItob(std::move(balanceVal), loc);
	return awst::makeAsBiguint(std::move(itob), loc);
}

} // anonymous namespace

std::shared_ptr<awst::Expression> SolAddressProperty::buildCodeMetadata(
	eb::ContractContext& ctx, Context& scope,
	solidity::frontend::Expression const& source, CodeProperty property,
	awst::SourceLocation const& loc)
{
	// Even a constant metadata result must evaluate its receiver, including
	// calls, reverts and delayed storage write-backs, exactly once.
	auto receiver = ctx.lower(source, false);
	bool const self = shorthand::isCurrentAppAddressGlobal(receiver.value.get());
	bool const zero = isConstantZeroAddress(source);
	bool const precompile = eb::detectPrecompileAddress(source).has_value();
	auto address = ctx.emitSequencedOperand(
		std::move(receiver.effects), std::move(receiver.value), true, loc);
	auto empty = [&]() -> std::shared_ptr<awst::Expression> {
		if (property == CodeProperty::Size)
			return awst::makeZero(loc, awst::WType::uint64Type());
		auto bytes = awst::makeBytesConstant({}, loc);
		if (property == CodeProperty::Hash)
			return awst::makeReinterpretCast(awst::makeKeccak256(bytes, loc),
				ctx.typeMapper.createType<awst::BytesWType>(32), loc);
		return bytes;
	};
	if (property == CodeProperty::Hash)
	{
		if (!self && !zero && !precompile)
			Logger::instance().error(
				"`address(addr).codehash` for a non-`this` address is not supported "
				"on AVM. An address literal does not prove account existence or "
				"EVM code identity; only self and the zero/precompile convention "
				"are supported.", loc);
		else
			Logger::instance().warning(
				"`address.codehash` uses AVM approval-program bytes, not EVM "
				"bytecode identity. Self during construction hashes empty code; "
				"constant zero/precompile addresses use the compiler's "
				"zero/empty-code convention, not an account-state lookup.", loc);
		if (!self && !precompile)
			return awst::makeBytesConstant(std::vector<uint8_t>(32, 0), loc,
				awst::BytesEncoding::Base16, ctx.typeMapper.createType<awst::BytesWType>(32));
	}
	else
		Logger::instance().warning(property == CodeProperty::Size
			? "`address.code.length` returns allocated AVM program capacity, not exact "
			  "EVM code size. Only self and zero-padded application-id addresses resolve; "
			  "missing applications read as zero."
			: "`address.code` returns AVM approval-program bytes, not EVM bytecode. "
			  "Only self and zero-padded application-id addresses resolve; missing "
			  "applications read as empty. Programs exceeding the AVM stack byte-value "
			  "limit cannot be materialised.", loc);

	if (zero || precompile || (self && scope.isInConstructor()))
		return empty();

	auto currentId = awst::makeGlobal("CurrentApplicationID", awst::WType::uint64Type(), loc);
	auto appId = ApplicationTarget::resolve(ctx.typeMapper.profile(), std::move(address), loc);
	if (property == CodeProperty::Size)
		return AppCodeSizeLowering::lower(ctx.typeMapper, std::move(appId), loc,
			ctx.preEffects(), scope.isInConstructor());
	appId = ctx.emitSequencedOperand({}, std::move(appId), true, loc);
	// AVM reference zero aliases self. Suppress it, and only the current
	// contract during construction, before issuing any application lookup.
	auto readable = awst::makeNumericCompare(appId, awst::NumericComparison::Ne,
		awst::makeZero(loc), loc);
	if (scope.isInConstructor())
		readable = awst::makeNumericCompare(
			awst::makeConditional(readable, appId, currentId, awst::WType::uint64Type(), loc),
			awst::NumericComparison::Ne, currentId, loc);
	auto read = ctx.lowerOperand([&]() -> std::shared_ptr<awst::Expression> {
		auto code = readApprovalProgram(ctx, appId, loc);
		if (property == CodeProperty::Hash)
			return awst::makeReinterpretCast(awst::makeKeccak256(code, loc),
				ctx.typeMapper.createType<awst::BytesWType>(32), loc);
		return code;
	});
	auto noCode = ctx.lowerOperand(empty);
	auto* type = read.value->wtype;
	return ctx.emitConditional(readable, std::move(read), std::move(noCode), type, loc);
}

std::shared_ptr<awst::Expression> SolAddressProperty::toAwst()
{
	auto const& member = memberName();
	if (member == "balance")
		return buildAddressBalance(m_ctx, m_scope, m_memberAccess, m_wtype, m_loc);
	if (member == "code" || member == "codehash")
		return buildCodeMetadata(m_ctx, m_scope, baseExpression(),
			member == "code" ? CodeProperty::Bytes : CodeProperty::Hash, m_loc);

	Logger::instance().warning("address property '." + member + "' has no Algorand equivalent", m_loc);
	return awst::makeBytesConstant({}, m_loc);
}

} // namespace puyasol::builder::sol_ast

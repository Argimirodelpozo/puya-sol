/// @file SolAddressProperty.cpp
/// address.code → app_params_get AppApprovalProgram.
/// Registry shape: one handler per member; unknown members warn and return empty bytes.

#include "builder/sol-ast/members/SolAddressProperty.h"
#include "builder/AwstShorthand.h"
#include "awst/NameGen.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <variant>

namespace puyasol::builder::sol_ast
{

namespace
{

// `address(N)` for literal N — the type-conversion FunctionCall wrapping a
// number literal. Precompile/EOA addresses under the compiler's
// contract-value convention.
solidity::frontend::Literal const* literalNumberArg(
	solidity::frontend::Expression const& baseExpr)
{
	auto const* fc = dynamic_cast<solidity::frontend::FunctionCall const*>(&baseExpr);
	if (!fc || *fc->annotation().kind != solidity::frontend::FunctionCallKind::TypeConversion
		|| fc->arguments().size() != 1)
		return nullptr;
	auto const* lit = dynamic_cast<solidity::frontend::Literal const*>(fc->arguments()[0].get());
	if (lit && lit->token() == solidity::frontend::Token::Number)
		return lit;
	return nullptr;
}

// Built base lowers to `global CurrentApplicationAddress`?
constexpr auto isCurrentAppAddress = builder::shorthand::isCurrentAppAddressGlobal;

std::shared_ptr<awst::Expression> buildAddressCode(
	eb::ContractContext& ctx, Context& scope,
	solidity::frontend::MemberAccess const& node, awst::WType const*,
	awst::SourceLocation const& loc)
{
	// EVM: extcodesize(this)==0 DURING construction (code isn't stored until
	// initcode returns). The AVM app program exists at create time, so match
	// EVM at compile time instead (same pattern as ctor msg.data → empty).
	if (scope.isInConstructor())
		return awst::makeBytesConstant({}, loc);

	// address(N).code for literal N → empty bytes. Precompile/EOA
	// addresses have no code; app_params_get panics on non-existent app ids.
	if (literalNumberArg(node.expression()))
		return awst::makeBytesConstant({}, loc);

	auto addrExpr = ctx.buildExpr(node.expression());

	// address(this) lowers to `global CurrentApplicationAddress`; swap in
	// CurrentApplicationID for app_params_get. Deriving app id from the
	// last 8 bytes of the address is unreliable across network configs.
	std::shared_ptr<awst::Expression> appId;
	if (isCurrentAppAddress(addrExpr.get()))
	{
		auto idCall = awst::makeGlobal(std::string("CurrentApplicationID"), awst::WType::uint64Type(), loc);
		auto cast = awst::makeAsApplication(std::move(idCall), loc);
		appId = std::move(cast);
	}

	if (!appId)
	{
		// Arbitrary address: derive the app id from the address's last 8
		// bytes — THIS CODEBASE'S contract-value convention
		// (bzero(24) ++ itob(appId)), the same one external calls use to
		// recover a call target (TypeCoercion account→application). Then
		// app_params_get yields (approvalProgram, exists), so
		// `addr.code.length > 0` — the SafeTransferLib / OZ Address.isContract
		// guard, and Morpho's NO_CODE check — answers correctly: a real
		// program length for a deployed app, 0 for anything else.
		// NOT a fabricated constant (contrast the old extcodesize stub that
		// returned 1 for everything): a genuine lookup whose only
		// assumption is the address form this compiler itself produces.
		Logger::instance().warning(
			"`address(addr).code` on a non-`this` address resolves the app id "
			"from the address's last 8 bytes (this compiler's contract-value "
			"convention) and returns that application's approval program. "
			"An address NOT in that form reads as 'no code'.", loc);
		auto toBytes = awst::makeAsBytes(addrExpr, loc);
		auto idU64 = awst::makeWord32ToUInt64(std::move(toBytes), loc);
		appId = awst::makeAsApplication(std::move(idU64), loc);
	}

	auto* tupleType = ctx.typeMapper.createType<awst::WTuple>(
		std::vector<awst::WType const*>{
			awst::WType::bytesType(), awst::WType::boolType()});
	auto appParamsGet = awst::makeAppParamsGet(
		"AppApprovalProgram", std::move(appId), tupleType, loc);

	// Stash (bytes, bool) into a fresh temp before extracting the bytes elem.
	// puya's TupleItemExpression miscompiles pop ordering for a raw
	// IntrinsicCall; VarExpression works (same pattern as SolNewExpression).
	// Counter makes the name unique: two `.code` reads in one expression
	// would both read the second call's temp if they shared a name
	// (all prepends run before the returned expression is consumed).
	std::string tmpName =
		"__app_program_result_" + std::to_string((awst::NameGen::next("SolAddressProperty.s_appProgramTmpCounter") + 1));
	auto tmpTarget = awst::makeVarExpression(tmpName, tupleType, loc);
	auto assign = awst::makeAssignmentStatement(tmpTarget, std::move(appParamsGet), loc);
	ctx.preEffects().push_back(std::move(assign));

	// Branch on the exists flag rather than reading element 0 blind. For a
	// non-existent app the AVM pushes a uint64 zero as the VALUE whatever
	// the field's declared type, so a downstream `len` fails at runtime
	// ("wanted []byte but got uint64"). That made `eoa.code.length` — the
	// OZ Address.isContract guard this lowering exists to serve — revert
	// instead of answering 0. The literal-address case above is folded at
	// compile time; this is the runtime-address twin.
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
	// Contract-typed identifiers lower to accountType (fake address);
	// reverse to app id. If already applicationType, reinterpret directly.
	std::shared_ptr<awst::Expression> appIdUint;
	if (appExpr->wtype == awst::WType::accountType())
	{
		auto appAsApp = TypeCoercion::coerceForAssignment(
			std::move(appExpr), awst::WType::applicationType(), loc);
		appIdUint = awst::makeAsUInt64(std::move(appAsApp), loc);
	}
	else
	{
		appIdUint = awst::makeAsUInt64(std::move(appExpr), loc);
	}
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
	Logger::instance().warning(
		"address.balance returns the account balance in microAlgos on AVM, "
		"not wei. 1 microAlgo = 1e-6 ALGO. This is NOT equivalent to EVM wei "
		"(1 wei = 1e-18 ETH). Ensure your contract logic accounts for this difference.", loc);

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

std::shared_ptr<awst::Expression> buildAddressCodehash(
	eb::ContractContext& ctx, Context&,
	solidity::frontend::MemberAccess const& node, awst::WType const* wtype,
	awst::SourceLocation const& loc)
{
	// address(this).codehash → keccak256(AppApprovalProgram).
	// Non-current addresses can't be cheaply resolved to an app id on AVM.
	// Literal bridge: address(0) → bytes32(0); address(1..10) → keccak256("") (no code, EVM convention).
	if (auto const* lit = literalNumberArg(node.expression()))
	{
		auto litVal = lit->annotation().type->literalValue(lit);
		auto* w = ctx.typeMapper.createType<awst::BytesWType>(32);
		if (litVal == 0)
			return awst::makeBytesConstant(std::vector<uint8_t>(32, 0), loc, awst::BytesEncoding::Base16, w);
		return awst::makeBytesConstant(
			// keccak256 of empty bytes
			std::vector<uint8_t>{
				0xc5, 0xd2, 0x46, 0x01, 0x86, 0xf7, 0x23, 0x3c,
				0x92, 0x7e, 0x7d, 0xb2, 0xdc, 0xc7, 0x03, 0xc0,
				0xe5, 0x00, 0xb6, 0x53, 0xca, 0x82, 0x27, 0x3b,
				0x7b, 0xfa, 0xd8, 0x04, 0x5d, 0x85, 0xa4, 0x70},
			loc, awst::BytesEncoding::Base16, w);
	}
	auto addrExpr = ctx.buildExpr(node.expression());
	if (isCurrentAppAddress(addrExpr.get()))
	{
		auto appId = awst::makeGlobal(std::string("CurrentApplicationID"), awst::WType::uint64Type(), loc);
		auto appIdCast = awst::makeAsApplication(std::move(appId), loc);

		auto* tupleType = ctx.typeMapper.createType<awst::WTuple>(
			std::vector<awst::WType const*>{
				awst::WType::bytesType(), awst::WType::boolType()});
		auto appParamsGet = awst::makeAppParamsGet(
			"AppApprovalProgram", std::move(appIdCast), tupleType, loc);

		auto bytesOut = awst::makeTupleItem(std::move(appParamsGet), 0, awst::WType::bytesType(), loc);

		auto hash = awst::makeKeccak256(std::move(bytesOut), loc);

		if (wtype && wtype != awst::WType::bytesType())
		{
			auto cast = awst::makeReinterpretCast(std::move(hash), wtype, loc);
			return cast;
		}
		return hash;
	}
	// Arbitrary address → HARD ERROR. Old stub returned bytes32(0), which
	// silently corrupts codehash-based identity checks.
	Logger::instance().error(
		"`address(addr).codehash` for a non-`this` address is not supported "
		"on AVM — an arbitrary address can't be dereferenced to its code, so "
		"the old stub returned bytes32(0). `address(this).codehash` is "
		"supported.", loc);
	return awst::makeBytesConstant(
		std::vector<uint8_t>(32, 0), loc, awst::BytesEncoding::Base16,
		ctx.typeMapper.createType<awst::BytesWType>(32));
}

using MemberHandler = std::shared_ptr<awst::Expression> (*)(
	eb::ContractContext&, Context&,
	solidity::frontend::MemberAccess const&, awst::WType const*,
	awst::SourceLocation const&);

struct MemberEntry
{
	char const* member;
	MemberHandler fn;
};

constexpr MemberEntry kAddressMembers[] = {
	{"code", buildAddressCode},
	{"balance", buildAddressBalance},
	{"codehash", buildAddressCodehash},
};

} // anonymous namespace

std::shared_ptr<awst::Expression> SolAddressProperty::toAwst()
{
	std::string member = memberName();

	for (auto const& entry: kAddressMembers)
		if (member == entry.member)
			return entry.fn(m_ctx, m_scope, m_memberAccess, m_wtype, m_loc);

	Logger::instance().warning("address property '." + member + "' has no Algorand equivalent", m_loc);
	return awst::makeBytesConstant({}, m_loc);
}

} // namespace puyasol::builder::sol_ast

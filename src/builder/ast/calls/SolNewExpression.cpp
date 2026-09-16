/// @file SolNewExpression.cpp
/// new bytes(N), new T[](N), new Contract(...).

#include "builder/ast/calls/SolNewExpression.h"
#include "builder/solc/SolcFacts.h"
#include "builder/lowering/itxn/ApplicationCall.h"
#include "builder/context/BuildArtifacts.h"
#include "awst/NameGen.h"
#include "builder/storage/StateVarWalker.h"
#include "builder/types/TypeMapper.h"
#include "builder/codec/Arc4Defaults.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/ConversionPlan.h"
#include "builder/lowering/abi/AbiEncoderBuilder.h"
#include "builder/types/ConstructorWirePlan.h"
#include "builder/storage/slot/EvmSlotLowering.h"
#include "builder/lowering/itxn/NativePayment.h"
#include "builder/contract/PostInitTriggers.h"
#include "builder/lowering/itxn/ChildDeployment.h"
#include "builder/types/SolIntType.h"
#include "builder/storage/StorageMapper.h"
#include "builder/target/EvmLayoutMode.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

std::shared_ptr<awst::Expression> SolNewExpression::allocationSize(uint64_t capacity)
{
	auto size = CallOperands::evaluate(m_ctx, *arguments().at(0), m_loc);
	size = TypeCoercion::checkedAllocationSizeToUint64(m_ctx.preEffects(), std::move(size), m_loc);
	m_ctx.preEffects().push_back(awst::makeExpressionStatement(awst::makeAssert(
		awst::makeNumericCompare(size, awst::NumericComparison::Lte,
			awst::makeIntegerConstant(capacity, m_loc), m_loc),
		m_loc, "allocation exceeds AVM value capacity"), m_loc));
	return size;
}

std::shared_ptr<awst::Expression> SolNewExpression::handleNewBytes()
{
	auto zero = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), m_loc);
	zero->stackArgs.push_back(allocationSize(4096));
	auto const* resultType = m_ctx.typeMapper.map(m_call.annotation().type);
	if (resultType == awst::WType::bytesType()) return zero;
	return awst::makeReinterpretCast(std::move(zero), resultType, m_loc);
}

std::shared_ptr<awst::Expression> SolNewExpression::handleNewArray()
{
	auto const* array = dynamic_cast<ArrayType const*>(m_call.annotation().type);
	assert(array);
	auto const* resultType = m_ctx.typeMapper.map(array);
	auto const* elemType = m_ctx.typeMapper.mapSolTypeToARC4(array->baseType());
	bool const packedBool = elemType == awst::WType::arc4BoolType()
		&& resultType->kind() == awst::WTypeKind::ARC4DynamicArray;
	auto const encodedDefault = builder::arc4DefaultEncoding(elemType);
	uint64_t const header = resultType->kind() == awst::WTypeKind::ARC4DynamicArray ? 2 : 0;
	uint64_t const elementBytes = encodedDefault
		? encodedDefault->size() + (builder::arc4IsDynamic(elemType) ? 2 : 0) : 0;
	uint64_t const capacity = packedBool ? (4096 - header) * 8
		: elementBytes ? (4096 - header) / elementBytes : 65535;
	auto size = allocationSize(capacity);

	// Packed bools bypass Puya's empty-array setbit encoder. Both constant
	// and runtime sizes use the same uint16 length + zeroed packed body.
	if (packedBool)
	{
		auto byteLen = awst::makeUInt64BinOp(
			awst::makeUInt64BinOp(size, awst::UInt64BinaryOperator::Add,
				awst::makeIntegerConstant(7, m_loc), m_loc),
			awst::UInt64BinaryOperator::FloorDiv, awst::makeIntegerConstant(8, m_loc), m_loc);
		auto zero = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), m_loc);
		zero->stackArgs.push_back(std::move(byteLen));
		return awst::makeReinterpretCast(awst::makeConcat(
			awst::makeExtract(awst::makeItob(size, m_loc), 6, 2, m_loc),
			std::move(zero), m_loc), resultType, m_loc);
	}

	auto initial = awst::makeNewArray(resultType, m_loc);
	// solc rational facts are safe to fold; a local initializer is not proof
	// that the local still has that value at this allocation site.
	if (auto const* rational = dynamic_cast<RationalNumberType const*>(arguments()[0]->annotation().type))
	{
		auto const n = rational->literalValue(nullptr);
		if (n <= capacity && header + n * elementBytes <= 4000)
		{
			for (uint64_t i = 0; i < n; ++i)
				initial->values.push_back(TypeCoercion::makeDefaultValue(elemType, m_loc));
			return initial;
		}
	}

	auto const suffix = std::to_string(awst::NameGen::next("SolNewExpression.rtArrayCounter"));
	auto arr = awst::makeVarExpression("__rt_arr_" + suffix, resultType, m_loc);
	auto idx = awst::makeVarExpression("__rt_idx_" + suffix, awst::WType::uint64Type(), m_loc);
	m_ctx.preEffects().push_back(awst::makeAssignmentStatement(arr, std::move(initial), m_loc));
	m_ctx.preEffects().push_back(awst::makeAssignmentStatement(idx, awst::makeZero(m_loc), m_loc));
	auto body = awst::makeBlock(m_loc);
	auto one = awst::makeNewArray(resultType, m_loc);
	one->values.push_back(TypeCoercion::makeDefaultValue(elemType, m_loc));
	body->body.push_back(awst::makeExpressionStatement(awst::makeArrayExtend(arr, std::move(one), m_loc), m_loc));
	body->body.push_back(awst::makeAssignmentStatement(idx, awst::makeUInt64BinOp(
		idx, awst::UInt64BinaryOperator::Add, awst::makeOne(m_loc), m_loc), m_loc));
	m_ctx.preEffects().push_back(awst::makeWhileLoop(
		awst::makeNumericCompare(idx, awst::NumericComparison::Lt, size, m_loc), std::move(body), m_loc));
	return arr;
}

std::shared_ptr<awst::Expression> SolNewExpression::toAwst()
{
	auto* resultType = m_ctx.typeMapper.map(m_call.annotation().type);

	if (resultType && resultType->kind() == awst::WTypeKind::Bytes)
		return handleNewBytes();

	if (resultType == awst::WType::stringType())
		return handleNewBytes();

	if (resultType && (resultType->kind() == awst::WTypeKind::ARC4StaticArray
		|| resultType->kind() == awst::WTypeKind::ARC4DynamicArray))
		return handleNewArray();

	// new Contract(...) — deploy child contract via inner app creation transaction.
	// The deployment artifact pass supplies the child's compiled programs.
	rejectCreate2Salt();

	auto const& funcExpr = funcExpression();
	if (auto const* newExpr = SolcFacts::expressionAs<NewExpression>(&funcExpr))
	{
		auto const* contractType = dynamic_cast<ContractType const*>(
			newExpr->typeName().annotation().type);
		if (contractType)
			return handleNewContract(*contractType);
	}

	auto vc = awst::makeVoidConstant(m_loc);
	return vc;
}

// ── new Contract(...) stages, in emission order ──────────────────────

void SolNewExpression::rejectCreate2Salt()
{
	// `new C{salt:s}(...)` is CREATE2. CREATE2's address derivation (salt+initcode
	// hash) has no AVM equivalent — fail loud rather than silently wrong-lower.
	auto const* type = dynamic_cast<FunctionType const*>(m_call.expression().annotation().type);
	if (type && type->saltSet())
		Logger::instance().error(
			"`new C{salt: ...}(...)` (CREATE2) is not supported on AVM. "
			"CREATE2's deterministic address derivation (salt + initcode "
			"hash) has no AVM equivalent — app IDs are assigned "
			"sequentially by the chain at inner-app-create time, so a "
			"salt-derived address can't be pre-computed. Use plain "
			"`new C(...)` if you don't need address prediction.",
			m_loc);
}

std::shared_ptr<awst::Expression> SolNewExpression::handleNewContract(
	ContractType const& _contractType)
{
	std::string childName = _contractType.contractDefinition().name();
	Logger::instance().info(
		"'new " + childName + "()' — using template variables for "
		"child bytecode (substitute before deployment).");

	// Track this child contract for .tmpl file generation
	m_ctx.typeMapper.artifacts().childContracts.insert(childName);

	// __postInit needed when ctor reads msg.value/sender/data (unavailable
	// at AppCreate time where sender/value belong to the parent).
	auto const* childCtor = _contractType.contractDefinition().constructor();
	// THE postInit decision — the SAME computeNeedsPostInit the child compiles
	// with (PostInitTriggers: box writes, new C(), msg.*, AVM stdlib calls). This
	// used to be a local msg.*-only re-derivation that DRIFTED from the child:
	// a ctor writing a box-stored state var (dynamic array `s_ = s`) made the
	// child defer ALL init to __postInit while the caller passed args at create
	// and never called __postInit -> child deployed with NO state, failing only
	// on the first read (arrays_in_constructors).
	bool childHasPostInit = computeNeedsPostInit(
		_contractType.contractDefinition(), m_ctx.storageMapper, m_ctx.typeMapper.analysis());

	ConstructorWirePlan wire(m_ctx.typeMapper, childCtor, childHasPostInit);
	// Solidity evaluates options and source arguments before creating the child.
	// Encoding consumes those captured operands; funding/postInit never re-lower them.
	auto callValue = extractCallValue();
	auto values = CallOperands::build(m_ctx, m_call, m_loc,
		[&](Expression const& source, size_t i) {
			auto const& parameter = wire.parameters.at(i);
			auto value = EvmSlotLowering::materializeRefValue(m_ctx, m_scope,
				buildExpr(source), source.annotation().type, parameter.type, m_loc);
			return ConversionPlan{source.annotation().type, parameter.declaration->type(), parameter.type,
				ConversionPlan::Context::Argument}.emit(std::move(value), m_loc, &m_ctx.preEffects());
		});
	auto applicationArgs = buildChildArgs(wire, std::move(values), childHasPostInit);

	// Build inner appl create transaction with TemplateVar programs
	static awst::WInnerTransactionFields s_applFieldsType(6); // appl
	auto create = awst::makeCreateInnerTransaction(&s_applFieldsType, m_loc);

	auto makeU64 = [&](std::string val) {
		auto c = awst::makeIntegerConstant(std::move(val), m_loc);
		return c;
	};
	create->fields["TypeEnum"] = makeU64("6");
	create->fields["Fee"] = makeU64("0");
	for (auto const& field: childSchemaFields)
		create->fields[field.transactionField] = awst::makeTemplateVar(
			"TMPL_CHILD_" + childName + "_" + field.transactionField, awst::WType::uint64Type(), m_loc);
	auto pages = buildChildApprovalPages(childName);
	create->fields["ApprovalProgramPages"] = pages;

	// ClearStateProgram = TemplateVar("TMPL_CLEAR_ChildName")
	create->fields["ClearStateProgram"] = awst::makeTemplateVar(
		"TMPL_CLEAR_" + childName, awst::WType::bytesType(), m_loc);
	// ExtraProgramPages counts 2 KiB of combined approval + clear bytes,
	// independently of the 4 KiB AVM byte-value chunks used above. Measuring
	// the actual supplied programs also covers box-provisioned children.
	std::shared_ptr<awst::Expression> programBytes = awst::makeLen(create->fields["ClearStateProgram"], m_loc);
	for (auto const& page: pages->items)
		programBytes = awst::makeUInt64BinOp(std::move(programBytes),
			awst::UInt64BinaryOperator::Add, awst::makeLen(page, m_loc), m_loc);
	create->fields["ExtraProgramPages"] = awst::makeUInt64BinOp(
		awst::makeUInt64BinOp(awst::makeUInt64BinOp(std::move(programBytes),
			awst::UInt64BinaryOperator::Add, awst::makeIntegerConstant(2047, m_loc), m_loc),
			awst::UInt64BinaryOperator::FloorDiv, awst::makeIntegerConstant(2048, m_loc), m_loc),
		awst::UInt64BinaryOperator::Sub, awst::makeOne(m_loc), m_loc);

	// No __postInit: ctor runs during AppCreate. EVM profile carries one
	// canonical constructor body; ARC4 profile keeps one encoded arg per slot.
	if (!childHasPostInit && applicationArgs)
		create->fields["ApplicationArgs"] = applicationArgs;

	// Submit the inner transaction
	static awst::WInnerTransaction s_applTxnType(6);
	auto submit = awst::makeSubmitInnerTransaction(&s_applTxnType, m_loc);
	submit->itxns.push_back(std::move(create));

	auto submitStmt = awst::makeExpressionStatement(std::move(submit), m_loc);
	m_ctx.preEffects().push_back(std::move(submitStmt));

	// Read CreatedApplicationID via itxn intrinsic and save to temp var
	// because subsequent fund txn would clobber the itxn context.
	auto createdAppIdCall = awst::makeItxn(
		"CreatedApplicationID", awst::WType::uint64Type(), m_loc);

	int newAppId = awst::NameGen::next("SolNewExpression.newAppIdCounter");
	std::string newAppIdVarName = "__new_app_id_" + std::to_string(newAppId);
	auto newAppIdTarget = awst::makeVarExpression(newAppIdVarName, awst::WType::uint64Type(), m_loc);
	auto newAppIdAssign = awst::makeAssignmentStatement(newAppIdTarget, std::move(createdAppIdCall), m_loc);
	m_ctx.preEffects().push_back(std::move(newAppIdAssign));

	// Use the stored app ID from now on
	auto createdAppId = awst::makeVarExpression(newAppIdVarName, awst::WType::uint64Type(), m_loc);

	emitChildFunding(createdAppId, childHasPostInit ? nullptr : callValue);

	if (childHasPostInit)
		emitChildPostInit(createdAppId, std::move(applicationArgs), std::move(callValue));
	ApplicationCall::setReturnData(m_ctx.typeMapper, awst::makeBytesConstant({}, m_loc),
		m_loc, m_ctx.preEffects());

	// Return as applicationType (avoids address-hash conversion for calls).
	auto appIdCast = awst::makeAsApplication(std::move(createdAppId), m_loc);

	return appIdCast;
}

std::shared_ptr<awst::TupleExpression> SolNewExpression::buildChildApprovalPages(
	std::string const& _childName)
{
	// ApprovalProgramPages = two ≤4096-byte pages: a single AVM stack
	// value (and bytecblock constant) caps at 4096 bytes while a
	// 3-extra-page program can reach 8192. Sources:
	//   default: TMPL_APPROVAL_ChildName_P0/_P1 template constants
	//     (page 1 substitutes to empty for small children);
	//   --child-programs-via-box: slices of the deployer-provisioned
	//     "__cp_<Child>" box (the program never enters the parent
	//     bytecode — 16KB-cap relief). Page split at runtime from
	//     box_len; box_extract(key, len, 0) is valid for the empty
	//     tail, so no branch on the extract itself.
	auto pages = awst::makeTupleExpression(nullptr, m_loc);
	if (m_ctx.typeMapper.profile().childProgramsViaBox)
	{
		m_ctx.typeMapper.artifacts().contract().boxProvisionedChildren
			.insert(_childName);
		auto boxKey = [&]() {
			return awst::makeUtf8BytesConstant(
				"__cp_" + _childName, m_loc);
		};
		auto lenU64 = [&]() {
			auto* tupleT = m_ctx.typeMapper.createType<awst::WTuple>(
				std::vector<awst::WType const*>{
					awst::WType::uint64Type(),
					awst::WType::boolType()});
			return awst::makeTupleItem(
				awst::makeBoxLen(boxKey(), tupleT, m_loc), 0,
				awst::WType::uint64Type(), m_loc);
		};
		auto page0Len = [&]() {
			return awst::makeConditional(
				awst::makeNumericCompare(lenU64(),
					awst::NumericComparison::Gt,
					awst::makeIntegerConstant("4096", m_loc), m_loc),
				awst::makeIntegerConstant("4096", m_loc), lenU64(),
				awst::WType::uint64Type(), m_loc);
		};
		pages->items.push_back(awst::makeBoxExtract(boxKey(),
			awst::makeIntegerConstant("0", m_loc), page0Len(), m_loc));
		pages->items.push_back(awst::makeBoxExtract(boxKey(),
			page0Len(),
			awst::makeUInt64BinOp(lenU64(),
				awst::UInt64BinaryOperator::Sub, page0Len(), m_loc),
			m_loc));
	}
	else
		for (int page = 0; page < 2; ++page)
			pages->items.push_back(awst::makeTemplateVar(
				"TMPL_APPROVAL_" + _childName + "_P" + std::to_string(page),
				awst::WType::bytesType(), m_loc));
	pages->wtype = m_ctx.typeMapper.createType<awst::WTuple>(
		std::vector<awst::WType const*>{
			awst::WType::bytesType(), awst::WType::bytesType()},
		std::nullopt);
	return pages;
}

std::shared_ptr<awst::TupleExpression> SolNewExpression::buildChildArgs(
	ConstructorWirePlan const& wire,
	std::vector<std::shared_ptr<awst::Expression>> values, bool postInit)
{
	auto args = awst::makeTupleExpression(nullptr, m_loc);
	if (postInit)
		args->items.push_back(awst::makeMethodConstant(wire.postInitSignature(), awst::WType::bytesType(), m_loc));
	if (!postInit && m_ctx.typeMapper.profile().contractAbi == ContractAbi::Evm && !values.empty())
	{
		std::vector<Type const*> types;
		for (auto const& parameter: wire.parameters) types.push_back(parameter.declaration->type());
		args->items.push_back(m_ctx.emitSequencedOperand({},
			eb::AbiEncoderBuilder::encodeValuesAsEvmAbi(m_ctx, types, std::move(values), m_loc), true, m_loc));
	}
	else
		for (size_t i = 0; i < values.size(); ++i)
			args->items.push_back(m_ctx.emitSequencedOperand({},
				wire.encode(i, std::move(values[i]), m_loc), true, m_loc));
	if (args->items.empty()) return nullptr;
	std::vector<awst::WType const*> types;
	for (auto const& value: args->items) types.push_back(value->wtype);
	args->wtype = m_ctx.typeMapper.createType<awst::WTuple>(std::move(types));
	return args;
}

void SolNewExpression::emitChildFunding(
	std::shared_ptr<awst::Expression> const& _createdAppId,
	std::shared_ptr<awst::Expression> ctorValueForFund)
{
	// Fund the newly created app's proven native escrow.
	// MBR (1M) + value ONLY when no __postInit: with postInit, value
	// travels in the [pay(value),__postInit] group (gtxns Amount GI-1).
	// Bundling here too → 2x value (MBR+2*500000 verified). A pay txn
	// always transfers Amount — no "only-sets-msg.value" mode.
	// [pay,create,postInit] impossible: child's addr/app-id unknown until create.
	// Slot mode: the child's state lives in boxes (page box ≈0.83
	// ALGO, sparse ≈0.029 each) — 1 ALGO starves any child that
	// stores an aggregate in its ctor. 10 ALGO covers a page + ~300
	// sparse slots.
	auto baseMbr = awst::makeIntegerConstant(
		m_ctx.typeMapper.profile().evmStorageLayout ? "4000000" : "1000000", m_loc);
	std::shared_ptr<awst::Expression> totalFundAmount;
	if (ctorValueForFund)
	{
		totalFundAmount = awst::makeUInt64BinOp(
			std::move(baseMbr), awst::UInt64BinaryOperator::Add,
			std::move(ctorValueForFund), m_loc);
	}
	else
	{
		totalFundAmount = std::move(baseMbr);
	}
	auto fundCreate = buildNativePayment(m_ctx.typeMapper.profile(), m_ctx.preEffects(),
		awst::makeAsApplication(_createdAppId, m_loc), std::move(totalFundAmount), m_loc);

	static awst::WInnerTransaction s_fundTxnType(1);
	auto fundSubmit = awst::makeSubmitInnerTransaction(&s_fundTxnType, m_loc);
	fundSubmit->itxns.push_back(std::move(fundCreate));

	auto fundStmt = awst::makeExpressionStatement(std::move(fundSubmit), m_loc);
	m_ctx.preEffects().push_back(std::move(fundStmt));
}

void SolNewExpression::emitChildPostInit(
	std::shared_ptr<awst::Expression> postAppId,
	std::shared_ptr<awst::Expression> argsTuple,
	std::shared_ptr<awst::Expression> callValue)
{
	if (!callValue) callValue = awst::makeZero(m_loc);

	// PaymentTxn (sets msg.value for __postInit)
	auto payTxn = buildNativePayment(m_ctx.typeMapper.profile(), m_ctx.preEffects(),
		awst::makeAsApplication(postAppId, m_loc), std::move(callValue), m_loc);

	// AppCall __postInit(args)
	static awst::WInnerTransactionFields s_applFieldsType2(6);
	auto postCall = awst::makeCreateInnerTransaction(&s_applFieldsType2, m_loc);
	postCall->fields["TypeEnum"] = awst::makeIntegerConstant("6", m_loc);
	postCall->fields["OnCompletion"] = awst::makeZero(m_loc);
	postCall->fields["Fee"] = awst::makeZero(m_loc);
	postCall->fields["ApplicationID"] = std::move(postAppId);
	postCall->fields["ApplicationArgs"] = std::move(argsTuple);

	// Group: PaymentTxn must be visible to __postInit's msg.value.
	static awst::WInnerTransaction s_payApplGroupType(1);
	auto postSubmit = awst::makeSubmitInnerTransaction(&s_payApplGroupType, m_loc);
	postSubmit->itxns.push_back(std::move(payTxn));
	postSubmit->itxns.push_back(std::move(postCall));

	auto postStmt = awst::makeExpressionStatement(std::move(postSubmit), m_loc);
	m_ctx.preEffects().push_back(std::move(postStmt));
}


} // namespace puyasol::builder::sol_ast

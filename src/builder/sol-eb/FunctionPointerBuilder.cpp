/// @file FunctionPointerBuilder.cpp
/// Implements function pointer support — dispatch tables for internal,
/// inner app calls for external.

#include "builder/sol-eb/FunctionPointerBuilder.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

namespace puyasol::builder::eb
{

using namespace solidity::frontend;

// Static members
std::map<int64_t, FuncPtrEntry> FunctionPointerBuilder::s_targets;
unsigned FunctionPointerBuilder::s_nextId = 1; // 0 = zero-initialized/invalid
std::map<std::string, solidity::frontend::FunctionType const*> FunctionPointerBuilder::s_neededDispatches;
std::string FunctionPointerBuilder::s_currentCref;

void FunctionPointerBuilder::reset()
{
	s_targets.clear();
	s_nextId = 1;
	s_neededDispatches.clear();
	s_currentCref.clear();
}

// ── Type mapping ──

awst::WType const* FunctionPointerBuilder::mapFunctionType(
	FunctionType const* _funcType)
{
	if (!_funcType)
		return awst::WType::uint64Type();

	if (_funcType->kind() == FunctionType::Kind::External
		|| _funcType->kind() == FunctionType::Kind::DelegateCall)
	{
		// 12 bytes: itob(appId) 8 + selector 4
		static awst::BytesWType s_extFnPtrType(12);
		return &s_extFnPtrType;
	}

	// Internal function pointers: uint64 ID
	return awst::WType::uint64Type();
}

// ── Register a function as a pointer target ──

void FunctionPointerBuilder::registerTarget(
	FunctionDefinition const* _funcDef,
	FunctionType const* _funcType,
	std::string _awstName)
{
	if (!_funcDef) return;
	int64_t id = _funcDef->id();
	if (s_targets.count(id)) return; // already registered

	std::string name = _awstName.empty() ? _funcDef->name() : std::move(_awstName);
	s_targets[id] = FuncPtrEntry{
		id,
		name,
		s_nextId++,
		_funcType,
		_funcDef,
		"" // subroutineId — populated later via setSubroutineId
	};
}

void FunctionPointerBuilder::setSubroutineIds(
	std::unordered_map<int64_t, std::string> const& _idMap)
{
	for (auto& [astId, entry] : s_targets)
	{
		auto it = _idMap.find(astId);
		if (it != _idMap.end())
			entry.subroutineId = it->second;
	}
}

// ── Build a reference to a function (taking its "address") ──

std::shared_ptr<awst::Expression> FunctionPointerBuilder::buildFunctionReference(
	BuilderContext& _ctx,
	FunctionDefinition const* _funcDef,
	awst::SourceLocation const& _loc,
	FunctionType const* _callerFuncType)
{
	if (!_funcDef)
	{
		// Zero-initialized function pointer
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = awst::WType::uint64Type();
		zero->value = "0";
		return zero;
	}

	// Use caller-provided FunctionType if available (determines
	// Internal vs External when both exist, e.g., `this.g` is External
	// even though g also has an Internal overload).
	auto const* funcType = _callerFuncType;
	if (!funcType)
	{
		funcType = _funcDef->functionType(true); // internal
		if (!funcType)
			funcType = _funcDef->functionType(false); // external
	}

	// Register as target
	registerTarget(_funcDef, funcType);

	bool isExternal = funcType && (funcType->kind() == FunctionType::Kind::External
		|| funcType->kind() == FunctionType::Kind::DelegateCall);

	if (isExternal)
	{
		// External function pointer = concat(itob(appId), selector).
		// `this.f` → (CurrentApplicationID, f.selector).
		// The 12-byte representation (8 appId + 4 selector) is stored as
		// raw bytes. Calling it extracts appId + selector for an inner txn.

		// Compute ARC4 selector: sha512_256("f(...)") first 4 bytes.
		// Use the function name + ABI param types.
		std::string abiSig = _funcDef->name() + "(";
		bool first = true;
		for (auto const& p : _funcDef->parameters())
		{
			if (!first) abiSig += ",";
			// Simplified: use Solidity canonical type names
			abiSig += p->type()->canonicalName();
			first = false;
		}
		abiSig += ")";

		// For now, use keccak256-based selector (EVM convention)
		// but our ARC4 routing uses sha512_256. Since this.f targets
		// our own contract, we need the ARC4 selector. Encode the
		// method signature and hash.
		auto sigConst = std::make_shared<awst::BytesConstant>();
		sigConst->sourceLocation = _loc;
		sigConst->wtype = awst::WType::bytesType();
		sigConst->encoding = awst::BytesEncoding::Utf8;
		sigConst->value = std::vector<uint8_t>(abiSig.begin(), abiSig.end());

		// sha512_256(sig)[:4] for ARC4 selector
		auto hash = std::make_shared<awst::IntrinsicCall>();
		hash->sourceLocation = _loc;
		hash->wtype = awst::WType::bytesType();
		hash->opCode = "sha512_256";
		hash->stackArgs.push_back(std::move(sigConst));

		auto four = std::make_shared<awst::IntegerConstant>();
		four->sourceLocation = _loc;
		four->wtype = awst::WType::uint64Type();
		four->value = "4";
		auto zero2 = std::make_shared<awst::IntegerConstant>();
		zero2->sourceLocation = _loc;
		zero2->wtype = awst::WType::uint64Type();
		zero2->value = "0";

		auto selector = std::make_shared<awst::IntrinsicCall>();
		selector->sourceLocation = _loc;
		selector->wtype = awst::WType::bytesType();
		selector->opCode = "extract3";
		selector->stackArgs.push_back(std::move(hash));
		selector->stackArgs.push_back(std::move(zero2));
		selector->stackArgs.push_back(std::move(four));

		// itob(CurrentApplicationID)
		auto appId = std::make_shared<awst::IntrinsicCall>();
		appId->sourceLocation = _loc;
		appId->wtype = awst::WType::uint64Type();
		appId->opCode = "global";
		appId->immediates = {std::string("CurrentApplicationID")};

		auto appIdBytes = std::make_shared<awst::IntrinsicCall>();
		appIdBytes->sourceLocation = _loc;
		appIdBytes->wtype = awst::WType::bytesType();
		appIdBytes->opCode = "itob";
		appIdBytes->stackArgs.push_back(std::move(appId));

		// concat(itob(appId), selector) → 12 bytes
		static awst::BytesWType s_bytes12(12);
		auto packed = std::make_shared<awst::IntrinsicCall>();
		packed->sourceLocation = _loc;
		packed->wtype = &s_bytes12;
		packed->opCode = "concat";
		packed->stackArgs.push_back(std::move(appIdBytes));
		packed->stackArgs.push_back(std::move(selector));
		return packed;
	}

	// Internal: return the function's unique ID
	auto it = s_targets.find(_funcDef->id());
	unsigned funcId = (it != s_targets.end()) ? it->second.id : 0;

	auto idConst = std::make_shared<awst::IntegerConstant>();
	idConst->sourceLocation = _loc;
	idConst->wtype = awst::WType::uint64Type();
	idConst->value = std::to_string(funcId);
	return idConst;
}

// ── Build a call through a function pointer ──

std::shared_ptr<awst::Expression> FunctionPointerBuilder::buildFunctionPointerCall(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> _ptrExpr,
	FunctionType const* _funcType,
	std::vector<std::shared_ptr<awst::Expression>> _args,
	awst::SourceLocation const& _loc)
{
	if (!_funcType)
		return nullptr;

	bool isExternal = (_funcType->kind() == FunctionType::Kind::External
		|| _funcType->kind() == FunctionType::Kind::DelegateCall);

	if (isExternal)
	{
		// External function pointer call: extract appId + selector from
		// the 12-byte packed representation, then emit an inner app call.
		// ptrExpr = concat(itob(appId), selector) = 12 bytes

		// Extract appId: btoi(extract(_ptr, 0, 8))
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = awst::WType::uint64Type();
		zero->value = "0";
		auto eight = std::make_shared<awst::IntegerConstant>();
		eight->sourceLocation = _loc;
		eight->wtype = awst::WType::uint64Type();
		eight->value = "8";

		auto extractAppId = std::make_shared<awst::IntrinsicCall>();
		extractAppId->sourceLocation = _loc;
		extractAppId->wtype = awst::WType::bytesType();
		extractAppId->opCode = "extract3";
		extractAppId->stackArgs.push_back(_ptrExpr);
		extractAppId->stackArgs.push_back(std::move(zero));
		extractAppId->stackArgs.push_back(std::move(eight));

		auto appId = std::make_shared<awst::IntrinsicCall>();
		appId->sourceLocation = _loc;
		appId->wtype = awst::WType::uint64Type();
		appId->opCode = "btoi";
		appId->stackArgs.push_back(std::move(extractAppId));

		// Extract selector: extract(_ptr, 8, 4)
		auto eight2 = std::make_shared<awst::IntegerConstant>();
		eight2->sourceLocation = _loc;
		eight2->wtype = awst::WType::uint64Type();
		eight2->value = "8";
		auto four = std::make_shared<awst::IntegerConstant>();
		four->sourceLocation = _loc;
		four->wtype = awst::WType::uint64Type();
		four->value = "4";

		auto extractSel = std::make_shared<awst::IntrinsicCall>();
		extractSel->sourceLocation = _loc;
		extractSel->wtype = awst::WType::bytesType();
		extractSel->opCode = "extract3";
		extractSel->stackArgs.push_back(_ptrExpr);
		extractSel->stackArgs.push_back(std::move(eight2));
		extractSel->stackArgs.push_back(std::move(four));

		// Build ApplicationArgs tuple: [selector, arg1_32bytes, ...]
		// Each arg is a separate tuple element (matching puya's inner txn layout).
		auto argsTuple = std::make_shared<awst::TupleExpression>();
		argsTuple->sourceLocation = _loc;
		// wtype will be set after all items are added
		argsTuple->items.push_back(std::move(extractSel));

		for (auto& arg : _args)
		{
			// Promote to biguint, reinterpret as bytes, pad to 32
			auto coerced = builder::TypeCoercion::implicitNumericCast(
				std::move(arg), awst::WType::biguintType(), _loc);
			auto toBytes = std::make_shared<awst::ReinterpretCast>();
			toBytes->sourceLocation = _loc;
			toBytes->wtype = awst::WType::bytesType();
			toBytes->expr = std::move(coerced);

			// Left-pad to 32 bytes: concat(bzero(32), bytes) then last 32
			auto pad = std::make_shared<awst::IntrinsicCall>();
			pad->sourceLocation = _loc;
			pad->wtype = awst::WType::bytesType();
			pad->opCode = "bzero";
			auto n32 = std::make_shared<awst::IntegerConstant>();
			n32->sourceLocation = _loc;
			n32->wtype = awst::WType::uint64Type();
			n32->value = "32";
			pad->stackArgs.push_back(std::move(n32));
			auto padded = std::make_shared<awst::IntrinsicCall>();
			padded->sourceLocation = _loc;
			padded->wtype = awst::WType::bytesType();
			padded->opCode = "concat";
			padded->stackArgs.push_back(std::move(pad));
			padded->stackArgs.push_back(std::move(toBytes));

			auto lenExpr = std::make_shared<awst::IntrinsicCall>();
			lenExpr->sourceLocation = _loc;
			lenExpr->wtype = awst::WType::uint64Type();
			lenExpr->opCode = "len";
			lenExpr->stackArgs.push_back(padded);
			auto n32b = std::make_shared<awst::IntegerConstant>();
			n32b->sourceLocation = _loc;
			n32b->wtype = awst::WType::uint64Type();
			n32b->value = "32";
			auto off = std::make_shared<awst::UInt64BinaryOperation>();
			off->sourceLocation = _loc;
			off->wtype = awst::WType::uint64Type();
			off->left = std::move(lenExpr);
			off->op = awst::UInt64BinaryOperator::Sub;
			off->right = std::move(n32b);
			auto n32c = std::make_shared<awst::IntegerConstant>();
			n32c->sourceLocation = _loc;
			n32c->wtype = awst::WType::uint64Type();
			n32c->value = "32";
			auto last32 = std::make_shared<awst::IntrinsicCall>();
			last32->sourceLocation = _loc;
			last32->wtype = awst::WType::bytesType();
			last32->opCode = "extract3";
			last32->stackArgs.push_back(std::move(padded));
			last32->stackArgs.push_back(std::move(off));
			last32->stackArgs.push_back(std::move(n32c));

			argsTuple->items.push_back(std::move(last32));
		}

		// Set WTuple type for the args tuple
		{
			std::vector<awst::WType const*> itemTypes;
			for (auto const& item : argsTuple->items)
				itemTypes.push_back(item->wtype ? item->wtype : awst::WType::bytesType());
			argsTuple->wtype = new awst::WTuple(std::move(itemTypes));
		}

		// CreateInnerTransaction
		static awst::WInnerTransactionFields s_applFields(6);
		auto create = std::make_shared<awst::CreateInnerTransaction>();
		create->sourceLocation = _loc;
		create->wtype = &s_applFields;
		auto makeU64 = [&](std::string val) {
			auto c = std::make_shared<awst::IntegerConstant>();
			c->sourceLocation = _loc;
			c->wtype = awst::WType::uint64Type();
			c->value = std::move(val);
			return c;
		};
		create->fields["TypeEnum"] = makeU64("6");
		create->fields["Fee"] = makeU64("0");
		create->fields["ApplicationID"] = std::move(appId);
		create->fields["OnCompletion"] = makeU64("0");
		create->fields["ApplicationArgs"] = std::move(argsTuple);

		// SubmitInnerTransaction
		static awst::WInnerTransaction s_applTxn(6);
		auto submit = std::make_shared<awst::SubmitInnerTransaction>();
		submit->sourceLocation = _loc;
		submit->wtype = &s_applTxn;
		submit->itxns.push_back(std::move(create));

		auto submitStmt = std::make_shared<awst::ExpressionStatement>();
		submitStmt->sourceLocation = _loc;
		submitStmt->expr = std::move(submit);
		_ctx.prePendingStatements.push_back(std::move(submitStmt));

		// Read return value from itxn LastLog
		awst::WType const* retType = awst::WType::voidType();
		if (!_funcType->returnParameterTypes().empty())
			retType = _ctx.typeMapper.map(_funcType->returnParameterTypes()[0]);

		if (retType == awst::WType::voidType())
		{
			auto vc = std::make_shared<awst::VoidConstant>();
			vc->sourceLocation = _loc;
			vc->wtype = awst::WType::voidType();
			return vc;
		}

		// itxn LastLog → strip 4-byte ARC4 prefix → decode
		auto readLog = std::make_shared<awst::IntrinsicCall>();
		readLog->sourceLocation = _loc;
		readLog->wtype = awst::WType::bytesType();
		readLog->opCode = "itxn";
		readLog->immediates = {std::string("LastLog")};

		auto stripPrefix = std::make_shared<awst::IntrinsicCall>();
		stripPrefix->sourceLocation = _loc;
		stripPrefix->wtype = awst::WType::bytesType();
		stripPrefix->opCode = "extract";
		stripPrefix->immediates = {4, 0};
		stripPrefix->stackArgs.push_back(std::move(readLog));

		// Reinterpret as biguint for uint256 returns
		if (retType == awst::WType::biguintType())
		{
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(stripPrefix);
			return cast;
		}
		if (retType == awst::WType::uint64Type())
		{
			auto btoi = std::make_shared<awst::IntrinsicCall>();
			btoi->sourceLocation = _loc;
			btoi->wtype = awst::WType::uint64Type();
			btoi->opCode = "btoi";
			btoi->stackArgs.push_back(std::move(stripPrefix));
			return btoi;
		}
		return stripPrefix;
	}

	// Internal: call __funcptr_dispatch_<signature>(id, args...)
	std::string dname = dispatchName(_funcType);
	// Track that this dispatch signature is needed (even if no targets registered)
	s_neededDispatches[dname] = _funcType;

	// Determine return type
	awst::WType const* retType = awst::WType::voidType();
	if (!_funcType->returnParameterTypes().empty())
	{
		if (_funcType->returnParameterTypes().size() == 1)
			retType = _ctx.typeMapper.map(_funcType->returnParameterTypes()[0]);
		else
		{
			std::vector<awst::WType const*> retTypes;
			for (auto const* rt : _funcType->returnParameterTypes())
				retTypes.push_back(_ctx.typeMapper.map(rt));
			retType = _ctx.typeMapper.createType<awst::WTuple>(std::move(retTypes), std::nullopt);
		}
	}

	auto call = std::make_shared<awst::SubroutineCallExpression>();
	call->sourceLocation = _loc;
	call->wtype = retType;
	// Use SubroutineID from library context (where InstanceMethodTarget
	// fails with "invocation outside of a contract method"). From contract
	// context, InstanceMethodTarget resolves correctly.
	bool inLibraryContext = !_ctx.contractName.empty()
		&& !s_currentCref.empty()
		&& s_currentCref.find("." + _ctx.contractName) == std::string::npos;
	if (inLibraryContext)
		call->target = awst::SubroutineID{s_currentCref + "." + dname};
	else
		call->target = awst::InstanceMethodTarget{dname};

	// First arg: the pointer ID
	awst::CallArg idArg;
	idArg.name = "__funcptr_id";
	idArg.value = std::move(_ptrExpr);
	call->args.push_back(std::move(idArg));

	// Remaining args: coerce to match dispatch parameter types
	for (size_t i = 0; i < _args.size(); ++i)
	{
		awst::CallArg arg;
		arg.name = "__arg" + std::to_string(i);
		arg.value = std::move(_args[i]);
		// Coerce arg type to match dispatch parameter type
		if (i < _funcType->parameterTypes().size())
		{
			auto const* paramSolType = _funcType->parameterTypes()[i];
			awst::WType const* expectedType = _ctx.typeMapper.map(paramSolType);
			if (arg.value->wtype != expectedType)
				arg.value = builder::TypeCoercion::implicitNumericCast(
					std::move(arg.value), expectedType, _loc);
		}
		call->args.push_back(std::move(arg));
	}

	return call;
}

// ── Dispatch name from function type signature ──

std::string FunctionPointerBuilder::dispatchName(
	FunctionType const* _funcType)
{
	// Build a name based on param and return types
	std::string name = "__funcptr_dispatch";
	if (_funcType)
	{
		for (auto const* pt : _funcType->parameterTypes())
		{
			if (auto const* intType = dynamic_cast<IntegerType const*>(pt))
				name += "_u" + std::to_string(intType->numBits());
			else if (dynamic_cast<BoolType const*>(pt))
				name += "_bool";
			else
				name += "_x";
		}
		name += "_ret";
		for (auto const* rt : _funcType->returnParameterTypes())
		{
			if (auto const* intType = dynamic_cast<IntegerType const*>(rt))
				name += "_u" + std::to_string(intType->numBits());
			else if (dynamic_cast<BoolType const*>(rt))
				name += "_bool";
			else
				name += "_x";
		}
	}
	return name;
}

// ── Generate dispatch subroutines ──

std::vector<awst::ContractMethod> FunctionPointerBuilder::generateDispatchMethods(
	std::string const& _cref,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Subroutine>>* _outRootSubs)
{
	std::vector<awst::ContractMethod> methods;

	if (s_targets.empty() && s_neededDispatches.empty())
		return methods;

	// Group targets by dispatch name (= signature)
	std::map<std::string, std::vector<FuncPtrEntry const*>> groups;
	for (auto const& [astId, entry] : s_targets)
	{
		std::string dname = dispatchName(entry.funcType);
		groups[dname].push_back(&entry);
	}
	// Ensure all needed dispatch signatures have entries (even if empty)
	for (auto const& [dname, funcType] : s_neededDispatches)
	{
		if (groups.find(dname) == groups.end())
			groups[dname] = {};
	}

	for (auto const& [dname, entries] : groups)
	{
		// Get the function type from entries or from s_neededDispatches
		FunctionType const* funcType = nullptr;
		if (!entries.empty())
			funcType = entries[0]->funcType;
		else if (s_neededDispatches.count(dname))
			funcType = s_neededDispatches.at(dname);
		if (!funcType) continue;

		awst::ContractMethod dispatch;
		dispatch.sourceLocation = _loc;
		dispatch.cref = _cref;
		dispatch.memberName = dname;
		dispatch.arc4MethodConfig = std::nullopt;
		dispatch.pure = false;

		// Return type — must match what buildFunction produces for the target.
		// Signed integers ≤64 bits get promoted to biguint for ABI sign extension.
		if (funcType->returnParameterTypes().empty())
			dispatch.returnType = awst::WType::voidType();
		else if (funcType->returnParameterTypes().size() == 1)
		{
			auto const* retSolType = funcType->returnParameterTypes()[0];
			if (auto const* intType = dynamic_cast<IntegerType const*>(retSolType))
			{
				if (intType->numBits() <= 64 && intType->isSigned())
					dispatch.returnType = awst::WType::biguintType(); // sign-extended
				else if (intType->numBits() <= 64)
					dispatch.returnType = awst::WType::uint64Type();
				else
					dispatch.returnType = awst::WType::biguintType();
			}
			else if (dynamic_cast<BoolType const*>(retSolType))
				dispatch.returnType = awst::WType::boolType();
			else
				dispatch.returnType = awst::WType::biguintType();
		}
		else
			dispatch.returnType = awst::WType::voidType(); // TODO: tuple returns

		// Args: __funcptr_id, then the function params
		{
			awst::SubroutineArgument idArg;
			idArg.name = "__funcptr_id";
			idArg.wtype = awst::WType::uint64Type();
			idArg.sourceLocation = _loc;
			dispatch.args.push_back(idArg);
		}
		for (size_t i = 0; i < funcType->parameterTypes().size(); ++i)
		{
			awst::SubroutineArgument arg;
			arg.name = "__arg" + std::to_string(i);
			auto const* paramSolType = funcType->parameterTypes()[i];
			if (auto const* intType = dynamic_cast<IntegerType const*>(paramSolType))
				arg.wtype = intType->numBits() <= 64
					? awst::WType::uint64Type() : awst::WType::biguintType();
			else if (dynamic_cast<BoolType const*>(paramSolType))
				arg.wtype = awst::WType::boolType();
			else
				arg.wtype = awst::WType::biguintType();
			arg.sourceLocation = _loc;
			dispatch.args.push_back(arg);
		}

		// Body: switch(__funcptr_id) { case ID1: return func1(args); ... }
		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = _loc;

		// Build if/else chain (innermost = default: assert false)
		auto defaultBlock = std::make_shared<awst::Block>();
		defaultBlock->sourceLocation = _loc;
		{
			// assert(false) — invalid function pointer ID
			auto assertExpr = std::make_shared<awst::AssertExpression>();
			assertExpr->sourceLocation = _loc;
			assertExpr->wtype = awst::WType::voidType();
			auto falseLit = std::make_shared<awst::BoolConstant>();
			falseLit->sourceLocation = _loc;
			falseLit->wtype = awst::WType::boolType();
			falseLit->value = false;
			assertExpr->condition = std::move(falseLit);
			assertExpr->errorMessage = "invalid function pointer";
			auto stmt = std::make_shared<awst::ExpressionStatement>();
			stmt->sourceLocation = _loc;
			stmt->expr = std::move(assertExpr);
			defaultBlock->body.push_back(std::move(stmt));
		}

		std::shared_ptr<awst::Block> elseBlock = defaultBlock;

		for (auto const* entry : entries)
		{
			// Condition: __funcptr_id == entry->id
			auto idVar = std::make_shared<awst::VarExpression>();
			idVar->sourceLocation = _loc;
			idVar->wtype = awst::WType::uint64Type();
			idVar->name = "__funcptr_id";

			auto idConst = std::make_shared<awst::IntegerConstant>();
			idConst->sourceLocation = _loc;
			idConst->wtype = awst::WType::uint64Type();
			idConst->value = std::to_string(entry->id);

			auto cmp = std::make_shared<awst::NumericComparisonExpression>();
			cmp->sourceLocation = _loc;
			cmp->wtype = awst::WType::boolType();
			cmp->lhs = std::move(idVar);
			cmp->op = awst::NumericComparison::Eq;
			cmp->rhs = std::move(idConst);

			// If branch: call the actual function and return result
			auto ifBlock = std::make_shared<awst::Block>();
			ifBlock->sourceLocation = _loc;
			{
				auto call = std::make_shared<awst::SubroutineCallExpression>();
				call->sourceLocation = _loc;
				call->wtype = dispatch.returnType;
				if (!entry->subroutineId.empty())
					call->target = awst::SubroutineID{entry->subroutineId};
				else
					call->target = awst::InstanceMethodTarget{entry->name};

				// Check if target is public (has ARC4 wrapping)
				bool isPublic = entry->funcDef && (
					entry->funcDef->visibility() == Visibility::Public
					|| entry->funcDef->visibility() == Visibility::External);

				for (size_t i = 0; i < funcType->parameterTypes().size(); ++i)
				{
					awst::CallArg arg;
					auto var = std::make_shared<awst::VarExpression>();
					var->sourceLocation = _loc;
					var->name = "__arg" + std::to_string(i);
					var->wtype = dispatch.args[i + 1].wtype;

					// Get the actual parameter name from the target function
					std::string paramName = "__arg" + std::to_string(i);
					if (entry->funcDef && i < entry->funcDef->parameters().size())
						paramName = entry->funcDef->parameters()[i]->name();

					if (isPublic && var->wtype == awst::WType::biguintType())
					{
						// Public target: wrap biguint → ARC4UIntN for ARC4 params
						auto const* paramSolType = funcType->parameterTypes()[i];
						unsigned bits = 256;
						if (auto const* intType = dynamic_cast<IntegerType const*>(paramSolType))
							bits = intType->numBits();
						auto arc4Type = new awst::ARC4UIntN(static_cast<int>(bits));

						auto encode = std::make_shared<awst::ARC4Encode>();
						encode->sourceLocation = _loc;
						encode->wtype = arc4Type;
						encode->value = std::move(var);

						arg.name = "__arc4_" + paramName;
						arg.value = std::move(encode);
					}
					else
					{
						arg.name = paramName;
						arg.value = std::move(var);
					}
					call->args.push_back(std::move(arg));
				}

				// For public targets with ARC4 return, decode the result
				if (isPublic && dispatch.returnType == awst::WType::biguintType())
				{
					call->wtype = new awst::ARC4UIntN(256); // arc4.uint256
				}

				if (dispatch.returnType != awst::WType::voidType())
				{
					std::shared_ptr<awst::Expression> retValue = std::move(call);
					// If target is public and returns ARC4, decode back to biguint
					if (isPublic && retValue->wtype != dispatch.returnType)
					{
						auto decode = std::make_shared<awst::ARC4Decode>();
						decode->sourceLocation = _loc;
						decode->wtype = dispatch.returnType;
						decode->value = std::move(retValue);
						retValue = std::move(decode);
					}
					auto ret = std::make_shared<awst::ReturnStatement>();
					ret->sourceLocation = _loc;
					ret->value = std::move(retValue);
					ifBlock->body.push_back(std::move(ret));
				}
				else
				{
					auto stmt = std::make_shared<awst::ExpressionStatement>();
					stmt->sourceLocation = _loc;
					stmt->expr = std::move(call);
					ifBlock->body.push_back(std::move(stmt));
					auto ret = std::make_shared<awst::ReturnStatement>();
					ret->sourceLocation = _loc;
					ifBlock->body.push_back(std::move(ret));
				}
			}

			auto ifElse = std::make_shared<awst::IfElse>();
			ifElse->sourceLocation = _loc;
			ifElse->condition = std::move(cmp);
			ifElse->ifBranch = std::move(ifBlock);
			ifElse->elseBranch = std::move(elseBlock);

			auto newElse = std::make_shared<awst::Block>();
			newElse->sourceLocation = _loc;
			newElse->body.push_back(std::move(ifElse));
			elseBlock = std::move(newElse);
		}

		for (auto& stmt : elseBlock->body)
			body->body.push_back(std::move(stmt));

		dispatch.body = body;

		// Also emit as a root-level Subroutine so library subroutines can
		// resolve the dispatch via SubroutineID (puya can't resolve
		// InstanceMethodTarget from outside the contract scope).
		if (_outRootSubs)
		{
			auto sub = std::make_shared<awst::Subroutine>();
			sub->sourceLocation = dispatch.sourceLocation;
			sub->id = _cref + "." + dispatch.memberName;
			sub->name = dispatch.memberName;
			sub->returnType = dispatch.returnType;
			sub->args = dispatch.args;
			sub->body = dispatch.body; // shared ptr — same body
			sub->pure = dispatch.pure;
			_outRootSubs->push_back(std::move(sub));
		}

		methods.push_back(std::move(dispatch));
	}

	return methods;
}

} // namespace puyasol::builder::eb

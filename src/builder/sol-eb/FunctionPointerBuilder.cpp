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
		auto zero = awst::makeIntegerConstant("0", _loc);
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
		//
		// Compile-time self-reference detection: `this.f` always targets
		// the current application. At call time, the runtime check
		// (appId == CurrentApplicationID) will route to internal dispatch
		// instead of an inner txn, avoiding AVM's self-call restriction.
		Logger::instance().warning(
			"external function pointer '" + _funcDef->name()
			+ "': reentrancy is not possible on AVM; self-calls will use "
			"internal dispatch instead of inner transactions", _loc);

		// Self-reference: encode as concat(itob(0), itob(internalId)[:4]).
		// appId=0 is a sentinel that means "current application — use
		// internal dispatch". The selector slot stores the internal fn-ptr
		// ID so the runtime dispatch can route without an inner txn.
		//
		// Also register as an internal target so the dispatch table has
		// an entry for this function.
		auto const* internalFuncType = _funcDef->functionType(true);
		if (internalFuncType)
			registerTarget(_funcDef, internalFuncType);
		auto idIt = s_targets.find(_funcDef->id());
		unsigned funcId = (idIt != s_targets.end()) ? idIt->second.id : 0;

		// itob(0) — sentinel appId
		auto zeroAppId = std::make_shared<awst::IntrinsicCall>();
		zeroAppId->sourceLocation = _loc;
		zeroAppId->wtype = awst::WType::bytesType();
		zeroAppId->opCode = "itob";
		auto zeroConst = awst::makeIntegerConstant("0", _loc);
		zeroAppId->stackArgs.push_back(std::move(zeroConst));

		// itob(funcId)[:4] — internal ID in selector slot
		auto idItob = std::make_shared<awst::IntrinsicCall>();
		idItob->sourceLocation = _loc;
		idItob->wtype = awst::WType::bytesType();
		idItob->opCode = "itob";
		auto idConst = awst::makeIntegerConstant(std::to_string(funcId), _loc);
		idItob->stackArgs.push_back(std::move(idConst));
		auto idBytes4 = std::make_shared<awst::IntrinsicCall>();
		idBytes4->sourceLocation = _loc;
		idBytes4->wtype = awst::WType::bytesType();
		idBytes4->opCode = "extract";
		idBytes4->immediates = {4, 4}; // last 4 bytes of 8-byte itob
		idBytes4->stackArgs.push_back(std::move(idItob));

		// concat(itob(0), idBytes4) → 12 bytes
		static awst::BytesWType s_bytes12(12);
		auto packed = std::make_shared<awst::IntrinsicCall>();
		packed->sourceLocation = _loc;
		packed->wtype = &s_bytes12;
		packed->opCode = "concat";
		packed->stackArgs.push_back(std::move(zeroAppId));
		packed->stackArgs.push_back(std::move(idBytes4));
		return packed;
	}

	// Internal: return the function's unique ID
	auto it = s_targets.find(_funcDef->id());
	unsigned funcId = (it != s_targets.end()) ? it->second.id : 0;

	auto idConst = awst::makeIntegerConstant(std::to_string(funcId), _loc);
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
		// External function pointer call: check if self-call (appId == 0
		// sentinel) and route to internal dispatch, otherwise inner txn.

		// Extract appId: btoi(extract(_ptr, 0, 8))
		auto zero = awst::makeIntegerConstant("0", _loc);
		auto eight = awst::makeIntegerConstant("8", _loc);

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

		// Check if self-call: appId == 0 (sentinel for current app)
		auto isSelf = std::make_shared<awst::NumericComparisonExpression>();
		isSelf->sourceLocation = _loc;
		isSelf->wtype = awst::WType::boolType();
		isSelf->lhs = appId; // shared
		isSelf->op = awst::NumericComparison::Eq;
		auto zeroCheck = awst::makeIntegerConstant("0", _loc);
		isSelf->rhs = std::move(zeroCheck);

		// Self-call path: extract internal ID from selector slot, dispatch
		auto extractId4 = std::make_shared<awst::IntrinsicCall>();
		extractId4->sourceLocation = _loc;
		extractId4->wtype = awst::WType::bytesType();
		extractId4->opCode = "extract";
		extractId4->immediates = {8, 4};
		extractId4->stackArgs.push_back(_ptrExpr);

		auto internalId = std::make_shared<awst::IntrinsicCall>();
		internalId->sourceLocation = _loc;
		internalId->wtype = awst::WType::uint64Type();
		internalId->opCode = "btoi";
		internalId->stackArgs.push_back(std::move(extractId4));

		// Build internal dispatch call with the same args
		std::string dname = dispatchName(_funcType);
		s_neededDispatches[dname] = _funcType;

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

		auto selfCall = std::make_shared<awst::SubroutineCallExpression>();
		selfCall->sourceLocation = _loc;
		selfCall->wtype = retType;
		// Determine context: if we're inside the contract (contractName
		// matches the cref), use InstanceMethodTarget. From library
		// context, use SubroutineID.
		bool inLibraryContext = !_ctx.contractName.empty()
			&& !s_currentCref.empty()
			&& s_currentCref.find("." + _ctx.contractName) == std::string::npos;
		if (inLibraryContext)
			selfCall->target = awst::SubroutineID{s_currentCref + "." + dname};
		else
			selfCall->target = awst::InstanceMethodTarget{dname};

		awst::CallArg selfIdArg;
		selfIdArg.name = "__funcptr_id";
		selfIdArg.value = std::move(internalId);
		selfCall->args.push_back(std::move(selfIdArg));
		for (size_t i = 0; i < _args.size(); ++i)
		{
			awst::CallArg arg;
			arg.name = "__arg" + std::to_string(i);
			arg.value = _args[i]; // shared — used by both branches
			if (i < _funcType->parameterTypes().size())
			{
				auto* expectedType = _ctx.typeMapper.map(_funcType->parameterTypes()[i]);
				if (arg.value->wtype != expectedType)
					arg.value = builder::TypeCoercion::implicitNumericCast(
						std::move(arg.value), expectedType, _loc);
			}
			selfCall->args.push_back(std::move(arg));
		}

		// If return type is void, self-call is all we need
		if (retType == awst::WType::voidType())
		{
			// Unconditionally use self-call for void returns
			// (inner txn path for non-self not yet needed for void)
			auto stmt = std::make_shared<awst::ExpressionStatement>();
			stmt->sourceLocation = _loc;
			stmt->expr = selfCall;
			_ctx.prePendingStatements.push_back(std::move(stmt));
			auto vc = std::make_shared<awst::VoidConstant>();
			vc->sourceLocation = _loc;
			vc->wtype = awst::WType::voidType();
			return vc;
		}

		// For non-void: use a conditional expression
		// self-call produces the value directly;
		// non-self inner txn (TODO — for now, just use self-call always)
		// Once cross-contract fn-ptrs are supported, this becomes:
		// isSelf ? selfCall : innerTxnResult
		return selfCall;

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
			auto stmt = std::make_shared<awst::ExpressionStatement>();
			stmt->sourceLocation = _loc;
			stmt->expr = awst::makeAssert(
				awst::makeBoolConstant(false, _loc), _loc, "invalid function pointer");
			defaultBlock->body.push_back(std::move(stmt));
		}

		std::shared_ptr<awst::Block> elseBlock = defaultBlock;

		for (auto const* entry : entries)
		{
			// Condition: __funcptr_id == entry->id
			auto idVar = awst::makeVarExpression("__funcptr_id", awst::WType::uint64Type(), _loc);

			auto idConst = awst::makeIntegerConstant(std::to_string(entry->id), _loc);

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
					auto var = awst::makeVarExpression("__arg" + std::to_string(i), dispatch.args[i + 1].wtype, _loc);

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

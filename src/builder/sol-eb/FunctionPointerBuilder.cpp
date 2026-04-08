/// @file FunctionPointerBuilder.cpp
/// Implements function pointer support — dispatch tables for internal,
/// inner app calls for external.

#include "builder/sol-eb/FunctionPointerBuilder.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

namespace puyasol::builder::eb
{

using namespace solidity::frontend;

// Static members
std::map<int64_t, FuncPtrEntry> FunctionPointerBuilder::s_targets;
unsigned FunctionPointerBuilder::s_nextId = 1; // 0 = zero-initialized/invalid

void FunctionPointerBuilder::reset()
{
	s_targets.clear();
	s_nextId = 1;
}

// ── Type mapping ──

awst::WType const* FunctionPointerBuilder::mapFunctionType(
	FunctionType const* _funcType)
{
	if (!_funcType)
		return awst::WType::uint64Type();

	if (_funcType->kind() == FunctionType::Kind::External
		|| _funcType->kind() == FunctionType::Kind::DelegateCall)
		return awst::WType::bytesType(); // address(32) + selector(4) = 36 bytes

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
		_funcDef
	};
}

// ── Build a reference to a function (taking its "address") ──

std::shared_ptr<awst::Expression> FunctionPointerBuilder::buildFunctionReference(
	BuilderContext& _ctx,
	FunctionDefinition const* _funcDef,
	awst::SourceLocation const& _loc)
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

	auto const* funcType = _funcDef->functionType(true); // internal
	if (!funcType)
		funcType = _funcDef->functionType(false); // external

	// Register as target
	registerTarget(_funcDef, funcType);

	bool isExternal = funcType && (funcType->kind() == FunctionType::Kind::External
		|| funcType->kind() == FunctionType::Kind::DelegateCall);

	if (isExternal)
	{
		// External: not yet implemented (would need address + selector)
		Logger::instance().warning(
			"external function pointer reference not fully supported", _loc);
		auto zero = std::make_shared<awst::BytesConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = awst::WType::bytesType();
		zero->encoding = awst::BytesEncoding::Base16;
		zero->value = {};
		return zero;
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
		Logger::instance().warning(
			"calling external function pointer not yet supported", _loc);
		return nullptr;
	}

	// Internal: call __funcptr_dispatch_<signature>(id, args...)
	std::string dname = dispatchName(_funcType);

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
	call->target = awst::InstanceMethodTarget{dname};

	// First arg: the pointer ID
	awst::CallArg idArg;
	idArg.name = "__funcptr_id";
	idArg.value = std::move(_ptrExpr);
	call->args.push_back(std::move(idArg));

	// Remaining args: the actual function arguments
	for (size_t i = 0; i < _args.size(); ++i)
	{
		awst::CallArg arg;
		arg.name = "__arg" + std::to_string(i);
		arg.value = std::move(_args[i]);
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
	awst::SourceLocation const& _loc)
{
	std::vector<awst::ContractMethod> methods;

	if (s_targets.empty())
		return methods;

	// Group targets by dispatch name (= signature)
	std::map<std::string, std::vector<FuncPtrEntry const*>> groups;
	for (auto const& [astId, entry] : s_targets)
	{
		std::string dname = dispatchName(entry.funcType);
		groups[dname].push_back(&entry);
	}

	for (auto const& [dname, entries] : groups)
	{
		if (entries.empty()) continue;
		auto const* funcType = entries[0]->funcType;
		if (!funcType) continue;

		awst::ContractMethod dispatch;
		dispatch.sourceLocation = _loc;
		dispatch.cref = _cref;
		dispatch.memberName = dname;
		dispatch.arc4MethodConfig = std::nullopt;
		dispatch.pure = false;

		// Return type
		if (funcType->returnParameterTypes().empty())
			dispatch.returnType = awst::WType::voidType();
		else if (funcType->returnParameterTypes().size() == 1)
		{
			// Map the return type — use biguint for large ints, uint64 for small
			auto const* retSolType = funcType->returnParameterTypes()[0];
			if (auto const* intType = dynamic_cast<IntegerType const*>(retSolType))
				dispatch.returnType = intType->numBits() <= 64
					? awst::WType::uint64Type() : awst::WType::biguintType();
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
		methods.push_back(std::move(dispatch));
	}

	return methods;
}

} // namespace puyasol::builder::eb

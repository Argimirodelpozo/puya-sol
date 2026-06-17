#include "builder/contract/ContractBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/contract/PostInitTriggers.h"
#include "builder/contract/SelectorRouter.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/sol-ast/calls/SolNewExpression.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/itxn/FunctionPointerBuilder.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

#include <boost/multiprecision/cpp_int.hpp>
#include <map>
#include <set>

namespace puyasol::builder
{

awst::ContractMethod ContractBuilder::buildApprovalProgram(
	solidity::frontend::ContractDefinition const& _contract,
	std::string const& _contractName
)
{
	awst::ContractMethod method;
	method.sourceLocation = makeLoc(_contract.location());
	method.returnType = awst::WType::boolType();
	method.cref = m_sourceFile + "." + _contractName;
	method.memberName = "approval_program";

	auto body = awst::makeBlock(method.sourceLocation);

	// __postInit triggers: box writes, new C(), msg.*, or AVM stdlib calls.
	bool needsPostInit = computeNeedsPostInit(_contract);

	// Create-time check: if (Txn.ApplicationID == 0) { base_ctors; ctor_body; return true; }
	{
		auto appIdCheck = awst::makeTxn(std::string("ApplicationID"), awst::WType::uint64Type(), method.sourceLocation);

		auto zero = awst::makeZero(method.sourceLocation);

		auto isCreate = awst::makeNumericCompare(appIdCheck, awst::NumericComparison::Eq, zero, method.sourceLocation);

		auto createBlock = awst::makeBlock(method.sourceLocation);

		// Emit state variable initialization for one contract level; 'initialized'
		// set prevents re-init when derived contracts shadow a base name.
		std::set<std::string> stateVarInitialized;
		auto emitStateVarInit = [&](solidity::frontend::ContractDefinition const& base,
			std::vector<std::shared_ptr<awst::Statement>>& targetBody)
		{
			for (auto const* var: base.stateVariables())
			{
				if (var->isConstant())
					continue;
				if (stateVarInitialized.count(var->name()))
					continue;
				stateVarInitialized.insert(var->name());

				auto kind = StorageMapper::shouldUseBoxStorage(*var)
					? awst::AppStorageKind::Box
					: awst::AppStorageKind::AppGlobal;

				auto* wtype = m_typeMapper.map(var->type());

				// Box ARC4 struct with explicit initializer: encode + box_put.
				// Dynamic arrays/bytes handled by m_boxArrayVarNames loop; skip here.
				if (kind == awst::AppStorageKind::Box)
				{
					if (!var->value())
						continue;
					bool isStructBox = wtype
						&& wtype->kind() == awst::WTypeKind::ARC4Struct;
					if (!isStructBox)
						continue;
					auto initVal = m_exprBuilder->build(*var->value());
					if (!initVal)
						continue;
					initVal = TypeCoercion::coerceForAssignment(
						std::move(initVal), wtype, method.sourceLocation);
					for (auto& preStmt: m_exprBuilder->takePrePending())
						targetBody.push_back(std::move(preStmt));
					for (auto& postStmt: m_exprBuilder->takePending())
						targetBody.push_back(std::move(postStmt));
					auto boxKey = awst::makeUtf8BytesConstant(
						var->name(), method.sourceLocation);
					auto put = awst::makeIntrinsicCall(
						"box_put", awst::WType::voidType(), method.sourceLocation);
					put->stackArgs.push_back(std::move(boxKey));
					put->stackArgs.push_back(std::move(initVal));
					targetBody.push_back(awst::makeExpressionStatement(
						std::move(put), method.sourceLocation));
					continue;
				}

				if (kind != awst::AppStorageKind::AppGlobal)
					continue;

				auto key = awst::makeUtf8BytesConstant(var->name(), method.sourceLocation);

				std::shared_ptr<awst::Expression> defaultVal;
				if (var->value())
				{
					// Pre-write zero so self-referencing immutable initializers
					// (`uint immutable x = x + 1`) read 0 via app_global_get_ex.
					// Non-immutable vars get zero from the fall-through below.
					if (var->immutable())
					{
						std::shared_ptr<awst::Expression> zeroVal;
						if (wtype == awst::WType::accountType())
							zeroVal = awst::makeAddressConstant(
								"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ",
								method.sourceLocation);
						else if (wtype == awst::WType::biguintType())
							zeroVal = awst::makeZero(method.sourceLocation, awst::WType::biguintType());
						else if (wtype == awst::WType::boolType() || wtype == awst::WType::uint64Type())
							zeroVal = awst::makeZero(method.sourceLocation);
						else
							zeroVal = StorageMapper::makeDefaultValue(wtype, method.sourceLocation);
						auto preKey = awst::makeUtf8BytesConstant(var->name(), method.sourceLocation);
						auto prePut = awst::makeAppGlobalPut(
							preKey, std::move(zeroVal), method.sourceLocation);
						targetBody.push_back(
							awst::makeExpressionStatement(std::move(prePut), method.sourceLocation));
					}

					defaultVal = m_exprBuilder->build(*var->value());
					if (defaultVal)
						defaultVal = TypeCoercion::coerceForAssignment(
							std::move(defaultVal), wtype, method.sourceLocation);
					// Flush prePending (e.g. new C() inner-txn create+fund)
					// before the state-var assignment uses __new_app_id_N.
					for (auto& preStmt: m_exprBuilder->takePrePending())
						targetBody.push_back(std::move(preStmt));
					for (auto& postStmt: m_exprBuilder->takePending())
						targetBody.push_back(std::move(postStmt));
				}
				if (!defaultVal)
				{
				if (wtype == awst::WType::accountType())
					defaultVal = awst::makeAddressConstant(
						"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ",
						method.sourceLocation);
				else if (wtype == awst::WType::biguintType())
				{
					auto val = awst::makeZero(method.sourceLocation, awst::WType::biguintType());
					defaultVal = val;
				}
				else if (wtype == awst::WType::boolType()
					|| wtype == awst::WType::uint64Type())
				{
					auto val = awst::makeZero(method.sourceLocation);
					defaultVal = val;
				}
				else if (wtype->kind() == awst::WTypeKind::ReferenceArray
					|| wtype->kind() == awst::WTypeKind::ARC4StaticArray
					|| wtype->kind() == awst::WTypeKind::ARC4DynamicArray)
				{
					defaultVal = StorageMapper::makeDefaultValue(wtype, method.sourceLocation);
				}
				else if (wtype->kind() == awst::WTypeKind::ARC4Struct
					|| wtype->kind() == awst::WTypeKind::WTuple)
				{
					defaultVal = StorageMapper::makeDefaultValue(wtype, method.sourceLocation);
				}
				else
				{
					// bytes1..bytes32: N zero bytes so the auto-getter ABI emits the
					// declared width. Dynamic bytes/string keep the empty default.
					int bytesLen = 0;
					if (auto const* bw = dynamic_cast<awst::BytesWType const*>(wtype))
						if (bw->length().has_value() && *bw->length() > 0)
							bytesLen = static_cast<int>(*bw->length());
					defaultVal = awst::makeBytesConstant(
						std::vector<uint8_t>(static_cast<size_t>(bytesLen), 0),
						method.sourceLocation,
						awst::BytesEncoding::Base16,
						wtype && wtype->kind() == awst::WTypeKind::Bytes
							? wtype : awst::WType::bytesType());
				}
				} // end if (!defaultVal)

				// app_global_put(key, defaultVal)
				auto put = awst::makeAppGlobalPut(key, defaultVal, method.sourceLocation);

				auto stmt = awst::makeExpressionStatement(put, method.sourceLocation);
				targetBody.push_back(stmt);
			}
		};

		// Collect box-stored array/bytes vars for box_create in __postInit.
		{
			std::set<std::string> lengthInitialized;
			forEachStateVarReverse(_contract, [&](auto const* var)
			{
				if (var->isConstant())
					return;
				if (lengthInitialized.count(var->name()))
					return;

				auto kind = StorageMapper::shouldUseBoxStorage(*var)
					? awst::AppStorageKind::Box
					: awst::AppStorageKind::AppGlobal;

				if (kind != awst::AppStorageKind::Box)
					return;

				auto* wtype = m_typeMapper.map(var->type());
				if (!wtype)
					return;
				// Dynamic arrays, dynamic bytes, and ARC4 static arrays all need
				// box_create at deploy time ("no such box" otherwise).
				bool isBoxType = wtype->kind() == awst::WTypeKind::ReferenceArray
					|| wtype->kind() == awst::WTypeKind::ARC4DynamicArray
					|| wtype->kind() == awst::WTypeKind::ARC4StaticArray
					|| wtype == awst::WType::bytesType()
					|| (wtype->kind() == awst::WTypeKind::Bytes
						&& !dynamic_cast<awst::BytesWType const*>(wtype)->length().has_value());
				if (!isBoxType)
					return;

				// ARC4StaticArray: oversized → multi-box layout (N boxes keyed
				// `<name>++itob(page)`). AVM single-box cap = 32768 B;
				// page = idx / elemsPerBox at runtime.
				if (wtype->kind() == awst::WTypeKind::ARC4StaticArray)
				{
					auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(wtype);
					if (sa && sa->arraySize() > 0)
					{
						uint64_t totalBytes = StorageMapper::arc4StaticArrayTotalBytes(wtype);
						// Cap pre-allocation at 4 boxes (128 KB). Beyond that,
						// __postInit box_create burst exceeds write-budget (~8
						// box_create per app call). .length reads still work
						// (compile-time constant); element writes on
						// un-pre-allocated arrays fail — see multi-box-storage.md.
						// totalBytes==0 (struct/dynamic-element) falls through to
						// single-box path.
						constexpr uint64_t MAX_PREALLOC_BYTES = 4ULL * 32768ULL;
						if (totalBytes > MAX_PREALLOC_BYTES)
						{
							Logger::instance().warning(
								"state array '" + var->name() + "' has declared size "
								+ std::to_string(sa->arraySize())
								+ " which exceeds 4-box (128 KB) pre-allocation cap — skipping box_create. "
								"Element writes will fail at runtime but .length reads "
								"still return the declared size.",
								method.sourceLocation);
							return;
						}
					}
				}

				lengthInitialized.insert(var->name());
				// Dynamic array boxes are created in __postInit (after funding)
				// Length is derived from box_len / element_size (no separate counter)
				m_boxArrayVarNames.push_back(var->name());
			});
		}

		if (!m_boxArrayVarNames.empty())
			needsPostInit = true;

		auto const* constructor = _contract.constructor();
		std::map<solidity::frontend::ContractDefinition const*,
			std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression>> const*>
			explicitBaseArgs;

		if (constructor)
		{
			// Decode constructor params from ApplicationArgs (ARC4-encoded, one per slot).
			int argIndex = 0;
			for (auto const& param: constructor->parameters())
			{
				auto* paramType = m_typeMapper.map(param->type());

				// txna ApplicationArgs i → raw ARC4 bytes
				auto readArg = awst::makeAppArg(argIndex, method.sourceLocation);

				std::shared_ptr<awst::Expression> paramVal;

				if (paramType == awst::WType::accountType())
				{
					auto cast = awst::makeAsAccount(std::move(readArg), method.sourceLocation);
					paramVal = std::move(cast);
				}
				else if (paramType == awst::WType::biguintType())
				{
					auto cast = awst::makeAsBiguint(std::move(readArg), method.sourceLocation);
					paramVal = std::move(cast);
				}
				else if (paramType == awst::WType::uint64Type()
					|| paramType == awst::WType::boolType())
				{
					// Args are 32-byte big-endian (EVM ABI); extract last 8 + btoi.
					auto len = awst::makeLen(readArg, method.sourceLocation);

					auto eight = awst::makeIntegerConstant("8", method.sourceLocation);

					auto offset = awst::makeUInt64BinOp(std::move(len), awst::UInt64BinaryOperator::Sub, eight, method.sourceLocation);

					auto eight2 = awst::makeIntegerConstant("8", method.sourceLocation);
					auto extract = awst::makeExtract3(
						std::move(readArg), std::move(offset), std::move(eight2),
						method.sourceLocation);

					paramVal = awst::makeBtoi(
						std::move(extract), method.sourceLocation, paramType);
				}
				else if (paramType == awst::WType::stringType())
				{
					auto cast = awst::makeReinterpretCast(std::move(readArg), awst::WType::stringType(), method.sourceLocation);
					paramVal = std::move(cast);
				}
				else if (paramType->kind() == awst::WTypeKind::ReferenceArray)
				{
					auto const* arc4Type = m_typeMapper.mapToARC4Type(paramType);
					auto cast = awst::makeReinterpretCast(std::move(readArg), arc4Type, method.sourceLocation);

					auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(paramType);
					if (refArr && !refArr->arraySize().has_value())
						paramVal = awst::makeConvertArray(std::move(cast), paramType, method.sourceLocation);
					else
					{
						auto decode = awst::makeARC4Decode(std::move(cast), paramType, method.sourceLocation);
						paramVal = std::move(decode);
					}
				}
				else if (paramType->kind() == awst::WTypeKind::ARC4StaticArray
					|| paramType->kind() == awst::WTypeKind::ARC4DynamicArray)
				{
					auto cast = awst::makeReinterpretCast(std::move(readArg), paramType, method.sourceLocation);
					paramVal = std::move(cast);
				}
				else if (paramType->kind() == awst::WTypeKind::Bytes
					&& dynamic_cast<awst::BytesWType const*>(paramType)
					&& dynamic_cast<awst::BytesWType const*>(paramType)->length().has_value())
				{
					auto cast = awst::makeReinterpretCast(std::move(readArg), paramType, method.sourceLocation);
					paramVal = std::move(cast);
				}
				else if (dynamic_cast<awst::ARC4Struct const*>(paramType))
				{
					auto cast = awst::makeReinterpretCast(std::move(readArg), paramType, method.sourceLocation);
					paramVal = std::move(cast);
				}
				else
				{
					paramVal = std::move(readArg);
				}

				auto target = awst::makeVarExpression(param->name(), paramType, method.sourceLocation);

				auto assignment = awst::makeAssignmentStatement(target, std::move(paramVal), method.sourceLocation);
				createBlock->body.push_back(std::move(assignment));

				++argIndex;
			}

		}

		// solc pre-populates baseConstructorArguments (InheritanceSpecifier or
		// ModifierInvocation → args) — no manual MRO walk needed.
		for (auto const& [baseCtor, argNode] : _contract.annotation().baseConstructorArguments)
		{
			auto const* baseContract = baseCtor->annotation().contract;
			if (!baseContract) continue;
			std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression>> const* args = nullptr;
			if (auto const* mod = dynamic_cast<solidity::frontend::ModifierInvocation const*>(argNode))
				args = mod->arguments();
			else if (auto const* spec = dynamic_cast<solidity::frontend::InheritanceSpecifier const*>(argNode))
				args = spec->arguments();
			if (args && !args->empty())
				explicitBaseArgs[baseContract] = args;
		}

		if (needsPostInit)
		{
			// Defer all init to __postInit; create call only sets the pending flag.
			auto pendingKey = awst::makeUtf8BytesConstant("__ctor_pending", method.sourceLocation);

			auto one = awst::makeOne(method.sourceLocation);

			auto setPending = awst::makeAppGlobalPut(pendingKey, one, method.sourceLocation);

			auto setPendingStmt = awst::makeExpressionStatement(setPending, method.sourceLocation);
			createBlock->body.push_back(std::move(setPendingStmt));

			// Build __postInit method with deferred constructor body
			awst::ContractMethod postInit;
			postInit.sourceLocation = method.sourceLocation;
			postInit.returnType = awst::WType::voidType();
			postInit.cref = m_sourceFile + "." + _contractName;
			postInit.memberName = "__postInit";

			// Mirror constructor params on __postInit so the caller passes the same values.
			if (constructor)
			{
				int paramIdx = 0;
				for (auto const& param: constructor->parameters())
				{
					awst::SubroutineArgument arg;
					arg.name = param->name().empty()
						? "_param" + std::to_string(paramIdx)
						: param->name();
					arg.sourceLocation = method.sourceLocation;
					arg.wtype = m_typeMapper.map(param->type());
					postInit.args.push_back(std::move(arg));
					++paramIdx;
				}
			}

			awst::ARC4ABIMethodConfig postInitConfig;
			postInitConfig.name = "__postInit";
			postInitConfig.sourceLocation = method.sourceLocation;
			postInitConfig.allowedCompletionTypes = {0}; // NoOp
			postInitConfig.create = 3; // Disallow
			postInitConfig.readonly = false;
			postInit.arc4MethodConfig = postInitConfig;

			// Remap biguint→ARC4UIntN and aggregates→ARC4 for correct ABI signature,
			// matching regular method-param remap. Track (origName, arc4Name) for
			// the decode statements emitted at the top of __postInit body.
			struct PostInitDecode { std::string origName; std::string arc4Name; awst::WType const* arc4Type; awst::WType const* origType; };
			std::vector<PostInitDecode> postInitDecodes;
			for (size_t pi = 0; pi < postInit.args.size(); ++pi)
			{
				auto& arg = postInit.args[pi];

				if (arg.wtype == awst::WType::biguintType() && constructor
					&& pi < constructor->parameters().size())
				{
					auto const* solType = constructor->parameters()[pi]->annotation().type;
					auto const* intType = solType ? dynamic_cast<solidity::frontend::IntegerType const*>(solType) : nullptr;
					if (!intType && solType)
						if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
							intType = dynamic_cast<solidity::frontend::IntegerType const*>(&udvt->underlyingType());
					// Signed stays as biguint (two's-complement); ARC4UIntN would reject it.
					if (intType && !intType->isSigned())
					{
						unsigned bits = intType->numBits();
						auto const* arc4Type = m_typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(bits));
						std::string origName = arg.name;
						std::string arc4Name = "__arc4_" + origName;
						postInitDecodes.push_back({origName, arc4Name, arc4Type, arg.wtype});
						arg.name = arc4Name;
						arg.wtype = arc4Type;
						continue;
					}
				}

				bool isAggregate = arg.wtype
					&& (arg.wtype->kind() == awst::WTypeKind::ReferenceArray
						|| arg.wtype->kind() == awst::WTypeKind::ARC4StaticArray
						|| arg.wtype->kind() == awst::WTypeKind::ARC4DynamicArray
						|| arg.wtype->kind() == awst::WTypeKind::WTuple);
				if (isAggregate)
				{
					awst::WType const* arc4Type = m_typeMapper.mapToARC4Type(arg.wtype);
					if (arc4Type != arg.wtype)
						arg.wtype = arc4Type;
				}
			}

			// Function context uses original names+types (remapped biguints resolved via
			// the decoded local emitted below, not the __arc4_* shim arg).
			{
				std::vector<std::pair<std::string, awst::WType const*>> paramContext;
				std::set<std::string> arc4Names;
				for (auto const& d: postInitDecodes)
					arc4Names.insert(d.arc4Name);
				for (auto const& arg: postInit.args)
				{
					if (arc4Names.count(arg.name)) continue; // skip the __arc4_ shim
					paramContext.emplace_back(arg.name, arg.wtype);
				}
				for (auto const& d: postInitDecodes)
					paramContext.emplace_back(d.origName, d.origType);
				setFunctionContext(paramContext, postInit.returnType);
			}

			auto postInitBody = awst::makeBlock(method.sourceLocation);

			// Guard: assert(__ctor_pending == 1)
			auto readPending = awst::makeIntrinsicCall("app_global_get", awst::WType::uint64Type(), method.sourceLocation);
			readPending->stackArgs.push_back(
				awst::makeUtf8BytesConstant("__ctor_pending", method.sourceLocation));

			auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(
				readPending, method.sourceLocation, "__postInit already called"), method.sourceLocation);
			postInitBody->body.push_back(std::move(assertStmt));

			// Clear flag: __ctor_pending = 0
			auto clearKey = awst::makeUtf8BytesConstant("__ctor_pending", method.sourceLocation);

			auto zeroVal = awst::makeZero(method.sourceLocation);

			auto clearPending = awst::makeAppGlobalPut(clearKey, zeroVal, method.sourceLocation);

			auto clearStmt = awst::makeExpressionStatement(clearPending, method.sourceLocation);
			postInitBody->body.push_back(std::move(clearStmt));

			// Decode each remapped biguint arg: `<origName> = ARC4Decode(__arc4_<origName>)`.
			for (auto const& decode: postInitDecodes)
			{
				auto arc4Var = awst::makeVarExpression(decode.arc4Name, decode.arc4Type, method.sourceLocation);

				auto decodeExpr = awst::makeARC4Decode(std::move(arc4Var), decode.origType, method.sourceLocation);

				auto target = awst::makeVarExpression(decode.origName, decode.origType, method.sourceLocation);

				auto assign = awst::makeAssignmentStatement(std::move(target), std::move(decodeExpr), method.sourceLocation);
				postInitBody->body.push_back(std::move(assign));
			}

			emitBoxCreateForStateVars(_contract, *postInitBody, method.sourceLocation);

			// State var defaults after box creation, before constructor bodies.
			{
				auto const& lin = _contract.annotation().linearizedBaseContracts;
				for (auto it2 = lin.rbegin(); it2 != lin.rend(); ++it2)
					emitStateVarInit(**it2, postInitBody->body);
			}

			// Inline base constructor bodies into __postInit
			auto const& linearized = _contract.annotation().linearizedBaseContracts;
			for (auto it = linearized.rbegin(); it != linearized.rend(); ++it)
			{
				auto const* base = *it;
				if (base == &_contract)
					continue;

				auto const* baseCtor = base->constructor();
				if (!baseCtor || !baseCtor->isImplemented())
					continue;
				if (baseCtor->body().statements().empty())
					continue;

				// Base constructor parameter assignments
				auto argIt = explicitBaseArgs.find(base);
				if (argIt != explicitBaseArgs.end() && argIt->second && !argIt->second->empty())
				{
					auto const& args = *(argIt->second);
					auto const& params = baseCtor->parameters();
					for (size_t i = 0; i < args.size() && i < params.size(); ++i)
					{
						auto argExpr = m_exprBuilder->build(*args[i]);
						if (!argExpr)
							continue;

						// Storage-pointer params: alias, don't copy — writes inside the
						// base ctor must reach the underlying storage (mirrors
						// ModifierInliner.cpp:200-228).
						if (params[i]->referenceLocation()
							== solidity::frontend::VariableDeclaration::Location::Storage)
						{
							sol_ast::StorageAlias alias = [&]() -> sol_ast::StorageAlias {
								if (dynamic_cast<awst::BytesConstant const*>(argExpr.get()))
									return sol_ast::StorageAlias::mappingHolder(std::move(argExpr));
								if (dynamic_cast<awst::IndexExpression const*>(argExpr.get()))
									return sol_ast::StorageAlias::indexedPath(std::move(argExpr));
								if (dynamic_cast<awst::FieldExpression const*>(argExpr.get()))
									return sol_ast::StorageAlias::fieldPath(std::move(argExpr));
								if (dynamic_cast<awst::TupleItemExpression const*>(argExpr.get()))
									return sol_ast::StorageAlias::tupleSlice(std::move(argExpr));
								return sol_ast::StorageAlias::stateRead(std::move(argExpr));
							}();
							m_tr->setStorageAlias(params[i]->id(), std::move(alias));
							continue;
						}

						auto target = awst::makeVarExpression(params[i]->name(), m_typeMapper.map(params[i]->type()), makeLoc(args[i]->location()));

						argExpr = TypeCoercion::implicitNumericCast(
							std::move(argExpr), target->wtype, target->sourceLocation
						);

						auto assignment = awst::makeAssignmentStatement(target, std::move(argExpr), target->sourceLocation);
						postInitBody->body.push_back(std::move(assignment));
					}
				}

				m_currentInConstructor = true;
			auto baseBody = buildBlock(baseCtor->body());
			m_currentInConstructor = false;
				inlineModifiers(*baseCtor, baseBody);
				for (auto& stmt: baseBody->body)
					postInitBody->body.push_back(std::move(stmt));
			}

			// Main constructor body
			if (constructor && constructor->body().statements().size() > 0)
			{
				m_tr->setInConstructor(true);
				m_currentInConstructor = true;
			auto ctorBody = buildBlock(constructor->body());
			m_currentInConstructor = false;
				inlineModifiers(*constructor, ctorBody);
				m_tr->setInConstructor(false);
				for (auto& stmt: ctorBody->body)
					postInitBody->body.push_back(std::move(stmt));
			}

			// `--ensure-budget __postInit:N`: __postInit is built here, not
			// through FunctionBuilder's per-method path, so budget injection
			// is explicit. fee_source=1 (AppAccount) — deploy_app doesn't
			// pad extra_fee like a user-driven group, so the contract's funded
			// balance covers the pump's ITxnCreate fees instead (GroupCredit=0
			// would underflow). Pump inserted at the very top so box-init /
			// inline-asm / EIP-712 hashing all draw from the expanded pool.
			if (auto it = m_ensureBudget.find("__postInit");
				it != m_ensureBudget.end() && it->second > 0)
			{
				auto budgetVal = awst::makeIntegerConstant(
					it->second, postInit.sourceLocation);
				auto feeSource = awst::makeIntegerConstant(
					"1", postInit.sourceLocation);
				auto call = awst::makePuyaLibCall("ensure_budget",
					{
						awst::CallArg{std::string("required_budget"), std::move(budgetVal)},
						awst::CallArg{std::string("fee_source"), std::move(feeSource)},
					},
					awst::WType::voidType(), postInit.sourceLocation);
				auto stmt = awst::makeExpressionStatement(
					std::move(call), postInit.sourceLocation);
				postInitBody->body.insert(postInitBody->body.begin(), std::move(stmt));
			}

			postInit.body = postInitBody;
			m_postInitMethod = std::move(postInit);
		}
		else
		{
		// Inline ctor into the bool-returning approval program.
		// Assembly return() must emit bool (AssemblyBuilder::handleReturn when
		// m_returnType is bool) — set returnType accordingly.
		auto const* savedReturnType = m_currentReturnType;
		m_currentReturnType = awst::WType::boolType();

		// Legacy (compileViaYul:false): all state var inits before any ctor arg eval.
		// `constructor_inheritance_init_order_3_legacy`: A's `uint x = 2` runs first,
		// THEN B's `A(f())` evaluates f() (sets x=4) — final x=4. emitStateVarInit
		// deduplicates via stateVarInitialized so the interleaved loop below is safe.
		// viaIR: keep interleaved order (derived inits observe base ctor state).
		if (!m_viaIR)
		{
			auto const& linEarly = _contract.annotation().linearizedBaseContracts;
			for (auto itEarly = linEarly.rbegin(); itEarly != linEarly.rend(); ++itEarly)
				emitStateVarInit(**itEarly, createBlock->body);
		}

		// Pre-evaluate ctor args in dependency order (viaIR only).
		// For D→C→A, C's params must be assigned first so A's args (from C's modifier)
		// see C's param values. Phase 1: direct args. Phase 2: transitive args.
		std::map<solidity::frontend::ContractDefinition const*,
			std::vector<std::shared_ptr<awst::Expression>>> preEvaluatedArgs;
		{
			// Direct bases: ModifierInvocation on derived ctor or InheritanceSpecifier
			// on the contract itself.
			std::set<solidity::frontend::ContractDefinition const*> directBases;
			auto recordBase = [&](solidity::frontend::Declaration const* _ref) {
				if (auto const* bc = dynamic_cast<solidity::frontend::ContractDefinition const*>(_ref))
					directBases.insert(bc);
			};
			if (constructor)
				for (auto const& mod: constructor->modifiers())
					recordBase(mod->name().annotation().referencedDeclaration);
			for (auto const& baseSpec: _contract.baseContracts())
				recordBase(baseSpec->name().annotation().referencedDeclaration);

			// Phase 1: Assign direct base ctor params into createBlock
			// (so transitive args can reference them)
			for (auto const* directBase: directBases)
			{
				auto argIt = explicitBaseArgs.find(directBase);
				if (argIt == explicitBaseArgs.end() || !argIt->second || argIt->second->empty())
					continue;
				auto const* baseCtor = directBase->constructor();
				if (!baseCtor)
					continue;

				auto const& args = *(argIt->second);
				auto const& params = baseCtor->parameters();
				for (size_t i = 0; i < args.size() && i < params.size(); ++i)
				{
					auto argExpr = m_exprBuilder->build(*args[i]);
					if (!argExpr)
						continue;
					auto* targetType = m_typeMapper.map(params[i]->type());
					argExpr = TypeCoercion::implicitNumericCast(
						std::move(argExpr), targetType, makeLoc(args[i]->location()));

					auto target = awst::makeVarExpression(params[i]->name(), targetType, makeLoc(args[i]->location()));

					auto assignment = awst::makeAssignmentStatement(target, std::move(argExpr), target->sourceLocation);
					createBlock->body.push_back(std::move(assignment));
				}
			}

			// Phase 2: Transitive args in derived-first order so intermediates are
			// assigned before deeper transitives reference them.
			// E.g. Final→Derived→Base1→Base: assign Base1.k first (from Derived.i),
			// then evaluate Base.j (from Base1.k).
			auto const& lin = _contract.annotation().linearizedBaseContracts;
			for (auto it = lin.begin(); it != lin.end(); ++it)
			{
				auto const* base = *it;
				if (base == &_contract)
					continue;
				if (directBases.count(base))
					continue;

				auto argIt = explicitBaseArgs.find(base);
				if (argIt == explicitBaseArgs.end() || !argIt->second || argIt->second->empty())
					continue;
				auto const* baseCtor = base->constructor();
				if (!baseCtor)
					continue;

				auto const& args = *(argIt->second);
				auto const& params = baseCtor->parameters();

				// Assign these params into createBlock NOW (so deeper transitives can see them)
				for (size_t i = 0; i < args.size() && i < params.size(); ++i)
				{
					auto argExpr = m_exprBuilder->build(*args[i]);
					if (!argExpr)
						continue;
					auto* targetType = m_typeMapper.map(params[i]->type());
					argExpr = TypeCoercion::implicitNumericCast(
						std::move(argExpr), targetType, makeLoc(args[i]->location()));

					auto target = awst::makeVarExpression(params[i]->name(), targetType, makeLoc(args[i]->location()));

					auto assignment = awst::makeAssignmentStatement(target, std::move(argExpr), target->sourceLocation);
					createBlock->body.push_back(std::move(assignment));
				}

				// Mark these params as pre-evaluated (empty vector = already assigned)
				preEvaluatedArgs[base] = {};
			}
		}

		// Interleave state var init with ctor bodies (base-first MRO order) so
		// derived initializers (e.g. `uint y = f()`) observe base ctor state (viaIR).
		auto const& linearized = _contract.annotation().linearizedBaseContracts;
		for (auto it = linearized.rbegin(); it != linearized.rend(); ++it)
		{
			auto const* base = *it;

			emitStateVarInit(*base, createBlock->body);

			if (base == &_contract)
				continue; // Main ctor handled separately below

			auto const* baseCtor = base->constructor();
			if (!baseCtor || !baseCtor->isImplemented())
				continue;
			if (baseCtor->body().statements().empty())
				continue;

			// Direct base params were assigned in Phase 1; transitive use pre-evaluated.
			auto preIt = preEvaluatedArgs.find(base);
			if (preIt != preEvaluatedArgs.end())
			{
				auto const& evaledArgs = preIt->second;
				auto const& params = baseCtor->parameters();
				for (size_t i = 0; i < evaledArgs.size() && i < params.size(); ++i)
				{
					if (!evaledArgs[i])
						continue;

					auto target = awst::makeVarExpression(params[i]->name(), m_typeMapper.map(params[i]->type()), method.sourceLocation);

					auto assignment = awst::makeAssignmentStatement(target, evaledArgs[i], method.sourceLocation);
					createBlock->body.push_back(std::move(assignment));
				}
			}

			// Translate the base constructor body and inline its modifiers
			m_currentInConstructor = true;
			auto baseBody = buildBlock(baseCtor->body());
			m_currentInConstructor = false;
			inlineModifiers(*baseCtor, baseBody);
			for (auto& stmt: baseBody->body)
				createBlock->body.push_back(std::move(stmt));
		}

		if (constructor && constructor->body().statements().size() > 0)
		{
			// Restore super targets (super.f() in ctor body) + per-ctor MRO overrides.
			for (auto const& [id, name]: m_allSuperTargetNames)
				m_tr->setSuperTarget(id, name);
			{
				auto pfit = m_perFuncSuperOverrides.find(constructor->id());
				if (pfit != m_perFuncSuperOverrides.end())
					for (auto const& [targetId, superName]: pfit->second)
						m_tr->setSuperTarget(targetId, superName);
			}
			m_tr->setInConstructor(true);
			m_currentInConstructor = true;
			auto ctorBody = buildBlock(constructor->body());
			m_currentInConstructor = false;
			inlineModifiers(*constructor, ctorBody);
			m_tr->setInConstructor(false);
			m_tr->clearSuperTargets();
			for (auto& stmt: ctorBody->body)
				createBlock->body.push_back(std::move(stmt));
		}
		m_currentReturnType = savedReturnType;
		} // end else (no postInit needed)

		// Return true to complete the create transaction
		auto createReturn = awst::makeReturnStatement(awst::makeTrue(method.sourceLocation), method.sourceLocation);
		createBlock->body.push_back(createReturn);

		// Init transient-storage blob (scratch TRANSIENT_SLOT) BEFORE the create/dispatch
		// split so the ctor body can use tload/tstore (create branch returns early).
		// Per-txn scratch bzero matches EIP-1153; writes persist across callsub.
		// Size = declared transient vars (packed), minimum SLOT_SIZE for asm tload/tstore.
		{
			unsigned blobBytes = m_transientStorage.blobSize();
			if (blobBytes < AssemblyBuilder::SLOT_SIZE)
				blobBytes = AssemblyBuilder::SLOT_SIZE;

			auto storeOp = awst::makeStoreSlot(
				AssemblyBuilder::TRANSIENT_SLOT,
				awst::makeBzero(blobBytes, method.sourceLocation),
				method.sourceLocation);

			auto exprStmt = awst::makeExpressionStatement(std::move(storeOp), method.sourceLocation);
			body->body.push_back(std::move(exprStmt));
		}

		// Init EVM memory blobs BEFORE create/dispatch split so ctor body's
		// `T memory t;` locals (FMP bumps on slot 0) see a valid blob.
		// Uninitialised scratch reads as uint64 0 — must bzero every slot per call.
		{
			for (int s = AssemblyBuilder::MEMORY_SLOT_FIRST; s <= AssemblyBuilder::MEMORY_SLOT_LAST; ++s)
			{
				auto storeOp = awst::makeStoreSlot(
					s,
					awst::makeBzero(AssemblyBuilder::SLOT_SIZE, method.sourceLocation),
					method.sourceLocation);
				body->body.push_back(awst::makeExpressionStatement(std::move(storeOp), method.sourceLocation));
			}

			// Write the free memory pointer (FMP) at offset 0x40 = 0x80.
			auto loadBlob = awst::makeLoadSlot(
				AssemblyBuilder::MEMORY_SLOT_FIRST, method.sourceLocation);

			auto fmpOffset = awst::makeIntegerConstant("64", method.sourceLocation); // 0x40

			std::vector<uint8_t> fmpBytesVal(31, 0);
			fmpBytesVal.push_back(0x80);
			auto fmpBytes = awst::makeBytesConstant(
				std::move(fmpBytesVal), method.sourceLocation, awst::BytesEncoding::Unknown);

			auto replaceOp = awst::makeReplace3(std::move(loadBlob), std::move(fmpOffset), std::move(fmpBytes), method.sourceLocation);
			auto storeFmpOp = awst::makeStoreSlot(
				AssemblyBuilder::MEMORY_SLOT_FIRST, std::move(replaceOp), method.sourceLocation);

			auto fmpStmt = awst::makeExpressionStatement(std::move(storeFmpOp), method.sourceLocation);
			body->body.push_back(std::move(fmpStmt));
		}

		body->body.push_back(awst::makeIfElse(
			isCreate, createBlock, nullptr, method.sourceLocation));
	}

	// Transient vars: preamble bzero satisfies EIP-1153 per-tx reset; no
	// per-call app_global reset needed.

	// solc's fallbackFunction()/receiveFunction() walk linearized MRO.
	auto const* fallbackFunc = _contract.fallbackFunction();
	auto const* receiveFunc = _contract.receiveFunction();
	if (fallbackFunc && !fallbackFunc->isImplemented())
		fallbackFunc = nullptr;
	if (receiveFunc && !receiveFunc->isImplemented())
		receiveFunc = nullptr;

	emitSelectorDispatch(*body, fallbackFunc, receiveFunc, method.sourceLocation);

	method.body = body;

	return method;
}

void ContractBuilder::emitBoxCreateForStateVars(
	solidity::frontend::ContractDefinition const& _contract,
	awst::Block& _postInitBody,
	awst::SourceLocation const& _loc)
{
	// Create boxes for dynamic array state variables
	for (auto const& varName: m_boxArrayVarNames)
	{
		auto boxKey = awst::makeUtf8BytesConstant(varName, _loc);

		// Dynamic bytes without init: box_create(size=0). Raw content has no length
		// header, so empty box = empty bytes. Required so BoxValueExpression (bare
		// box_extract path) works; old box_get→select fallback reverts on >4 KB
		// (AVM stack-value cap). See StorageMapper::makeStateGetWithDefault.
		bool isDynamicBytesWithoutInit = false;
		forEachStateVar(_contract, [&](auto const* var)
		{
			if (var->name() != varName || var->isConstant())
				return;
			auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(var->type());
			if (arrType && arrType->isByteArrayOrString() && !var->value())
				isDynamicBytesWithoutInit = true;
		});
		if (isDynamicBytesWithoutInit)
		{
			auto sizeZero = awst::makeIntegerConstant(0, _loc);
			auto boxCreate = awst::makeBoxCreate(
				std::move(boxKey), std::move(sizeZero),
				_loc);
			auto boxStmt = awst::makeExpressionStatement(
				std::move(boxCreate), _loc);
			_postInitBody.body.push_back(std::move(boxStmt));
			continue;
		}

		// boxSizeVal: 2 (ARC4 dyn-array length header), or literal size,
		// or elementSize*N for static arrays (e.g. uint[20]).
		unsigned boxSizeVal = 2; // ARC4 dynamic array length header
		std::shared_ptr<awst::Expression> boxInitVal;
		// ARC4StaticArray<dynamic T>: zeroed buffer is invalid ARC4 (head offsets
		// must exceed head). Synthesise default encoding → box_put instead.
		std::optional<std::vector<uint8_t>> dynArc4Default;
		forEachStateVar(_contract, [&](auto const* var)
		{
			if (var->name() != varName || var->isConstant())
				return;
			// ARC4StaticArray (uint[N], int[N], etc.): allocate
			// elementSize * arraySize bytes so the contract can
			// write to slot indices without "no such box".
			auto* varWtype = m_typeMapper.map(var->type());
			if (varWtype && varWtype->kind() == awst::WTypeKind::ARC4StaticArray)
			{
				auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(varWtype);
				if (sa && sa->arraySize() > 0)
				{
					if (arc4IsDynamic(sa))
					{
						if (auto enc = arc4DefaultEncoding(sa))
							if (enc->size() > 0 && enc->size() <= 32768)
								dynArc4Default = std::move(*enc);
					}
					uint64_t elemSize = 32; // default for uint256
					auto const* elemT = sa->elementType();
					if (elemT)
					{
						if (auto const* uintN = dynamic_cast<awst::ARC4UIntN const*>(elemT))
							elemSize = std::max<uint64_t>(1u, static_cast<uint64_t>(uintN->n() / 8));
						else if (elemT->kind() == awst::WTypeKind::Bytes)
						{
							auto const* bw = dynamic_cast<awst::BytesWType const*>(elemT);
							if (bw && bw->length().has_value())
								elemSize = *bw->length();
						}
					}
					// AVM box cap = 32768 B; oversized → multi-box below.
					// Record per-box size here.
					uint64_t size = elemSize * static_cast<uint64_t>(sa->arraySize());
					if (size > 32768)
						size = 32768;
					boxSizeVal = static_cast<unsigned>(size);
				}
			}
			if (!var->value())
				return;
			auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(var->type());
			if (arrType && arrType->isByteArrayOrString())
			{
				if (auto const* lit = dynamic_cast<solidity::frontend::Literal const*>(var->value().get()))
					boxSizeVal = static_cast<unsigned>(lit->value().size());
				if (boxSizeVal > 0)
				{
					boxInitVal = m_exprBuilder->build(*var->value());
					if (boxInitVal && boxInitVal->wtype == awst::WType::stringType())
					{
						auto cast = awst::makeAsBytes(std::move(boxInitVal), _loc);
						boxInitVal = std::move(cast);
					}
				}
			}
			// Non-bytes dynamic array with initializer (e.g. `int16[] x = [-1,-2]`):
			// set boxInitVal so the loop below emits box_put instead of box_create(2).
			else if (arrType && arrType->isDynamicallySized()
				&& !arrType->isByteArrayOrString())
			{
				auto initVal = m_exprBuilder->build(*var->value());
				if (initVal)
				{
					auto* tgtWtype = m_typeMapper.map(arrType);
					initVal = TypeCoercion::coerceForAssignment(
						std::move(initVal), tgtWtype, _loc);
					// Materialise as bytes for box_put.
					if (initVal->wtype != awst::WType::bytesType())
						initVal = awst::makeAsBytes(std::move(initVal), _loc);
					boxInitVal = std::move(initVal);
				}
			}
		});

		// Multi-box detection: if the var's ARC4StaticArray total size
		// exceeds a single box's capacity, emit N box_create calls
		// keyed `<name>` ++ `itob(page)` instead of one. Element
		// reads/writes route at runtime via the same key suffix
		// scheme (see SolIndexAccessHandlers.cpp).
		unsigned multiBoxN = 0;
		unsigned multiBoxElemSize = 0;
		uint64_t multiBoxTotalBytes = 0;
		uint64_t multiBoxPerPageBytes = 0;
		forEachStateVar(_contract, [&](auto const* var)
		{
			if (var->name() != varName || var->isConstant())
				return;
			auto* varWtype = m_typeMapper.map(var->type());
			if (StorageMapper::isMultiBoxArray(varWtype))
			{
				multiBoxN = StorageMapper::numBoxesForArray(varWtype);
				multiBoxElemSize = StorageMapper::arc4StaticArrayElementSize(varWtype);
				multiBoxTotalBytes = StorageMapper::arc4StaticArrayTotalBytes(varWtype);
				multiBoxPerPageBytes = static_cast<uint64_t>(
					StorageMapper::elementsPerBox(varWtype)) * multiBoxElemSize;
			}
		});

		if (multiBoxN > 1 && multiBoxElemSize > 0 && !dynArc4Default && !boxInitVal)
		{
			// Multi-box: N box_create calls, key = name++itob(page).
			for (unsigned page = 0; page < multiBoxN; ++page)
			{
				auto nameBytes = awst::makeUtf8BytesConstant(varName, _loc);
				auto pageInt = awst::makeIntegerConstant(page, _loc);
				auto pageItob = awst::makeItob(std::move(pageInt), _loc);
				auto pageKey = awst::makeConcat(std::move(nameBytes), std::move(pageItob), _loc);

				uint64_t pageSize = (page == multiBoxN - 1)
					? (multiBoxTotalBytes - static_cast<uint64_t>(page) * multiBoxPerPageBytes)
					: multiBoxPerPageBytes;
				auto pageSizeExpr = awst::makeIntegerConstant(pageSize, _loc);

				auto boxCreate = awst::makeBoxCreate(
					std::move(pageKey), std::move(pageSizeExpr),
					_loc);

				auto boxStmt = awst::makeExpressionStatement(std::move(boxCreate), _loc);
				_postInitBody.body.push_back(std::move(boxStmt));
			}
		}
		else if (dynArc4Default)
		{
			// box_put creates + initialises with valid ARC4 head/tail in one op.
			auto put = awst::makeBoxPut(std::move(boxKey), awst::makeBytesConstant(
				std::move(*dynArc4Default), _loc), _loc);
			auto putStmt = awst::makeExpressionStatement(std::move(put), _loc);
			_postInitBody.body.push_back(std::move(putStmt));
		}
		else
		{
			// Non-bytes dyn-array init: encoded length ≠ header boxSizeVal=2;
			// box_put can't grow a pre-created box → skip box_create, let box_put
			// create at the right size.
			bool isNonBytesDynArrInit = false;
			forEachStateVar(_contract, [&](auto const* var)
			{
				if (var->name() != varName || var->isConstant() || !var->value())
					return;
				auto const* arrType =
					dynamic_cast<solidity::frontend::ArrayType const*>(var->type());
				if (arrType && arrType->isDynamicallySized()
					&& !arrType->isByteArrayOrString())
					isNonBytesDynArrInit = true;
			});

			if (!isNonBytesDynArrInit)
			{
				auto boxSize = awst::makeIntegerConstant(boxSizeVal, _loc);

				auto boxCreate = awst::makeBoxCreate(
					std::move(boxKey), std::move(boxSize),
					_loc);

				auto boxStmt = awst::makeExpressionStatement(std::move(boxCreate), _loc);
				_postInitBody.body.push_back(std::move(boxStmt));
			}

			if (boxInitVal)
			{
				auto putKey = awst::makeUtf8BytesConstant(varName, _loc);
				auto put = awst::makeBoxPut(std::move(putKey), std::move(boxInitVal), _loc);
				auto putStmt = awst::makeExpressionStatement(std::move(put), _loc);
				_postInitBody.body.push_back(std::move(putStmt));
			}
		}
	}
}

} // namespace puyasol::builder

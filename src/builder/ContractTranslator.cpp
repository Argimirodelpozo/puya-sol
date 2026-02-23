#include "builder/ContractTranslator.h"
#include "Logger.h"

#include <map>
#include <set>

namespace puyasol::builder
{

ContractTranslator::ContractTranslator(
	TypeMapper& _typeMapper,
	StorageMapper& _storageMapper,
	std::string const& _sourceFile,
	LibraryFunctionIdMap const& _libraryFunctionIds
)
	: m_typeMapper(_typeMapper),
	  m_storageMapper(_storageMapper),
	  m_sourceFile(_sourceFile),
	  m_libraryFunctionIds(_libraryFunctionIds)
{
}

awst::SourceLocation ContractTranslator::makeLoc(
	solidity::langutil::SourceLocation const& _solLoc
)
{
	awst::SourceLocation loc;
	loc.file = m_sourceFile;
	loc.line = _solLoc.start >= 0 ? _solLoc.start : 0;
	loc.endLine = _solLoc.end >= 0 ? _solLoc.end : 0;
	return loc;
}

std::shared_ptr<awst::Contract> ContractTranslator::translate(
	solidity::frontend::ContractDefinition const& _contract
)
{
	std::string contractName = _contract.name();
	std::string contractId = m_sourceFile + "." + contractName;

	// Create translators for this contract
	m_exprTranslator = std::make_unique<ExpressionTranslator>(
		m_typeMapper, m_storageMapper, m_sourceFile, contractName, m_libraryFunctionIds
	);
	m_stmtTranslator = std::make_unique<StatementTranslator>(
		*m_exprTranslator, m_typeMapper, m_sourceFile
	);

	auto contract = std::make_shared<awst::Contract>();
	contract->sourceLocation = makeLoc(_contract.location());
	contract->id = contractId;
	contract->name = contractName;

	// Description from NatSpec
	if (_contract.documentation())
		contract->description = *_contract.documentation()->text();

	// Method resolution order (linearized base contracts)
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		if (base != &_contract)
			contract->methodResolutionOrder.push_back(
				m_sourceFile + "." + base->name()
			);
	}

	// State variables → AppStorageDefinitions
	contract->appState = m_storageMapper.mapStateVariables(_contract, m_sourceFile);

	// Approval and clear programs
	contract->approvalProgram = buildApprovalProgram(_contract, contractName);
	contract->clearProgram = buildClearProgram(_contract, contractName);

	// Detect overloaded function names across all linearized base contracts
	m_overloadedNames.clear();
	std::unordered_map<std::string, int> nameCount;
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		for (auto const* func: base->definedFunctions())
		{
			if (func->isConstructor() || !func->isImplemented())
				continue;
			nameCount[func->name()]++;
		}
	}
	for (auto const& [name, count]: nameCount)
	{
		if (count > 1)
			m_overloadedNames.insert(name);
	}

	// Re-create expression translator with overload info
	m_exprTranslator = std::make_unique<ExpressionTranslator>(
		m_typeMapper, m_storageMapper, m_sourceFile, contractName,
		m_libraryFunctionIds, m_overloadedNames
	);
	m_stmtTranslator = std::make_unique<StatementTranslator>(
		*m_exprTranslator, m_typeMapper, m_sourceFile
	);

	// Translate all defined functions in this contract
	// Use "name(paramCount)" for overloaded functions to disambiguate
	std::set<std::string> translatedFunctions;
	for (auto const* func: _contract.definedFunctions())
	{
		if (func->isConstructor())
			continue;

		std::string key = func->name();
		if (m_overloadedNames.count(key))
			key += "(" + std::to_string(func->parameters().size()) + ")";
		translatedFunctions.insert(key);
		auto method = translateFunction(*func, contractName);
		contract->methods.push_back(std::move(method));
	}

	// Also include inherited functions that may be needed
	// (e.g. _checkOwner from Ownable, owner() from Ownable)
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		if (base == &_contract)
			continue; // Already handled above

		for (auto const* func: base->definedFunctions())
		{
			if (func->isConstructor())
				continue;

			std::string key = func->name();
			if (m_overloadedNames.count(key))
				key += "(" + std::to_string(func->parameters().size()) + ")";
			if (translatedFunctions.count(key))
				continue;

			if (!func->isImplemented())
				continue;

			translatedFunctions.insert(key);
			auto method = translateFunction(*func, contractName);
			contract->methods.push_back(std::move(method));
		}
	}

	return contract;
}

awst::ContractMethod ContractTranslator::buildApprovalProgram(
	solidity::frontend::ContractDefinition const& _contract,
	std::string const& _contractName
)
{
	awst::ContractMethod method;
	method.sourceLocation = makeLoc(_contract.location());
	method.returnType = awst::WType::boolType();
	method.cref = m_sourceFile + "." + _contractName;
	method.memberName = "approval_program";

	auto body = std::make_shared<awst::Block>();
	body->sourceLocation = method.sourceLocation;

	// Create-time check: if (Txn.ApplicationID == 0) { base_ctors; ctor_body; return true; }
	{
		auto appIdCheck = std::make_shared<awst::IntrinsicCall>();
		appIdCheck->sourceLocation = method.sourceLocation;
		appIdCheck->opCode = "txn";
		appIdCheck->immediates = {std::string("ApplicationID")};
		appIdCheck->wtype = awst::WType::uint64Type();

		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = method.sourceLocation;
		zero->wtype = awst::WType::uint64Type();
		zero->value = "0";

		auto isCreate = std::make_shared<awst::NumericComparisonExpression>();
		isCreate->sourceLocation = method.sourceLocation;
		isCreate->wtype = awst::WType::boolType();
		isCreate->lhs = appIdCheck;
		isCreate->op = awst::NumericComparison::Eq;
		isCreate->rhs = zero;

		auto createBlock = std::make_shared<awst::Block>();
		createBlock->sourceLocation = method.sourceLocation;

		// Zero-initialize all global state variables (matching EVM implicit zero storage)
		// This must happen before any constructor code runs, because constructors may
		// read state set by base classes (e.g. _transferOwnership reads _owner).
		{
			auto const& linearized = _contract.annotation().linearizedBaseContracts;
			std::set<std::string> initialized;
			for (auto it = linearized.rbegin(); it != linearized.rend(); ++it)
			{
				for (auto const* var: (*it)->stateVariables())
				{
					if (var->isConstant())
						continue;
					if (initialized.count(var->name()))
						continue;
					initialized.insert(var->name());

					auto kind = StorageMapper::shouldUseBoxStorage(*var)
						? awst::AppStorageKind::Box
						: awst::AppStorageKind::AppGlobal;

					// Only zero-initialize global state (not box storage)
					if (kind != awst::AppStorageKind::AppGlobal)
						continue;

					auto* wtype = m_typeMapper.map(var->type());

					// Build key
					auto key = std::make_shared<awst::BytesConstant>();
					key->sourceLocation = method.sourceLocation;
					key->wtype = awst::WType::bytesType();
					key->encoding = awst::BytesEncoding::Utf8;
					std::string keyStr = var->name();
					key->value = std::vector<uint8_t>(keyStr.begin(), keyStr.end());

					// Build default value
					std::shared_ptr<awst::Expression> defaultVal;
					if (wtype == awst::WType::accountType())
					{
						auto addr = std::make_shared<awst::AddressConstant>();
						addr->sourceLocation = method.sourceLocation;
						addr->wtype = awst::WType::accountType();
						addr->value = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ";
						defaultVal = addr;
					}
					else if (wtype == awst::WType::biguintType())
					{
						auto val = std::make_shared<awst::IntegerConstant>();
						val->sourceLocation = method.sourceLocation;
						val->wtype = awst::WType::biguintType();
						val->value = "0";
						defaultVal = val;
					}
					else if (wtype == awst::WType::boolType()
						|| wtype == awst::WType::uint64Type())
					{
						auto val = std::make_shared<awst::IntegerConstant>();
						val->sourceLocation = method.sourceLocation;
						val->wtype = awst::WType::uint64Type();
						val->value = "0";
						defaultVal = val;
					}
					else
					{
						// bytes or other → empty bytes
						auto val = std::make_shared<awst::BytesConstant>();
						val->sourceLocation = method.sourceLocation;
						val->wtype = awst::WType::bytesType();
						val->encoding = awst::BytesEncoding::Base16;
						val->value = {};
						defaultVal = val;
					}

					// app_global_put(key, defaultVal)
					auto put = std::make_shared<awst::IntrinsicCall>();
					put->sourceLocation = method.sourceLocation;
					put->opCode = "app_global_put";
					put->wtype = awst::WType::voidType();
					put->stackArgs.push_back(key);
					put->stackArgs.push_back(defaultVal);

					auto stmt = std::make_shared<awst::ExpressionStatement>();
					stmt->sourceLocation = method.sourceLocation;
					stmt->expr = put;
					createBlock->body.push_back(stmt);
				}
			}
		}

		// Collect explicit base constructor calls from the constructor's modifiers
		auto const* constructor = _contract.constructor();
		std::map<solidity::frontend::ContractDefinition const*,
			std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression>> const*>
			explicitBaseArgs;

		if (constructor)
		{
			// Zero-initialize the constructor's own parameters with type-correct defaults.
			// This is needed because base constructor args may reference these parameters,
			// and Puya defaults uninitialized vars to integer 0 regardless of type.
			for (auto const& param: constructor->parameters())
			{
				auto* paramType = m_typeMapper.map(param->type());
				std::shared_ptr<awst::Expression> defaultVal;

				if (paramType == awst::WType::accountType())
				{
					auto addr = std::make_shared<awst::AddressConstant>();
					addr->sourceLocation = method.sourceLocation;
					addr->wtype = awst::WType::accountType();
					addr->value = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ";
					defaultVal = addr;
				}
				else if (paramType == awst::WType::biguintType())
				{
					auto val = std::make_shared<awst::IntegerConstant>();
					val->sourceLocation = method.sourceLocation;
					val->wtype = awst::WType::biguintType();
					val->value = "0";
					defaultVal = val;
				}
				else if (paramType == awst::WType::uint64Type()
					|| paramType == awst::WType::boolType())
				{
					auto val = std::make_shared<awst::IntegerConstant>();
					val->sourceLocation = method.sourceLocation;
					val->wtype = awst::WType::uint64Type();
					val->value = "0";
					defaultVal = val;
				}
				else
				{
					auto val = std::make_shared<awst::BytesConstant>();
					val->sourceLocation = method.sourceLocation;
					val->wtype = paramType ? paramType : awst::WType::bytesType();
					val->encoding = awst::BytesEncoding::Base16;
					val->value = {};
					defaultVal = val;
				}

				auto target = std::make_shared<awst::VarExpression>();
				target->sourceLocation = method.sourceLocation;
				target->name = param->name();
				target->wtype = paramType;

				auto assignment = std::make_shared<awst::AssignmentStatement>();
				assignment->sourceLocation = method.sourceLocation;
				assignment->target = target;
				assignment->value = defaultVal;
				createBlock->body.push_back(assignment);
			}

			for (auto const& mod: constructor->modifiers())
			{
				auto const* refDecl = mod->name().annotation().referencedDeclaration;
				if (auto const* baseContract =
						dynamic_cast<solidity::frontend::ContractDefinition const*>(refDecl))
				{
					explicitBaseArgs[baseContract] = mod->arguments();
				}
			}
		}

		// Inline base constructor bodies in linearization order (most-base-first)
		auto const& linearized = _contract.annotation().linearizedBaseContracts;
		for (auto it = linearized.rbegin(); it != linearized.rend(); ++it)
		{
			auto const* base = *it;
			if (base == &_contract)
				continue; // Main contract ctor handled separately below

			auto const* baseCtor = base->constructor();
			if (!baseCtor || !baseCtor->isImplemented())
				continue;
			if (baseCtor->body().statements().empty())
				continue;

			// Generate parameter assignments from explicit base constructor arguments
			auto argIt = explicitBaseArgs.find(base);
			if (argIt != explicitBaseArgs.end() && argIt->second && !argIt->second->empty())
			{
				auto const& args = *(argIt->second);
				auto const& params = baseCtor->parameters();
				for (size_t i = 0; i < args.size() && i < params.size(); ++i)
				{
					auto argExpr = m_exprTranslator->translate(*args[i]);
					if (!argExpr)
						continue;

					auto target = std::make_shared<awst::VarExpression>();
					target->sourceLocation = makeLoc(args[i]->location());
					target->name = params[i]->name();
					target->wtype = m_typeMapper.map(params[i]->type());

					// Cast argument to parameter type if needed
					argExpr = ExpressionTranslator::implicitNumericCast(
						std::move(argExpr), target->wtype, target->sourceLocation
					);

					auto assignment = std::make_shared<awst::AssignmentStatement>();
					assignment->sourceLocation = target->sourceLocation;
					assignment->target = target;
					assignment->value = std::move(argExpr);
					createBlock->body.push_back(std::move(assignment));
				}
			}

			// Translate the base constructor body
			auto baseBody = m_stmtTranslator->translateBlock(baseCtor->body());
			for (auto& stmt: baseBody->body)
				createBlock->body.push_back(std::move(stmt));
		}

		// Include main contract constructor body if present
		if (constructor && constructor->body().statements().size() > 0)
		{
			auto ctorBody = m_stmtTranslator->translateBlock(constructor->body());
			for (auto& stmt: ctorBody->body)
				createBlock->body.push_back(std::move(stmt));
		}

		// Return true to complete the create transaction
		auto trueLit = std::make_shared<awst::BoolConstant>();
		trueLit->sourceLocation = method.sourceLocation;
		trueLit->wtype = awst::WType::boolType();
		trueLit->value = true;

		auto createReturn = std::make_shared<awst::ReturnStatement>();
		createReturn->sourceLocation = method.sourceLocation;
		createReturn->value = trueLit;
		createBlock->body.push_back(createReturn);

		auto ifCreate = std::make_shared<awst::IfElse>();
		ifCreate->sourceLocation = method.sourceLocation;
		ifCreate->condition = isCreate;
		ifCreate->ifBranch = createBlock;

		body->body.push_back(ifCreate);
	}

	// ARC4 router
	auto routerExpr = std::make_shared<awst::ARC4Router>();
	routerExpr->sourceLocation = method.sourceLocation;
	routerExpr->wtype = awst::WType::boolType();

	auto routerReturn = std::make_shared<awst::ReturnStatement>();
	routerReturn->sourceLocation = method.sourceLocation;
	routerReturn->value = routerExpr;

	body->body.push_back(routerReturn);

	method.body = body;

	return method;
}

awst::ContractMethod ContractTranslator::buildClearProgram(
	solidity::frontend::ContractDefinition const& _contract,
	std::string const& _contractName
)
{
	awst::ContractMethod method;
	method.sourceLocation = makeLoc(_contract.location());
	method.returnType = awst::WType::boolType();
	method.cref = m_sourceFile + "." + _contractName;
	method.memberName = "clear_state_program";

	auto body = std::make_shared<awst::Block>();
	body->sourceLocation = method.sourceLocation;

	// return true
	auto trueLit = std::make_shared<awst::BoolConstant>();
	trueLit->sourceLocation = method.sourceLocation;
	trueLit->wtype = awst::WType::boolType();
	trueLit->value = true;

	auto ret = std::make_shared<awst::ReturnStatement>();
	ret->sourceLocation = method.sourceLocation;
	ret->value = trueLit;

	body->body.push_back(ret);
	method.body = body;

	return method;
}

awst::ContractMethod ContractTranslator::translateFunction(
	solidity::frontend::FunctionDefinition const& _func,
	std::string const& _contractName
)
{
	awst::ContractMethod method;
	method.sourceLocation = makeLoc(_func.location());
	method.cref = m_sourceFile + "." + _contractName;
	// Disambiguate overloaded function names by appending parameter count
	method.memberName = _func.name();
	if (m_overloadedNames.count(_func.name()))
		method.memberName += "(" + std::to_string(_func.parameters().size()) + ")";

	// Documentation
	if (_func.documentation())
		method.documentation.description = *_func.documentation()->text();

	// Parameters
	for (auto const& param: _func.parameters())
	{
		awst::SubroutineArgument arg;
		arg.name = param->name();
		arg.sourceLocation = makeLoc(param->location());
		arg.wtype = m_typeMapper.map(param->type());
		method.args.push_back(std::move(arg));
	}

	// Return type
	auto const& returnParams = _func.returnParameters();
	if (returnParams.empty())
		method.returnType = awst::WType::voidType();
	else if (returnParams.size() == 1)
		method.returnType = m_typeMapper.map(returnParams[0]->type());
	else
	{
		// Multiple return values → tuple
		std::vector<awst::WType const*> types;
		std::vector<std::string> names;
		bool hasNames = false;
		for (auto const& rp: returnParams)
		{
			types.push_back(m_typeMapper.map(rp->type()));
			names.push_back(rp->name());
			if (!rp->name().empty())
				hasNames = true;
		}
		if (hasNames)
			method.returnType = new awst::WTuple(std::move(types), std::move(names));
		else
			method.returnType = new awst::WTuple(std::move(types));
	}

	// Pure/view
	method.pure = _func.stateMutability() == solidity::frontend::StateMutability::Pure;

	// ARC4 method config for public/external functions
	method.arc4MethodConfig = buildARC4Config(_func, method.sourceLocation);

	// Function body
	if (_func.isImplemented())
	{
		method.body = m_stmtTranslator->translateBlock(_func.body());

		// Inline modifiers
		inlineModifiers(_func, method.body);

		// Synthesize implicit return for named return parameters
		auto const& returnParams = _func.returnParameters();
		if (!method.body->body.empty()
			&& !dynamic_cast<awst::ReturnStatement const*>(method.body->body.back().get())
			&& !returnParams.empty())
		{
			bool hasNames = false;
			for (auto const& rp: returnParams)
				if (!rp->name().empty())
					hasNames = true;

			if (hasNames)
			{
				auto implicitReturn = std::make_shared<awst::ReturnStatement>();
				implicitReturn->sourceLocation = method.sourceLocation;

				if (returnParams.size() == 1)
				{
					auto var = std::make_shared<awst::VarExpression>();
					var->sourceLocation = method.sourceLocation;
					var->name = returnParams[0]->name();
					var->wtype = m_typeMapper.map(returnParams[0]->type());
					implicitReturn->value = std::move(var);
				}
				else
				{
					auto tuple = std::make_shared<awst::TupleExpression>();
					tuple->sourceLocation = method.sourceLocation;
					for (auto const& rp: returnParams)
					{
						auto var = std::make_shared<awst::VarExpression>();
						var->sourceLocation = method.sourceLocation;
						var->name = rp->name();
						var->wtype = m_typeMapper.map(rp->type());
						tuple->items.push_back(std::move(var));
					}
					tuple->wtype = method.returnType;
					implicitReturn->value = std::move(tuple);
				}

				method.body->body.push_back(std::move(implicitReturn));
			}
		}
	}
	else
	{
		// Abstract function — empty body
		Logger::instance().debug("function '" + method.memberName + "' has no implementation", method.sourceLocation);
		method.body = std::make_shared<awst::Block>();
		method.body->sourceLocation = method.sourceLocation;
	}

	return method;
}

std::optional<awst::ARC4MethodConfig> ContractTranslator::buildARC4Config(
	solidity::frontend::FunctionDefinition const& _func,
	awst::SourceLocation const& _loc
)
{
	using namespace solidity::frontend;

	auto vis = _func.visibility();

	if (vis == Visibility::Private || vis == Visibility::Internal)
		return std::nullopt;

	// Skip ARC4 config for methods with non-ARC4-compatible parameter types
	for (auto const& param: _func.parameters())
	{
		auto* ptype = m_typeMapper.map(param->type());
		if (ptype && ptype->kind() == awst::WTypeKind::ReferenceArray)
			return std::nullopt;
		if (ptype && ptype->kind() == awst::WTypeKind::WTuple)
			return std::nullopt;
	}

	// Public/external functions get ARC4 ABI method configs
	awst::ARC4ABIMethodConfig config;
	config.sourceLocation = _loc;
	config.name = _func.name();
	config.allowedCompletionTypes = {0}; // NoOp
	config.create = 3; // Disallow

	// View functions are readonly
	if (_func.stateMutability() == StateMutability::View ||
		_func.stateMutability() == StateMutability::Pure)
	{
		config.readonly = true;
	}

	return awst::ARC4MethodConfig(config);
}

void ContractTranslator::inlineModifiers(
	solidity::frontend::FunctionDefinition const& _func,
	std::shared_ptr<awst::Block>& _body
)
{
	// For each modifier invocation, wrap the function body
	for (auto const& modInvocation: _func.modifiers())
	{
		auto const* modDef = dynamic_cast<solidity::frontend::ModifierDefinition const*>(
			modInvocation->name().annotation().referencedDeclaration
		);

		if (!modDef)
			continue;

		// Translate modifier body, replacing `_` (PlaceholderStatement) with the original body
		auto modBody = std::make_shared<awst::Block>();
		modBody->sourceLocation = makeLoc(modDef->location());

		if (modDef->body().statements().empty())
			continue;

		for (auto const& stmt: modDef->body().statements())
		{
			if (dynamic_cast<solidity::frontend::PlaceholderStatement const*>(stmt.get()))
			{
				// Replace placeholder with the original function body
				for (auto const& bodyStmt: _body->body)
					modBody->body.push_back(bodyStmt);
			}
			else
			{
				auto translated = m_stmtTranslator->translate(*stmt);
				if (translated)
					modBody->body.push_back(std::move(translated));
			}
		}

		_body = modBody;
	}
}

} // namespace puyasol::builder

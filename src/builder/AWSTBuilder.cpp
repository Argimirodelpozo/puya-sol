#include "builder/AWSTBuilder.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

#include <set>
#include <queue>

namespace puyasol::builder
{

std::vector<std::shared_ptr<awst::RootNode>> AWSTBuilder::build(
	solidity::frontend::CompilerStack& _compiler,
	std::string const& _sourceFile
)
{
	m_storageMapper = std::make_unique<StorageMapper>(m_typeMapper);
	m_libraryFunctionIds.clear();
	std::vector<std::shared_ptr<awst::RootNode>> roots;

	// First pass: register all library function IDs (before translating any bodies)
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& sourceUnit = _compiler.ast(sourceName);

		for (auto const& node: sourceUnit.nodes())
		{
			auto const* contract = dynamic_cast<solidity::frontend::ContractDefinition const*>(
				node.get()
			);

			if (!contract || !contract->isLibrary())
				continue;

			std::string libraryName = contract->name();

			// Detect overloaded function names
			std::unordered_map<std::string, int> nameCount;
			for (auto const* func: contract->definedFunctions())
			{
				if (!func->isImplemented())
					continue;
				nameCount[libraryName + "." + func->name()]++;
			}

			for (auto const* func: contract->definedFunctions())
			{
				if (!func->isImplemented())
					continue;

				std::string baseName = libraryName + "." + func->name();
				std::string qualifiedName = baseName;
				std::string subroutineId = _sourceFile + "." + baseName;
				// Disambiguate overloaded functions by parameter count
				if (nameCount[baseName] > 1)
				{
					qualifiedName += "(" + std::to_string(func->parameters().size()) + ")";
					subroutineId += "(" + std::to_string(func->parameters().size()) + ")";
				}
				m_libraryFunctionIds[qualifiedName] = subroutineId;
			}
		}
	}

	// Second pass: translate library functions as Subroutine root nodes
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& sourceUnit = _compiler.ast(sourceName);

		for (auto const& node: sourceUnit.nodes())
		{
			auto const* contract = dynamic_cast<solidity::frontend::ContractDefinition const*>(
				node.get()
			);

			if (!contract || !contract->isLibrary())
				continue;

			std::string libraryName = contract->name();
			Logger::instance().info("Translating library: " + libraryName);

			for (auto const* func: contract->definedFunctions())
			{
				if (!func->isImplemented())
					continue;

				std::string qualifiedName = libraryName + "." + func->name();
				std::string subroutineId;
				auto it = m_libraryFunctionIds.find(qualifiedName);
				if (it != m_libraryFunctionIds.end())
				{
					subroutineId = it->second;
				}
				else
				{
					// Try disambiguated name for overloaded functions
					std::string overloadName = qualifiedName + "(" + std::to_string(func->parameters().size()) + ")";
					auto it2 = m_libraryFunctionIds.find(overloadName);
					if (it2 != m_libraryFunctionIds.end())
					{
						qualifiedName = overloadName;
						subroutineId = it2->second;
					}
					else
						subroutineId = _sourceFile + "." + qualifiedName;
				}

				Logger::instance().debug("Translating library function: " + qualifiedName);

				auto sub = std::make_shared<awst::Subroutine>();

				awst::SourceLocation loc;
				loc.file = _sourceFile;
				loc.line = func->location().start >= 0 ? func->location().start : 0;
				loc.endLine = func->location().end >= 0 ? func->location().end : 0;

				sub->sourceLocation = loc;
				sub->id = subroutineId;
				sub->name = qualifiedName;

				// Documentation
				if (func->documentation())
					sub->documentation.description = *func->documentation()->text();

				// Parameters
				for (auto const& param: func->parameters())
				{
					awst::SubroutineArgument arg;
					arg.name = param->name();
					arg.sourceLocation.file = _sourceFile;
					arg.sourceLocation.line = param->location().start >= 0 ? param->location().start : 0;
					arg.sourceLocation.endLine = param->location().end >= 0 ? param->location().end : 0;
					arg.wtype = m_typeMapper.map(param->type());
					sub->args.push_back(std::move(arg));
				}

				// Return type
				auto const& returnParams = func->returnParameters();
				if (returnParams.empty())
					sub->returnType = awst::WType::voidType();
				else if (returnParams.size() == 1)
					sub->returnType = m_typeMapper.map(returnParams[0]->type());
				else
				{
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
						sub->returnType = new awst::WTuple(std::move(types), std::move(names));
					else
						sub->returnType = new awst::WTuple(std::move(types));
				}

				// Pure
				sub->pure = func->stateMutability() == solidity::frontend::StateMutability::Pure;

				// Translate body using ExpressionTranslator and StatementTranslator
				ExpressionTranslator exprTranslator(
					m_typeMapper, *m_storageMapper, _sourceFile, libraryName, m_libraryFunctionIds
				);
				StatementTranslator stmtTranslator(exprTranslator, m_typeMapper, _sourceFile);

				sub->body = stmtTranslator.translateBlock(func->body());

				// Handle assembly-only functions with known semantics
				if (sub->body->body.empty() && func->name() == "efficientKeccak256"
					&& func->parameters().size() == 2)
				{
					// efficientKeccak256(a, b) → return keccak256(concat(a, b))
					auto varA = std::make_shared<awst::VarExpression>();
					varA->sourceLocation = loc;
					varA->name = func->parameters()[0]->name();
					varA->wtype = m_typeMapper.map(func->parameters()[0]->type());

					auto varB = std::make_shared<awst::VarExpression>();
					varB->sourceLocation = loc;
					varB->name = func->parameters()[1]->name();
					varB->wtype = m_typeMapper.map(func->parameters()[1]->type());

					auto concat = std::make_shared<awst::IntrinsicCall>();
					concat->sourceLocation = loc;
					concat->wtype = awst::WType::bytesType();
					concat->opCode = "concat";
					concat->stackArgs.push_back(std::move(varA));
					concat->stackArgs.push_back(std::move(varB));

					auto hash = std::make_shared<awst::IntrinsicCall>();
					hash->sourceLocation = loc;
					hash->wtype = awst::WType::bytesType();
					hash->opCode = "keccak256";
					hash->stackArgs.push_back(std::move(concat));

					// Cast bytes → bytes[32] to match return type
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = sub->returnType;
					cast->expr = std::move(hash);

					auto ret = std::make_shared<awst::ReturnStatement>();
					ret->sourceLocation = loc;
					ret->value = std::move(cast);
					sub->body->body.push_back(std::move(ret));
				}

				// Synthesize implicit return for named return parameters
				if (!sub->body->body.empty()
					&& !dynamic_cast<awst::ReturnStatement const*>(sub->body->body.back().get())
					&& !returnParams.empty())
				{
					auto implicitReturn = std::make_shared<awst::ReturnStatement>();
					implicitReturn->sourceLocation = loc;

					if (returnParams.size() == 1)
					{
						auto var = std::make_shared<awst::VarExpression>();
						var->sourceLocation = loc;
						var->name = returnParams[0]->name();
						var->wtype = m_typeMapper.map(returnParams[0]->type());
						implicitReturn->value = std::move(var);
					}
					else
					{
						auto tuple = std::make_shared<awst::TupleExpression>();
						tuple->sourceLocation = loc;
						std::vector<awst::WType const*> types;
						for (auto const& rp: returnParams)
						{
							auto var = std::make_shared<awst::VarExpression>();
							var->sourceLocation = loc;
							var->name = rp->name();
							var->wtype = m_typeMapper.map(rp->type());
							types.push_back(var->wtype);
							tuple->items.push_back(std::move(var));
						}
						tuple->wtype = sub->returnType;
						implicitReturn->value = std::move(tuple);
					}

					sub->body->body.push_back(std::move(implicitReturn));
				}

				roots.push_back(std::move(sub));
			}
		}
	}

	// Second pass: translate contracts
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& sourceUnit = _compiler.ast(sourceName);

		for (auto const& node: sourceUnit.nodes())
		{
			auto const* contract = dynamic_cast<solidity::frontend::ContractDefinition const*>(
				node.get()
			);

			if (!contract)
				continue;

			// Skip interfaces, abstract contracts, and libraries (already handled)
			if (contract->isInterface())
			{
				Logger::instance().debug("Skipping interface: " + contract->name());
				continue;
			}

			if (contract->abstract())
			{
				Logger::instance().debug("Skipping abstract contract: " + contract->name());
				continue;
			}

			if (contract->isLibrary())
				continue;

			Logger::instance().info("Translating contract: " + contract->name());

			ContractTranslator translator(
				m_typeMapper, *m_storageMapper, _sourceFile, m_libraryFunctionIds
			);
			auto awstContract = translator.translate(*contract);
			roots.push_back(std::move(awstContract));
		}
	}

	// Dead-code elimination: only keep subroutines reachable from contract methods
	// Collect SubroutineID references from an expression tree
	std::function<void(awst::Expression const&, std::set<std::string>&)> collectRefs;
	collectRefs = [&](awst::Expression const& expr, std::set<std::string>& refs) {
		if (auto const* call = dynamic_cast<awst::SubroutineCallExpression const*>(&expr))
		{
			if (auto const* sid = std::get_if<awst::SubroutineID>(&call->target))
				refs.insert(sid->target);
			for (auto const& arg: call->args)
				if (arg.value) collectRefs(*arg.value, refs);
		}
		// Recurse into common expression types that contain sub-expressions
		if (auto const* e = dynamic_cast<awst::UInt64BinaryOperation const*>(&expr))
		{
			if (e->left) collectRefs(*e->left, refs);
			if (e->right) collectRefs(*e->right, refs);
		}
		if (auto const* e = dynamic_cast<awst::BigUIntBinaryOperation const*>(&expr))
		{
			if (e->left) collectRefs(*e->left, refs);
			if (e->right) collectRefs(*e->right, refs);
		}
		if (auto const* e = dynamic_cast<awst::NumericComparisonExpression const*>(&expr))
		{
			if (e->lhs) collectRefs(*e->lhs, refs);
			if (e->rhs) collectRefs(*e->rhs, refs);
		}
		if (auto const* e = dynamic_cast<awst::BytesComparisonExpression const*>(&expr))
		{
			if (e->lhs) collectRefs(*e->lhs, refs);
			if (e->rhs) collectRefs(*e->rhs, refs);
		}
		if (auto const* e = dynamic_cast<awst::BooleanBinaryOperation const*>(&expr))
		{
			if (e->left) collectRefs(*e->left, refs);
			if (e->right) collectRefs(*e->right, refs);
		}
		if (auto const* e = dynamic_cast<awst::Not const*>(&expr))
		{
			if (e->expr) collectRefs(*e->expr, refs);
		}
		if (auto const* e = dynamic_cast<awst::AssertExpression const*>(&expr))
		{
			if (e->condition) collectRefs(*e->condition, refs);
		}
		if (auto const* e = dynamic_cast<awst::AssignmentExpression const*>(&expr))
		{
			if (e->target) collectRefs(*e->target, refs);
			if (e->value) collectRefs(*e->value, refs);
		}
		if (auto const* e = dynamic_cast<awst::ReinterpretCast const*>(&expr))
		{
			if (e->expr) collectRefs(*e->expr, refs);
		}
		if (auto const* e = dynamic_cast<awst::ConditionalExpression const*>(&expr))
		{
			if (e->condition) collectRefs(*e->condition, refs);
			if (e->trueExpr) collectRefs(*e->trueExpr, refs);
			if (e->falseExpr) collectRefs(*e->falseExpr, refs);
		}
		if (auto const* e = dynamic_cast<awst::FieldExpression const*>(&expr))
		{
			if (e->base) collectRefs(*e->base, refs);
		}
		if (auto const* e = dynamic_cast<awst::IndexExpression const*>(&expr))
		{
			if (e->base) collectRefs(*e->base, refs);
			if (e->index) collectRefs(*e->index, refs);
		}
		if (auto const* e = dynamic_cast<awst::IntrinsicCall const*>(&expr))
		{
			for (auto const& arg: e->stackArgs)
				if (arg) collectRefs(*arg, refs);
		}
		if (auto const* e = dynamic_cast<awst::TupleExpression const*>(&expr))
		{
			for (auto const& item: e->items)
				if (item) collectRefs(*item, refs);
		}
		if (auto const* e = dynamic_cast<awst::NamedTupleExpression const*>(&expr))
		{
			for (auto const& [_, v]: e->values)
				if (v) collectRefs(*v, refs);
		}
		if (auto const* e = dynamic_cast<awst::NewArray const*>(&expr))
		{
			for (auto const& v: e->values)
				if (v) collectRefs(*v, refs);
		}
		if (auto const* e = dynamic_cast<awst::ArrayLength const*>(&expr))
		{
			if (e->array) collectRefs(*e->array, refs);
		}
		if (auto const* e = dynamic_cast<awst::ArrayPop const*>(&expr))
		{
			if (e->base) collectRefs(*e->base, refs);
		}
		if (auto const* e = dynamic_cast<awst::BoxValueExpression const*>(&expr))
		{
			if (e->key) collectRefs(*e->key, refs);
		}
	};

	// Collect refs from a statement tree
	std::function<void(awst::Statement const&, std::set<std::string>&)> collectStmtRefs;
	collectStmtRefs = [&](awst::Statement const& stmt, std::set<std::string>& refs) {
		if (auto const* block = dynamic_cast<awst::Block const*>(&stmt))
		{
			for (auto const& s: block->body)
				if (s) collectStmtRefs(*s, refs);
		}
		if (auto const* es = dynamic_cast<awst::ExpressionStatement const*>(&stmt))
		{
			if (es->expr) collectRefs(*es->expr, refs);
		}
		if (auto const* as = dynamic_cast<awst::AssignmentStatement const*>(&stmt))
		{
			if (as->target) collectRefs(*as->target, refs);
			if (as->value) collectRefs(*as->value, refs);
		}
		if (auto const* rs = dynamic_cast<awst::ReturnStatement const*>(&stmt))
		{
			if (rs->value) collectRefs(*rs->value, refs);
		}
		if (auto const* ie = dynamic_cast<awst::IfElse const*>(&stmt))
		{
			if (ie->condition) collectRefs(*ie->condition, refs);
			if (ie->ifBranch) collectStmtRefs(*ie->ifBranch, refs);
			if (ie->elseBranch) collectStmtRefs(*ie->elseBranch, refs);
		}
		if (auto const* wl = dynamic_cast<awst::WhileLoop const*>(&stmt))
		{
			if (wl->condition) collectRefs(*wl->condition, refs);
			if (wl->loopBody) collectStmtRefs(*wl->loopBody, refs);
		}
	};

	// Collect refs from a method body
	auto collectMethodRefs = [&](awst::ContractMethod const& method, std::set<std::string>& refs) {
		if (method.body)
			collectStmtRefs(*method.body, refs);
	};

	// Build reachability set
	std::set<std::string> reachable;
	std::queue<std::string> worklist;

	// Seed with contract method references
	for (auto const& root: roots)
	{
		if (auto const* contract = dynamic_cast<awst::Contract const*>(root.get()))
		{
			std::set<std::string> refs;
			collectMethodRefs(contract->approvalProgram, refs);
			collectMethodRefs(contract->clearProgram, refs);
			for (auto const& method: contract->methods)
				collectMethodRefs(method, refs);
			for (auto const& id: refs)
			{
				reachable.insert(id);
				worklist.push(id);
			}
		}
	}

	// Build ID→subroutine map
	std::unordered_map<std::string, awst::Subroutine const*> subMap;
	for (auto const& root: roots)
	{
		if (auto const* sub = dynamic_cast<awst::Subroutine const*>(root.get()))
			subMap[sub->id] = sub;
	}

	// Transitively find all reachable subroutines
	while (!worklist.empty())
	{
		std::string id = worklist.front();
		worklist.pop();
		auto it = subMap.find(id);
		if (it == subMap.end())
			continue;
		std::set<std::string> refs;
		if (it->second->body)
			collectStmtRefs(*it->second->body, refs);
		for (auto const& ref: refs)
		{
			if (reachable.find(ref) == reachable.end())
			{
				reachable.insert(ref);
				worklist.push(ref);
			}
		}
	}

	// Filter roots: keep contracts and reachable subroutines
	std::vector<std::shared_ptr<awst::RootNode>> filteredRoots;
	for (auto& root: roots)
	{
		if (dynamic_cast<awst::Contract const*>(root.get()))
		{
			filteredRoots.push_back(std::move(root));
		}
		else if (auto const* sub = dynamic_cast<awst::Subroutine const*>(root.get()))
		{
			if (reachable.count(sub->id))
				filteredRoots.push_back(std::move(root));
		}
		else
		{
			filteredRoots.push_back(std::move(root));
		}
	}

	return filteredRoots;
}

} // namespace puyasol::builder

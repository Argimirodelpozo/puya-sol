/// @file InlineAssemblyBuilder.cpp
/// Handles inline assembly blocks and function context setup.

#include "builder/statements/StatementBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

namespace puyasol::builder
{

void StatementBuilder::setFunctionContext(
	std::vector<std::pair<std::string, awst::WType const*>> const& _params,
	awst::WType const* _returnType
)
{
	m_functionParams = _params;
	m_returnType = _returnType;
}

bool StatementBuilder::visit(solidity::frontend::InlineAssembly const& _node)
{
	auto loc = makeLoc(_node.location());

	Logger::instance().debug("translating inline assembly block", loc);

	// Determine context name from source file
	std::string contextName = m_sourceFile;
	auto lastDot = contextName.rfind('.');
	if (lastDot != std::string::npos)
		contextName = contextName.substr(0, lastDot);
	auto lastSlash = contextName.rfind('/');
	if (lastSlash != std::string::npos)
		contextName = contextName.substr(lastSlash + 1);

	// Extract external constant values from the InlineAssembly annotation
	std::map<std::string, std::string> constants;
	auto const& annotation = _node.annotation();
	for (auto const& [yulId, extInfo]: annotation.externalReferences)
	{
		if (!extInfo.declaration)
			continue;

		auto const* varDecl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
			extInfo.declaration
		);
		if (!varDecl || !varDecl->isConstant())
			continue;

		// Get the constant value from the initializer expression
		auto const& initExpr = varDecl->value();
		if (!initExpr)
			continue;

		// Prefer the type annotation's rational value — handles scientific notation
		// (e.g., 1e18, 1e27), subdenominations (365 days), and expressions.
		auto const* exprType = initExpr->annotation().type;
		auto const* ratType = dynamic_cast<solidity::frontend::RationalNumberType const*>(exprType);
		if (ratType && !ratType->isFractional())
		{
			auto const& val = ratType->value();
			solidity::u256 intVal = solidity::u256(val.numerator() / val.denominator());
			std::ostringstream oss;
			oss << intVal;
			std::string name = yulId->name.str();
			constants[name] = oss.str();
		}
		else if (auto const* literal = dynamic_cast<solidity::frontend::Literal const*>(initExpr.get()))
		{
			std::string name = yulId->name.str();
			std::string value = literal->value();

			// Convert hex literal to decimal
			if (value.size() > 2 && value.substr(0, 2) == "0x")
			{
				try
				{
					solidity::u256 numVal(value);
					std::ostringstream oss;
					oss << numVal;
					constants[name] = oss.str();
				}
				catch (...)
				{
					Logger::instance().warning(
						"failed to parse constant " + name + " = " + value, loc
					);
				}
			}
			else
			{
				constants[name] = value;
			}
		}
	}

	// Build augmented params list: input params + non-constant external variables
	// (e.g., named return variables like `bool z` in exactlyOneZero)
	auto augmentedParams = m_functionParams;
	for (auto const& [yulId, extInfo]: annotation.externalReferences)
	{
		if (!extInfo.declaration)
			continue;
		auto const* varDecl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
			extInfo.declaration
		);
		if (!varDecl || varDecl->isConstant())
			continue;

		std::string name = yulId->name.str();
		// Skip if already in params list
		bool found = false;
		for (auto const& [pName, pType]: augmentedParams)
			if (pName == name)
				found = true;
		if (!found)
		{
			auto* type = m_typeMapper.map(varDecl->type());
			augmentedParams.emplace_back(name, type);
		}
	}

	AssemblyBuilder asmTranslator(m_typeMapper, m_sourceFile, contextName);

	auto stmts = asmTranslator.buildBlock(
		_node.operations().root(),
		augmentedParams,
		m_returnType,
		constants
	);

	for (auto& stmt: stmts)
		push(std::move(stmt));

	return false;
}

} // namespace puyasol::builder

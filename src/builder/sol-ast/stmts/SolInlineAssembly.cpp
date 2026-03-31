/// @file SolInlineAssembly.cpp
/// Migrated from InlineAssemblyBuilder.cpp.

#include "builder/sol-ast/stmts/SolInlineAssembly.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

#include <libsolidity/ast/Types.h>
#include <libsolutil/Numeric.h>

namespace
{
solidity::frontend::IntegerType const* resolveIntegerType(solidity::frontend::Type const* _type)
{
	if (!_type) return nullptr;
	if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(_type))
		return intType;
	if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(_type))
		return dynamic_cast<solidity::frontend::IntegerType const*>(&udvt->underlyingType());
	return nullptr;
}
}

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

SolInlineAssembly::SolInlineAssembly(
	StatementContext& _ctx,
	InlineAssembly const& _node,
	awst::SourceLocation _loc,
	std::vector<std::pair<std::string, awst::WType const*>> const& _functionParams,
	awst::WType const* _returnType,
	std::map<std::string, unsigned> const& _functionParamBitWidths)
	: SolStatement(_ctx, std::move(_loc)), m_node(_node),
	  m_functionParams(_functionParams), m_returnType(_returnType),
	  m_functionParamBitWidths(_functionParamBitWidths)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolInlineAssembly::toAwst()
{
	Logger::instance().debug("translating inline assembly block", m_loc);

	std::string contextName = m_ctx.sourceFile;
	auto lastDot = contextName.rfind('.');
	if (lastDot != std::string::npos)
		contextName = contextName.substr(0, lastDot);
	auto lastSlash = contextName.rfind('/');
	if (lastSlash != std::string::npos)
		contextName = contextName.substr(lastSlash + 1);

	// Extract constant values from external references
	std::map<std::string, std::string> constants;
	auto const& annotation = m_node.annotation();
	for (auto const& [yulId, extInfo]: annotation.externalReferences)
	{
		if (!extInfo.declaration) continue;
		auto const* varDecl = dynamic_cast<VariableDeclaration const*>(extInfo.declaration);
		if (!varDecl || !varDecl->isConstant()) continue;
		auto const& initExpr = varDecl->value();
		if (!initExpr) continue;

		auto const* exprType = initExpr->annotation().type;
		auto const* ratType = dynamic_cast<RationalNumberType const*>(exprType);
		if (ratType && !ratType->isFractional())
		{
			auto const& val = ratType->value();
			solidity::u256 intVal = solidity::u256(val.numerator() / val.denominator());
			std::ostringstream oss;
			oss << intVal;
			constants[yulId->name.str()] = oss.str();
		}
		else if (auto const* literal = dynamic_cast<Literal const*>(initExpr.get()))
		{
			std::string name = yulId->name.str();
			std::string value = literal->value();
			if (value.size() > 2 && value.substr(0, 2) == "0x")
			{
				try
				{
					solidity::u256 numVal(value);
					std::ostringstream oss;
					oss << numVal;
					constants[name] = oss.str();
				}
				catch (...) {}
			}
			else
			{
				solidity::u256 numVal = 0;
				for (char ch: value)
					numVal = (numVal << 8) | static_cast<unsigned char>(ch);
				size_t shiftBits = (32 - value.size()) * 8;
				numVal <<= shiftBits;
				std::ostringstream oss;
				oss << numVal;
				constants[name] = oss.str();
			}
		}
	}

	// Build augmented params
	auto augmentedParams = m_functionParams;
	std::map<std::string, unsigned> paramBitWidths;
	for (auto const& [yulId, extInfo]: annotation.externalReferences)
	{
		if (!extInfo.declaration) continue;
		auto const* varDecl = dynamic_cast<VariableDeclaration const*>(extInfo.declaration);
		if (!varDecl || varDecl->isConstant()) continue;

		std::string name = yulId->name.str();
		bool found = false;
		for (auto const& [pName, pType]: augmentedParams)
			if (pName == name) found = true;
		if (!found)
		{
			auto* type = m_ctx.typeMapper->map(varDecl->type());
			augmentedParams.emplace_back(name, type);
		}

		if (auto const* intType = resolveIntegerType(varDecl->annotation().type))
		{
			if (intType->numBits() < 64)
				paramBitWidths[name] = intType->numBits();
		}
	}
	for (auto const& [n, bw]: m_functionParamBitWidths)
		paramBitWidths.emplace(n, bw);

	AssemblyBuilder asmTranslator((*m_ctx.typeMapper), m_ctx.sourceFile, contextName);
	return asmTranslator.buildBlock(
		m_node.operations().root(),
		augmentedParams,
		m_returnType,
		constants,
		paramBitWidths);
}

} // namespace puyasol::builder::sol_ast

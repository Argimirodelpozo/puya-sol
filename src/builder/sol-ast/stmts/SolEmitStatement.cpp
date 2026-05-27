/// @file SolEmitStatement.cpp
/// Migrated from EmitBuilder.cpp.

#include "builder/sol-ast/stmts/SolEmitStatement.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

SolEmitStatement::SolEmitStatement(
	BlockContext& _blk, EmitStatement const& _node, awst::SourceLocation _loc)
	: SolStatement(_blk, std::move(_loc)), m_node(_node)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolEmitStatement::toAwst()
{
	auto const& eventCall = m_node.eventCall();

	// Resolve via solc's ASTNode::referencedDeclaration helper so the same
	// code path handles `emit MyEvent(...)` (Identifier),
	// `emit Lib.MyEvent(...)` (MemberAccess), and IdentifierPath forms.
	auto const* eventDef = dynamic_cast<EventDefinition const*>(
		ASTNode::referencedDeclaration(eventCall.expression()));
	std::string eventName = eventDef ? eventDef->name() : "Event";

	auto arc4SigName = [this](Type const* _type) -> std::string {
		auto* wtype = m_blk.typeMapper().map(_type);
		if (wtype == awst::WType::biguintType()) return "uint256";
		if (wtype == awst::WType::uint64Type()) return "uint64";
		if (wtype == awst::WType::boolType()) return "bool";
		if (wtype == awst::WType::accountType()) return "address";
		if (wtype == awst::WType::bytesType()) return "byte[]";
		if (wtype == awst::WType::stringType()) return "string";
		if (wtype->kind() == awst::WTypeKind::Bytes)
		{
			auto const* bw = static_cast<awst::BytesWType const*>(wtype);
			if (bw->length().has_value())
				return "byte[" + std::to_string(bw->length().value()) + "]";
			return "byte[]";
		}
		return _type->toString(true);
	};

	std::string eventSignature = eventName + "(";
	if (eventDef)
	{
		bool first = true;
		for (auto const& param: eventDef->parameters())
		{
			if (!first) eventSignature += ",";
			eventSignature += arc4SigName(param->type());
			first = false;
		}
	}
	eventSignature += ")";

	struct FieldInfo {
		std::string name;
		awst::WType const* arc4Type;
		std::shared_ptr<awst::Expression> value;
	};
	std::vector<FieldInfo> fields;

	auto const& callArgs = eventCall.arguments();
	auto const& params = eventDef ? eventDef->parameters()
		: std::vector<std::shared_ptr<VariableDeclaration>>{};
	std::vector<std::shared_ptr<awst::Statement>> preStatements;

	for (size_t i = 0; i < callArgs.size(); ++i)
	{
		auto translated = m_blk.builderCtx().build(*callArgs[i]);

		// Enum range validation: EVM panics (0x21) on invalid enum values in events
		if (i < params.size())
		{
			auto const* paramSolType = params[i]->annotation().type;
			if (auto const* enumType = dynamic_cast<EnumType const*>(paramSolType))
			{
				unsigned numMembers = enumType->numberOfMembers();
				auto val = builder::TypeCoercion::implicitNumericCast(translated, awst::WType::uint64Type(), m_loc);

				auto maxVal = awst::makeIntegerConstant(numMembers, m_loc);

				auto cmp = awst::makeNumericCompare(val, awst::NumericComparison::Lt, std::move(maxVal), m_loc);

				auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), m_loc, "enum out of range"), m_loc);
				preStatements.push_back(std::move(assertStmt));

				translated = std::move(val);
			}
		}

		auto* arc4Type = m_blk.typeMapper().mapToARC4Type(translated->wtype);

		std::shared_ptr<awst::Expression> arc4Value;
		if (translated->wtype->kind() >= awst::WTypeKind::ARC4UIntN
			&& translated->wtype->kind() <= awst::WTypeKind::ARC4Struct)
			arc4Value = std::move(translated);
		else
		{
			auto encode = awst::makeARC4Encode(std::move(translated), arc4Type, m_loc);
			arc4Value = std::move(encode);
		}

		std::string fieldName = (i < params.size() && !params[i]->name().empty())
			? params[i]->name() : "_" + std::to_string(i);
		fields.push_back({fieldName, arc4Type, std::move(arc4Value)});
	}

	if (fields.empty())
	{
		// Zero-argument event: raw log with 4-byte ARC-28 selector
		auto sigBytes = awst::makeUtf8BytesConstant(eventSignature, m_loc);

		auto hash = awst::makeKeccak256(std::move(sigBytes), m_loc);

		auto zero = awst::makeZero(m_loc);
		auto four = awst::makeIntegerConstant("4", m_loc);

		auto selector = awst::makeExtract3(std::move(hash), std::move(zero), std::move(four), m_loc);
		auto logCall = awst::makeIntrinsicCall("log", awst::WType::voidType(), m_loc);
		logCall->stackArgs.push_back(std::move(selector));

		auto stmt = awst::makeExpressionStatement(logCall, m_loc);
		return {stmt};
	}

	std::vector<std::pair<std::string, awst::WType const*>> structFields;
	for (auto const& f: fields)
		structFields.emplace_back(f.name, f.arc4Type);
	auto const* structType = m_blk.typeMapper().createType<awst::ARC4Struct>(
		eventName, std::move(structFields), true);

	auto newStruct = awst::makeNewStruct(structType, m_loc);
	for (auto& f: fields)
		newStruct->values[f.name] = std::move(f.value);

	auto emit = awst::makeEmit(eventSignature, std::move(newStruct), m_loc);

	auto stmt = awst::makeExpressionStatement(emit, m_loc);

	std::vector<std::shared_ptr<awst::Statement>> result;
	for (auto& s: preStatements)
		result.push_back(std::move(s));
	result.push_back(std::move(stmt));
	return result;
}

} // namespace puyasol::builder::sol_ast

/// @file SolEmitStatement.cpp
/// Migrated from EmitBuilder.cpp.

#include "builder/sol-ast/stmts/SolEmitStatement.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

SolEmitStatement::SolEmitStatement(
	StatementContext& _ctx, EmitStatement const& _node, awst::SourceLocation _loc)
	: SolStatement(_ctx, std::move(_loc)), m_node(_node)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolEmitStatement::toAwst()
{
	auto const& eventCall = m_node.eventCall();

	std::string eventName;
	if (auto const* ident = dynamic_cast<Identifier const*>(&eventCall.expression()))
		eventName = ident->name();
	else
		eventName = "Event";

	EventDefinition const* eventDef = nullptr;
	if (auto const* ident = dynamic_cast<Identifier const*>(&eventCall.expression()))
		eventDef = dynamic_cast<EventDefinition const*>(ident->annotation().referencedDeclaration);

	auto arc4SigName = [this](Type const* _type) -> std::string {
		auto* wtype = m_ctx.typeMapper->map(_type);
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
	for (size_t i = 0; i < callArgs.size(); ++i)
	{
		auto translated = m_ctx.buildExpr(*callArgs[i]);
		auto* arc4Type = m_ctx.typeMapper->mapToARC4Type(translated->wtype);

		std::shared_ptr<awst::Expression> arc4Value;
		if (translated->wtype->kind() >= awst::WTypeKind::ARC4UIntN
			&& translated->wtype->kind() <= awst::WTypeKind::ARC4Struct)
			arc4Value = std::move(translated);
		else
		{
			auto encode = std::make_shared<awst::ARC4Encode>();
			encode->sourceLocation = m_loc;
			encode->wtype = arc4Type;
			encode->value = std::move(translated);
			arc4Value = std::move(encode);
		}

		std::string fieldName = (i < params.size() && !params[i]->name().empty())
			? params[i]->name() : "_" + std::to_string(i);
		fields.push_back({fieldName, arc4Type, std::move(arc4Value)});
	}

	if (fields.empty())
	{
		// Zero-argument event: raw log with 4-byte ARC-28 selector
		auto sigBytes = std::make_shared<awst::BytesConstant>();
		sigBytes->sourceLocation = m_loc;
		sigBytes->wtype = awst::WType::bytesType();
		sigBytes->encoding = awst::BytesEncoding::Utf8;
		sigBytes->value = std::vector<uint8_t>(eventSignature.begin(), eventSignature.end());

		auto hash = std::make_shared<awst::IntrinsicCall>();
		hash->sourceLocation = m_loc;
		hash->wtype = awst::WType::bytesType();
		hash->opCode = "keccak256";
		hash->stackArgs.push_back(std::move(sigBytes));

		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = m_loc;
		zero->wtype = awst::WType::uint64Type();
		zero->value = "0";
		auto four = std::make_shared<awst::IntegerConstant>();
		four->sourceLocation = m_loc;
		four->wtype = awst::WType::uint64Type();
		four->value = "4";

		auto selector = std::make_shared<awst::IntrinsicCall>();
		selector->sourceLocation = m_loc;
		selector->wtype = awst::WType::bytesType();
		selector->opCode = "extract3";
		selector->stackArgs.push_back(std::move(hash));
		selector->stackArgs.push_back(std::move(zero));
		selector->stackArgs.push_back(std::move(four));

		auto logCall = std::make_shared<awst::IntrinsicCall>();
		logCall->sourceLocation = m_loc;
		logCall->wtype = awst::WType::voidType();
		logCall->opCode = "log";
		logCall->stackArgs.push_back(std::move(selector));

		auto stmt = std::make_shared<awst::ExpressionStatement>();
		stmt->sourceLocation = m_loc;
		stmt->expr = logCall;
		return {stmt};
	}

	std::vector<std::pair<std::string, awst::WType const*>> structFields;
	for (auto const& f: fields)
		structFields.emplace_back(f.name, f.arc4Type);
	auto const* structType = m_ctx.typeMapper->createType<awst::ARC4Struct>(
		eventName, std::move(structFields), true);

	auto newStruct = std::make_shared<awst::NewStruct>();
	newStruct->sourceLocation = m_loc;
	newStruct->wtype = structType;
	for (auto& f: fields)
		newStruct->values[f.name] = std::move(f.value);

	auto emit = std::make_shared<awst::Emit>();
	emit->sourceLocation = m_loc;
	emit->wtype = awst::WType::voidType();
	emit->signature = eventSignature;
	emit->value = std::move(newStruct);

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = m_loc;
	stmt->expr = emit;
	return {stmt};
}

} // namespace puyasol::builder::sol_ast

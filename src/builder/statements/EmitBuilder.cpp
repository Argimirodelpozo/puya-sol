/// @file EmitBuilder.cpp
/// Handles event emit statements.

#include "builder/statements/StatementBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

namespace puyasol::builder
{

bool StatementBuilder::visit(solidity::frontend::EmitStatement const& _node)
{
	auto loc = makeLoc(_node.location());

	auto const& eventCall = _node.eventCall();

	// Extract event name
	std::string eventName;
	if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(&eventCall.expression()))
		eventName = ident->name();
	else
		eventName = "Event";

	// Resolve EventDefinition
	solidity::frontend::EventDefinition const* eventDef = nullptr;
	if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(&eventCall.expression()))
	{
		auto const* decl = ident->annotation().referencedDeclaration;
		eventDef = dynamic_cast<solidity::frontend::EventDefinition const*>(decl);
	}

	// Helper: map a Solidity type to its ARC4 signature name
	auto arc4SigName = [this](solidity::frontend::Type const* _type) -> std::string {
		auto* wtype = m_typeMapper.map(_type);
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

	// Build ARC4 event signature: EventName(arc4type1,arc4type2,...)
	// Include ALL parameters (indexed + non-indexed) in the signature
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

	Logger::instance().debug(
		"event '" + eventName + "' ARC-28 signature: " + eventSignature, loc
	);

	// Collect non-indexed argument expressions and their ARC4 field info
	struct FieldInfo {
		std::string name;
		awst::WType const* arc4Type;
		std::shared_ptr<awst::Expression> value;
	};
	std::vector<FieldInfo> fields;

	auto const& callArgs = eventCall.arguments();
	if (eventDef)
	{
		auto const& params = eventDef->parameters();
		for (size_t i = 0; i < callArgs.size() && i < params.size(); ++i)
		{
			// ARC-28 has no indexed params — include ALL params in the log body
			auto translated = m_exprBuilder.build(*callArgs[i]);
			auto* arc4Type = m_typeMapper.mapToARC4Type(translated->wtype);

			// ARC4Encode the value if it's not already an ARC4 type
			std::shared_ptr<awst::Expression> arc4Value;
			if (translated->wtype->kind() >= awst::WTypeKind::ARC4UIntN
				&& translated->wtype->kind() <= awst::WTypeKind::ARC4Struct)
			{
				arc4Value = std::move(translated);
			}
			else
			{
				auto encode = std::make_shared<awst::ARC4Encode>();
				encode->sourceLocation = loc;
				encode->wtype = arc4Type;
				encode->value = std::move(translated);
				arc4Value = std::move(encode);
			}

			std::string fieldName = params[i]->name().empty()
				? "_" + std::to_string(i)
				: params[i]->name();
			fields.push_back({fieldName, arc4Type, std::move(arc4Value)});
		}
	}
	else
	{
		for (size_t i = 0; i < callArgs.size(); ++i)
		{
			auto translated = m_exprBuilder.build(*callArgs[i]);
			auto* arc4Type = m_typeMapper.mapToARC4Type(translated->wtype);

			std::shared_ptr<awst::Expression> arc4Value;
			if (translated->wtype->kind() >= awst::WTypeKind::ARC4UIntN
				&& translated->wtype->kind() <= awst::WTypeKind::ARC4Struct)
			{
				arc4Value = std::move(translated);
			}
			else
			{
				auto encode = std::make_shared<awst::ARC4Encode>();
				encode->sourceLocation = loc;
				encode->wtype = arc4Type;
				encode->value = std::move(translated);
				arc4Value = std::move(encode);
			}
			fields.push_back({"_" + std::to_string(i), arc4Type, std::move(arc4Value)});
		}
	}

	if (fields.empty())
	{
		// Zero-argument event: emit raw log with just the 4-byte ARC-28 selector.
		// Puya's Emit node requires a non-empty ARC4Struct, so we use a raw log instead.
		// ARC-28 selector = first 4 bytes of keccak256(signature)
		auto sigBytes = std::make_shared<awst::BytesConstant>();
		sigBytes->sourceLocation = loc;
		sigBytes->wtype = awst::WType::bytesType();
		sigBytes->encoding = awst::BytesEncoding::Utf8;
		std::vector<uint8_t> sigVec(eventSignature.begin(), eventSignature.end());
		sigBytes->value = std::move(sigVec);

		// keccak256(signature)
		auto hash = std::make_shared<awst::IntrinsicCall>();
		hash->sourceLocation = loc;
		hash->wtype = awst::WType::bytesType();
		hash->opCode = "keccak256";
		hash->stackArgs.push_back(std::move(sigBytes));

		// extract first 4 bytes: extract3(hash, 0, 4)
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = loc;
		zero->wtype = awst::WType::uint64Type();
		zero->value = "0";
		auto four = std::make_shared<awst::IntegerConstant>();
		four->sourceLocation = loc;
		four->wtype = awst::WType::uint64Type();
		four->value = "4";

		auto selector = std::make_shared<awst::IntrinsicCall>();
		selector->sourceLocation = loc;
		selector->wtype = awst::WType::bytesType();
		selector->opCode = "extract3";
		selector->stackArgs.push_back(std::move(hash));
		selector->stackArgs.push_back(std::move(zero));
		selector->stackArgs.push_back(std::move(four));

		// log(selector)
		auto logCall = std::make_shared<awst::IntrinsicCall>();
		logCall->sourceLocation = loc;
		logCall->wtype = awst::WType::voidType();
		logCall->opCode = "log";
		logCall->stackArgs.push_back(std::move(selector));

		auto stmt = std::make_shared<awst::ExpressionStatement>();
		stmt->sourceLocation = loc;
		stmt->expr = logCall;
		push(stmt);
	}
	else
	{
		// Build ARC4Struct wtype for the event
		std::vector<std::pair<std::string, awst::WType const*>> structFields;
		for (auto const& f: fields)
			structFields.emplace_back(f.name, f.arc4Type);
		auto const* structType = m_typeMapper.createType<awst::ARC4Struct>(
			eventName, std::move(structFields), true
		);

		// Build NewStruct with the ARC4-encoded field values
		auto newStruct = std::make_shared<awst::NewStruct>();
		newStruct->sourceLocation = loc;
		newStruct->wtype = structType;
		for (auto& f: fields)
			newStruct->values[f.name] = std::move(f.value);

		// Emit the ARC-28 event
		auto emit = std::make_shared<awst::Emit>();
		emit->sourceLocation = loc;
		emit->wtype = awst::WType::voidType();
		emit->signature = eventSignature;
		emit->value = std::move(newStruct);

		auto stmt = std::make_shared<awst::ExpressionStatement>();
		stmt->sourceLocation = loc;
		stmt->expr = emit;
		push(stmt);
	}

	return false;
}


} // namespace puyasol::builder

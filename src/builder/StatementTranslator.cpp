#include "builder/StatementTranslator.h"
#include "builder/AssemblyTranslator.h"
#include "builder/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/ASTAnnotations.h>
#include <libsolutil/Numeric.h>
#include <libyul/AST.h>
#include <libyul/YulName.h>

#include <algorithm>
#include <sstream>

namespace puyasol::builder
{

StatementTranslator::StatementTranslator(
	ExpressionTranslator& _exprTranslator,
	TypeMapper& _typeMapper,
	std::string const& _sourceFile
)
	: m_exprTranslator(_exprTranslator), m_typeMapper(_typeMapper), m_sourceFile(_sourceFile)
{
}

std::shared_ptr<awst::Statement> StatementTranslator::translate(
	solidity::frontend::Statement const& _stmt
)
{
	m_stack.clear();
	_stmt.accept(*this);
	if (m_stack.empty())
		return nullptr;
	return pop();
}

std::shared_ptr<awst::Block> StatementTranslator::translateBlock(
	solidity::frontend::Block const& _block
)
{
	auto awstBlock = std::make_shared<awst::Block>();
	awstBlock->sourceLocation = makeLoc(_block.location());

	// Track unchecked blocks for wrapping arithmetic
	bool const wasUnchecked = m_exprTranslator.inUncheckedBlock();
	if (_block.unchecked())
		m_exprTranslator.setInUncheckedBlock(true);

	for (auto const& stmt: _block.statements())
	{
		// Flatten unchecked blocks (and any nested blocks) into the parent
		if (auto const* innerBlock = dynamic_cast<solidity::frontend::Block const*>(stmt.get()))
		{
			auto translatedBlock = translateBlock(*innerBlock);
			for (auto& innerStmt: translatedBlock->body)
				awstBlock->body.push_back(std::move(innerStmt));
		}
		else
		{
			// Use the stack directly instead of translate() to capture ALL
			// statements (pending inner txn submits + the primary statement).
			m_stack.clear();
			stmt->accept(*this);
			for (auto& translated: m_stack)
				if (translated)
					awstBlock->body.push_back(std::move(translated));
			m_stack.clear();
		}
	}

	m_exprTranslator.setInUncheckedBlock(wasUnchecked);
	return awstBlock;
}

void StatementTranslator::push(std::shared_ptr<awst::Statement> _stmt)
{
	m_stack.push_back(std::move(_stmt));
}

std::shared_ptr<awst::Statement> StatementTranslator::pop()
{
	if (m_stack.empty())
		return nullptr;
	auto stmt = m_stack.back();
	m_stack.pop_back();
	return stmt;
}

awst::SourceLocation StatementTranslator::makeLoc(
	solidity::langutil::SourceLocation const& _solLoc
)
{
	awst::SourceLocation loc;
	loc.file = m_sourceFile;
	loc.line = _solLoc.start >= 0 ? _solLoc.start : 0;
	loc.endLine = _solLoc.end >= 0 ? _solLoc.end : 0;
	return loc;
}

bool StatementTranslator::visit(solidity::frontend::Block const& _node)
{
	push(translateBlock(_node));
	return false;
}

bool StatementTranslator::visit(solidity::frontend::ExpressionStatement const& _node)
{
	auto loc = makeLoc(_node.location());
	auto expr = m_exprTranslator.translate(_node.expression());

	// Flush pre-pending statements BEFORE the expression statement
	// (e.g., biguint exponentiation loops that compute values used by the expression)
	for (auto& pending: m_exprTranslator.takePrePendingStatements())
		push(std::move(pending));

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = loc;
	stmt->expr = std::move(expr);
	push(stmt);

	// Pick up any post-pending statements from the expression translator
	// (e.g., array length increment after push)
	for (auto& pending: m_exprTranslator.takePendingStatements())
		push(std::move(pending));

	return false;
}

bool StatementTranslator::visit(solidity::frontend::Return const& _node)
{
	auto loc = makeLoc(_node.location());
	auto stmt = std::make_shared<awst::ReturnStatement>();
	stmt->sourceLocation = loc;

	if (_node.expression())
	{
		stmt->value = m_exprTranslator.translate(*_node.expression());

		// Insert implicit numeric cast to match function return type
		auto const& retAnnotation = dynamic_cast<solidity::frontend::ReturnAnnotation const&>(
			_node.annotation()
		);
		if (retAnnotation.functionReturnParameters
			&& retAnnotation.functionReturnParameters->parameters().size() == 1)
		{
			auto* expectedType = m_typeMapper.map(
				retAnnotation.functionReturnParameters->parameters()[0]->type()
			);
			stmt->value = ExpressionTranslator::implicitNumericCast(
				std::move(stmt->value), expectedType, loc
			);

			// Coerce IntegerConstant → BytesConstant for bytes[N] return types
			if (expectedType && expectedType->kind() == awst::WTypeKind::Bytes
				&& stmt->value->wtype != expectedType)
			{
				auto const* bytesType = dynamic_cast<awst::BytesWType const*>(expectedType);
				auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(stmt->value.get());
				if (bytesType && intConst && bytesType->length().value_or(0) > 0)
				{
					// Convert integer to fixed-size bytes
					int numBytes = bytesType->length().value();
					uint64_t val = std::stoull(intConst->value);
					std::vector<unsigned char> bytes(numBytes, 0);
					for (int i = numBytes - 1; i >= 0 && val > 0; --i)
					{
						bytes[i] = static_cast<unsigned char>(val & 0xFF);
						val >>= 8;
					}
					auto bc = std::make_shared<awst::BytesConstant>();
					bc->sourceLocation = loc;
					bc->wtype = expectedType;
					bc->encoding = awst::BytesEncoding::Base16;
					bc->value = bytes;
					stmt->value = std::move(bc);
				}
				else if (stmt->value->wtype && stmt->value->wtype->kind() == awst::WTypeKind::Bytes)
				{
					// bytes → bytes[N] reinterpret cast
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = expectedType;
					cast->expr = std::move(stmt->value);
					stmt->value = std::move(cast);
				}
			}
		}
	}

	// Flush pre-pending statements (e.g., biguint exponentiation loops)
	for (auto& pending: m_exprTranslator.takePrePendingStatements())
		push(std::move(pending));

	// Pick up any pending statements from the expression translator
	// (e.g., inner transaction submits that must execute before return)
	for (auto& pending: m_exprTranslator.takePendingStatements())
		push(std::move(pending));

	push(stmt);
	return false;
}

bool StatementTranslator::visit(solidity::frontend::IfStatement const& _node)
{
	auto loc = makeLoc(_node.location());
	auto stmt = std::make_shared<awst::IfElse>();
	stmt->sourceLocation = loc;
	stmt->condition = m_exprTranslator.translate(_node.condition());

	// True branch
	auto const& trueBody = _node.trueStatement();
	if (auto const* block = dynamic_cast<solidity::frontend::Block const*>(&trueBody))
		stmt->ifBranch = translateBlock(*block);
	else
	{
		auto syntheticBlock = std::make_shared<awst::Block>();
		syntheticBlock->sourceLocation = makeLoc(trueBody.location());
		auto translated = translate(trueBody);
		if (translated)
			syntheticBlock->body.push_back(std::move(translated));
		stmt->ifBranch = syntheticBlock;
	}

	// False branch (optional)
	if (_node.falseStatement())
	{
		auto const& falseBody = *_node.falseStatement();
		if (auto const* block = dynamic_cast<solidity::frontend::Block const*>(&falseBody))
			stmt->elseBranch = translateBlock(*block);
		else
		{
			auto syntheticBlock = std::make_shared<awst::Block>();
			syntheticBlock->sourceLocation = makeLoc(falseBody.location());
			auto translated = translate(falseBody);
			if (translated)
				syntheticBlock->body.push_back(std::move(translated));
			stmt->elseBranch = syntheticBlock;
		}
	}

	push(stmt);
	return false;
}

bool StatementTranslator::visit(solidity::frontend::WhileStatement const& _node)
{
	auto loc = makeLoc(_node.location());

	if (_node.isDoWhile())
	{
		// do { body } while (cond) → while (true) { body; if (!cond) break; }
		auto loop = std::make_shared<awst::WhileLoop>();
		loop->sourceLocation = loc;

		auto trueLit = std::make_shared<awst::BoolConstant>();
		trueLit->sourceLocation = loc;
		trueLit->wtype = awst::WType::boolType();
		trueLit->value = true;
		loop->condition = trueLit;

		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = makeLoc(_node.body().location());

		// Body statements
		if (auto const* block = dynamic_cast<solidity::frontend::Block const*>(&_node.body()))
		{
			for (auto const& stmt: block->statements())
			{
				auto translated = translate(*stmt);
				if (translated)
					body->body.push_back(std::move(translated));
			}
		}

		// Check condition at end
		auto cond = m_exprTranslator.translate(_node.condition());
		auto notCond = std::make_shared<awst::Not>();
		notCond->sourceLocation = loc;
		notCond->wtype = awst::WType::boolType();
		notCond->expr = std::move(cond);

		auto breakBlock = std::make_shared<awst::Block>();
		breakBlock->sourceLocation = loc;
		breakBlock->body.push_back(std::make_shared<awst::LoopExit>());

		auto ifBreak = std::make_shared<awst::IfElse>();
		ifBreak->sourceLocation = loc;
		ifBreak->condition = notCond;
		ifBreak->ifBranch = breakBlock;

		body->body.push_back(ifBreak);
		loop->loopBody = body;

		push(loop);
	}
	else
	{
		auto loop = std::make_shared<awst::WhileLoop>();
		loop->sourceLocation = loc;
		loop->condition = m_exprTranslator.translate(_node.condition());

		if (auto const* block = dynamic_cast<solidity::frontend::Block const*>(&_node.body()))
			loop->loopBody = translateBlock(*block);
		else
		{
			auto body = std::make_shared<awst::Block>();
			body->sourceLocation = makeLoc(_node.body().location());
			auto translated = translate(_node.body());
			if (translated)
				body->body.push_back(std::move(translated));
			loop->loopBody = body;
		}

		push(loop);
	}

	return false;
}

bool StatementTranslator::visit(solidity::frontend::ForStatement const& _node)
{
	// for (init; cond; post) body → { init; while (cond) { body; post; } }
	auto loc = makeLoc(_node.location());
	auto outerBlock = std::make_shared<awst::Block>();
	outerBlock->sourceLocation = loc;

	// Init
	if (_node.initializationExpression())
	{
		auto init = translate(*_node.initializationExpression());
		if (init)
			outerBlock->body.push_back(std::move(init));
	}

	// While loop
	auto loop = std::make_shared<awst::WhileLoop>();
	loop->sourceLocation = loc;

	// Condition (default true if absent)
	if (_node.condition())
		loop->condition = m_exprTranslator.translate(*_node.condition());
	else
	{
		auto trueLit = std::make_shared<awst::BoolConstant>();
		trueLit->sourceLocation = loc;
		trueLit->wtype = awst::WType::boolType();
		trueLit->value = true;
		loop->condition = trueLit;
	}

	// Loop body
	auto loopBody = std::make_shared<awst::Block>();
	loopBody->sourceLocation = loc;

	if (auto const* block = dynamic_cast<solidity::frontend::Block const*>(&_node.body()))
	{
		for (auto const& stmt: block->statements())
		{
			auto translated = translate(*stmt);
			if (translated)
				loopBody->body.push_back(std::move(translated));
		}
	}
	else
	{
		auto translated = translate(_node.body());
		if (translated)
			loopBody->body.push_back(std::move(translated));
	}

	// Loop post-expression
	if (_node.loopExpression())
	{
		auto post = translate(*_node.loopExpression());
		if (post)
			loopBody->body.push_back(std::move(post));
	}

	loop->loopBody = loopBody;
	outerBlock->body.push_back(loop);
	push(outerBlock);
	return false;
}

bool StatementTranslator::visit(solidity::frontend::Continue const& _node)
{
	auto stmt = std::make_shared<awst::LoopContinue>();
	stmt->sourceLocation = makeLoc(_node.location());
	push(stmt);
	return false;
}

bool StatementTranslator::visit(solidity::frontend::Break const& _node)
{
	auto stmt = std::make_shared<awst::LoopExit>();
	stmt->sourceLocation = makeLoc(_node.location());
	push(stmt);
	return false;
}

bool StatementTranslator::visit(solidity::frontend::VariableDeclarationStatement const& _node)
{
	auto loc = makeLoc(_node.location());

	auto const& declarations = _node.declarations();
	auto const* initialValue = _node.initialValue();

	if (declarations.size() == 1 && declarations[0])
	{
		auto const& decl = *declarations[0];
		auto* type = m_typeMapper.map(decl.type());

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = makeLoc(decl.location());
		target->wtype = type;
		target->name = decl.name();

		std::shared_ptr<awst::Expression> value;
		if (initialValue)
		{
			value = m_exprTranslator.translate(*initialValue);
			// Insert implicit numeric cast if value type differs from target type
			value = ExpressionTranslator::implicitNumericCast(std::move(value), type, loc);
			// Coerce between bytes-compatible types (string → bytes, bytes → bytes[N], etc.)
			if (value->wtype != type && type && type->kind() == awst::WTypeKind::Bytes)
			{
				bool valueIsCompatible = value->wtype == awst::WType::stringType()
					|| (value->wtype && value->wtype->kind() == awst::WTypeKind::Bytes);
				if (valueIsCompatible)
				{
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = type;
					cast->expr = std::move(value);
					value = std::move(cast);
				}
			}
		}
		else
		{
			// Default value
			if (type == awst::WType::boolType())
			{
				auto def = std::make_shared<awst::BoolConstant>();
				def->sourceLocation = loc;
				def->wtype = type;
				def->value = false;
				value = def;
			}
			else if (type == awst::WType::uint64Type())
			{
				auto def = std::make_shared<awst::IntegerConstant>();
				def->sourceLocation = loc;
				def->wtype = type;
				def->value = "0";
				value = def;
			}
			else if (type == awst::WType::biguintType())
			{
				// Use BytesConstant for biguint defaults to prevent the puya
				// backend from folding biguint(0) into a uint64 constant pool
				// entry, which breaks concat/b| when biguint is used as bytes.
				auto def = std::make_shared<awst::BytesConstant>();
				def->sourceLocation = loc;
				def->wtype = type;
				def->encoding = awst::BytesEncoding::Base16;
				def->value = {}; // empty bytes = biguint(0)
				value = def;
			}
			else if (type && type->kind() == awst::WTypeKind::Bytes)
			{
				auto const* bytesType = dynamic_cast<awst::BytesWType const*>(type);
				if (bytesType && bytesType->length())
				{
					// Fixed-length bytes (e.g. bytes32) → zero-filled
					auto def = std::make_shared<awst::BytesConstant>();
					def->sourceLocation = loc;
					def->wtype = type;
					def->encoding = awst::BytesEncoding::Base16;
					def->value = std::vector<uint8_t>(*bytesType->length(), 0);
					value = def;
				}
				else
				{
					// Dynamic bytes → empty
					auto def = std::make_shared<awst::BytesConstant>();
					def->sourceLocation = loc;
					def->wtype = type;
					def->encoding = awst::BytesEncoding::Base16;
					def->value = {};
					value = def;
				}
			}
			else if (type == awst::WType::stringType())
			{
				auto def = std::make_shared<awst::StringConstant>();
				def->sourceLocation = loc;
				def->wtype = type;
				def->value = "";
				value = def;
			}
			else if (type == awst::WType::accountType())
			{
				auto def = std::make_shared<awst::AddressConstant>();
				def->sourceLocation = loc;
				def->wtype = type;
				def->value = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ";
				value = def;
			}
			else if (type && type->kind() == awst::WTypeKind::ReferenceArray)
			{
				auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(type);
				if (refArr && refArr->arraySize())
				{
					auto def = std::make_shared<awst::NewArray>();
					def->sourceLocation = loc;
					def->wtype = type;
					for (int i = 0; i < *refArr->arraySize(); ++i)
						def->values.push_back(
							StorageMapper::makeDefaultValue(refArr->elementType(), loc));
					value = def;
				}
				else
				{
					// Dynamic array (no fixed size)
					auto def = std::make_shared<awst::NewArray>();
					def->sourceLocation = loc;
					def->wtype = type;
					value = def;
				}
			}
			else
			{
				value = StorageMapper::makeDefaultValue(type, loc);
			}
		}

		// Storage pointer alias: Type storage p = _mapping[key]
		// Don't create a local variable — register as alias to the box expression.
		// Later references to `p` will resolve to the box read, so field writes persist.
		if (decl.referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
			&& initialValue)
		{
			if (dynamic_cast<awst::StateGet const*>(value.get())
				|| dynamic_cast<awst::BoxValueExpression const*>(value.get()))
			{
				// Ensure it's wrapped in StateGet for proper read semantics
				auto aliasExpr = value;
				if (auto const* boxVal = dynamic_cast<awst::BoxValueExpression const*>(value.get()))
				{
					auto stateGet = std::make_shared<awst::StateGet>();
					stateGet->sourceLocation = loc;
					stateGet->wtype = boxVal->wtype;
					stateGet->field = value;
					stateGet->defaultValue = StorageMapper::makeDefaultValue(boxVal->wtype, loc);
					aliasExpr = stateGet;
				}
				m_exprTranslator.addStorageAlias(decl.id(), aliasExpr);
				// Emit pre-pending + pending statements but skip the local variable assignment
				for (auto& pending: m_exprTranslator.takePrePendingStatements())
					push(std::move(pending));
				for (auto& pending: m_exprTranslator.takePendingStatements())
					push(std::move(pending));
				return false;
			}
		}

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = loc;
		assign->target = std::move(target);
		assign->value = std::move(value);

		// Flush pre-pending statements (e.g., biguint exponentiation loops)
		for (auto& pending: m_exprTranslator.takePrePendingStatements())
			push(std::move(pending));

		// Pick up any pending statements from the expression translator
		for (auto& pending: m_exprTranslator.takePendingStatements())
			push(std::move(pending));

		push(assign);

		// Flush any pending ArrayExtend statements from large array chunking
		for (auto& pending: m_pendingExtends)
			push(std::move(pending));
		m_pendingExtends.clear();
	}
	else if (declarations.size() > 1 && initialValue)
	{
		// Tuple destructuring: (type1 var1, type2 var2) = expr
		auto rhsExpr = m_exprTranslator.translate(*initialValue);

		// Flush pre-pending statements
		for (auto& pending: m_exprTranslator.takePrePendingStatements())
			push(std::move(pending));

		// Pick up any pending statements (e.g., inner transaction submits)
		for (auto& pending: m_exprTranslator.takePendingStatements())
			push(std::move(pending));

		// Wrap in SingleEvaluation to ensure the RHS is evaluated only once.
		// Without this, each TupleItemExpression would independently evaluate
		// the base expression, causing side effects (like inner txn submits) to repeat.
		auto singleRhs = std::make_shared<awst::SingleEvaluation>();
		singleRhs->sourceLocation = loc;
		singleRhs->wtype = rhsExpr->wtype;
		singleRhs->source = std::move(rhsExpr);
		singleRhs->id = static_cast<int>(_node.id());
		rhsExpr = std::move(singleRhs);

		// Assign each declared variable from the tuple
		for (size_t i = 0; i < declarations.size(); ++i)
		{
			if (!declarations[i])
				continue; // Skip anonymous placeholders (e.g., (bool x, ) = ...)

			auto const& decl = *declarations[i];
			auto* type = m_typeMapper.map(decl.type());

			auto target = std::make_shared<awst::VarExpression>();
			target->sourceLocation = makeLoc(decl.location());
			target->wtype = type;
			target->name = decl.name();

			auto itemExpr = std::make_shared<awst::TupleItemExpression>();
			itemExpr->sourceLocation = loc;
			itemExpr->wtype = type;
			itemExpr->base = rhsExpr;
			itemExpr->index = static_cast<int>(i);

			auto assign = std::make_shared<awst::AssignmentStatement>();
			assign->sourceLocation = loc;
			assign->target = std::move(target);
			assign->value = std::move(itemExpr);
			push(assign);
		}
	}

	return false;
}

bool StatementTranslator::visit(solidity::frontend::EmitStatement const& _node)
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
			auto translated = m_exprTranslator.translate(*callArgs[i]);
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
			auto translated = m_exprTranslator.translate(*callArgs[i]);
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

	return false;
}

bool StatementTranslator::visit(solidity::frontend::RevertStatement const& _node)
{
	auto loc = makeLoc(_node.location());

	// Extract custom error name from the errorCall expression
	std::string errorName = "revert";
	auto const& errorCall = _node.errorCall();
	if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(&errorCall.expression()))
		errorName = ident->name();
	else if (auto const* ma = dynamic_cast<solidity::frontend::MemberAccess const*>(&errorCall.expression()))
		errorName = ma->memberName();

	Logger::instance().debug("revert mapped to assert(false, \"" + errorName + "\")", loc);
	auto assertExpr = std::make_shared<awst::AssertExpression>();
	assertExpr->sourceLocation = loc;
	assertExpr->wtype = awst::WType::voidType();

	auto falseLit = std::make_shared<awst::BoolConstant>();
	falseLit->sourceLocation = loc;
	falseLit->wtype = awst::WType::boolType();
	falseLit->value = false;

	assertExpr->condition = falseLit;
	assertExpr->errorMessage = errorName;

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = loc;
	stmt->expr = assertExpr;
	push(stmt);
	return false;
}

void StatementTranslator::setFunctionContext(
	std::vector<std::pair<std::string, awst::WType const*>> const& _params,
	awst::WType const* _returnType
)
{
	m_functionParams = _params;
	m_returnType = _returnType;
}

bool StatementTranslator::visit(solidity::frontend::InlineAssembly const& _node)
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

	AssemblyTranslator asmTranslator(m_typeMapper, m_sourceFile, contextName);

	auto stmts = asmTranslator.translateBlock(
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

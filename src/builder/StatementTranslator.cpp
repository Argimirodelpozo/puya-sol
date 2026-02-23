#include "builder/StatementTranslator.h"
#include "Logger.h"

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

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = loc;
	stmt->expr = std::move(expr);
	push(stmt);

	// Pick up any pending statements from the expression translator
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
		}
	}

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
			else if (type == awst::WType::uint64Type() || type == awst::WType::biguintType())
			{
				auto def = std::make_shared<awst::IntegerConstant>();
				def->sourceLocation = loc;
				def->wtype = type;
				def->value = "0";
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
				auto def = std::make_shared<awst::NewArray>();
				def->sourceLocation = loc;
				def->wtype = type;
				value = def;
			}
			else
			{
				auto def = std::make_shared<awst::IntegerConstant>();
				def->sourceLocation = loc;
				def->wtype = type;
				def->value = "0";
				value = def;
			}
		}

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = loc;
		assign->target = std::move(target);
		assign->value = std::move(value);

		// Pick up any pending statements from the expression translator
		for (auto& pending: m_exprTranslator.takePendingStatements())
			push(std::move(pending));

		push(assign);
	}
	else if (declarations.size() > 1 && initialValue)
	{
		// Tuple destructuring: (type1 var1, type2 var2) = expr
		auto rhsExpr = m_exprTranslator.translate(*initialValue);

		// Pick up any pending statements (e.g., inner transaction submits)
		for (auto& pending: m_exprTranslator.takePendingStatements())
			push(std::move(pending));

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

	// Emit event → log intrinsic (simplified: just log a comment for now)
	// Full ARC-28 emit requires ARC4Struct encoding which is complex
	auto const& eventCall = _node.eventCall();

	std::string eventNameForLog;
	if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(&eventCall.expression()))
		eventNameForLog = ident->name();
	else
		eventNameForLog = "Event";
	Logger::instance().warning(
		"event '" + eventNameForLog + "' emitted as log() (ARC-28 encoding not yet supported)", loc
	);

	std::string eventName;
	if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(&eventCall.expression()))
		eventName = ident->name();
	else
		eventName = "Event";

	// Create: log(bytes("event:" + eventName))
	auto logCall = std::make_shared<awst::IntrinsicCall>();
	logCall->sourceLocation = loc;
	logCall->wtype = awst::WType::voidType();
	logCall->opCode = "log";

	auto logMsg = std::make_shared<awst::BytesConstant>();
	logMsg->sourceLocation = loc;
	logMsg->wtype = awst::WType::bytesType();
	logMsg->encoding = awst::BytesEncoding::Utf8;
	std::string msg = "event:" + eventName;
	logMsg->value = std::vector<uint8_t>(msg.begin(), msg.end());

	logCall->stackArgs.push_back(logMsg);

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = loc;
	stmt->expr = logCall;
	push(stmt);

	return false;
}

bool StatementTranslator::visit(solidity::frontend::RevertStatement const& _node)
{
	auto loc = makeLoc(_node.location());

	// revert → assert(false, "error message")
	Logger::instance().debug("revert mapped to assert(false)", loc);
	auto assertExpr = std::make_shared<awst::AssertExpression>();
	assertExpr->sourceLocation = loc;
	assertExpr->wtype = awst::WType::voidType();

	auto falseLit = std::make_shared<awst::BoolConstant>();
	falseLit->sourceLocation = loc;
	falseLit->wtype = awst::WType::boolType();
	falseLit->value = false;

	assertExpr->condition = falseLit;
	assertExpr->errorMessage = "revert";

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = loc;
	stmt->expr = assertExpr;
	push(stmt);
	return false;
}

} // namespace puyasol::builder

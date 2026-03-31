#pragma once

#include "builder/sol-ast/SolExpression.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

/// Unary operations: !, ~, -, ++, --, delete.
/// Delegates to sol-eb builders for Not/Sub/BitNot; handles inc/dec/delete inline.
class SolUnaryOperation: public SolExpression
{
public:
	SolUnaryOperation(eb::BuilderContext& _ctx, solidity::frontend::UnaryOperation const& _node);
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	solidity::frontend::UnaryOperation const& m_unaryOp;

	bool isBigUInt(awst::WType const* _type) const;
	std::shared_ptr<awst::Expression> handleNot(std::shared_ptr<awst::Expression> _operand);
	std::shared_ptr<awst::Expression> handleNegate(std::shared_ptr<awst::Expression> _operand);
	std::shared_ptr<awst::Expression> handleBitNot(std::shared_ptr<awst::Expression> _operand);
	std::shared_ptr<awst::Expression> handleIncDec(std::shared_ptr<awst::Expression> _operand);
	std::shared_ptr<awst::Expression> handleDelete(std::shared_ptr<awst::Expression> _operand);
};

} // namespace puyasol::builder::sol_ast

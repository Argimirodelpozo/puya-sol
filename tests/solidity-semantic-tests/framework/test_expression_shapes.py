"""Keep Solidity expression-shape classification behind the grouping boundary."""

import re
from pathlib import Path


def test_expression_shape_queries_use_parenthesis_boundary():
    builder = Path(__file__).resolve().parents[3] / "src" / "builder"
    shapes = ("Identifier|MemberAccess|IndexAccess|IndexRangeAccess|FunctionCall|"
              "TupleExpression|Conditional|Literal|UnaryOperation|Assignment|"
              "NewExpression|BinaryOperation|FunctionCallOptions")
    raw_query = re.compile(r"dynamic_cast\s*<\s*(?:solidity::frontend::)?(?:"
                           + shapes + r")\s+const\s*\*\s*>")
    violations = []
    for path in builder.rglob("*.cpp"):
        if path == builder / "solc" / "SolcFacts.cpp":
            continue  # The canonical query implementation must inspect raw nodes.
        for number, line in enumerate(path.read_text().splitlines(), 1):
            if raw_query.search(line):
                violations.append(f"{path.relative_to(builder)}:{number}")
    assert not violations, "Use SolcFacts::expressionAs<T> for grouping-independent queries: " + ", ".join(violations)

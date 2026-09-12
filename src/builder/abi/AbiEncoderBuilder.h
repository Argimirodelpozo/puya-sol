#pragma once

#include "awst/Node.h"
#include <libsolidity/ast/AST.h>

namespace puyasol::builder::eb
{
class ContractContext;

/// Solidity ABI builtins and explicit ARC4 facade envelopes. EVM layout
/// lives in EvmAbiEncode/Decode; exact ARC4 widths and private payload widths
/// remain distinct wire conventions.
class AbiEncoderBuilder
{
public:
	static std::shared_ptr<awst::Expression> build(
		ContractContext&, solidity::frontend::FunctionCall const&, awst::SourceLocation const&);

	static std::shared_ptr<awst::Expression> encodeValuesAsEvmAbi(
		ContractContext&, std::vector<solidity::frontend::Type const*> const&,
		std::vector<std::shared_ptr<awst::Expression>>, awst::SourceLocation const&);

	/// Evaluate args[first..] in solc order, at their resolved mobile types.
	static std::shared_ptr<awst::Expression> encodeArgsAsEvmAbi(
		ContractContext&,
		std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression const>> const&,
		size_t first, awst::SourceLocation const&, bool packed = false);

	/// Private payload convention: encode values at their native backing widths.
	static std::shared_ptr<awst::Expression> arc4EncodeValues(
		ContractContext&, std::vector<std::shared_ptr<awst::Expression>>, awst::SourceLocation const&);

	/// Explicit ARC4 facade convention: exact Solidity widths, not backing widths.
	static std::shared_ptr<awst::Expression> arc4EncodeSolidityArgs(
		ContractContext&,
		std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression const>> const&,
		awst::SourceLocation const&);
	static std::shared_ptr<awst::Expression> decodeArc4(
		ContractContext&, solidity::frontend::FunctionCall const&,
		solidity::frontend::Expression const&, awst::SourceLocation const&);

private:
	static std::shared_ptr<awst::Expression> handleDecode(
		ContractContext&, solidity::frontend::FunctionCall const&, awst::SourceLocation const&);
};
} // namespace puyasol::builder::eb

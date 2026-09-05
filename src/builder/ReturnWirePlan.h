#pragma once

/// @file ReturnWirePlan.h
/// Return representations derived from solc declarations. TypeMapper owns one
/// immutable signature plan per function; body builders and call sites share it.
/// Element planning is also shared with generated getters and ABI decoding.

#include <vector>

namespace solidity::frontend { class Type; }
namespace puyasol::awst { class WType; }

namespace puyasol::builder
{

class TypeMapper;

struct ReturnWireElem
{
	awst::WType const* nativeType = nullptr;   // element type in method.returnType (post promotion)
	awst::WType const* wireType = nullptr;     // ABI wire type
	bool isSigned = false;                     // sign-extend to 256 bits before encode
	unsigned bits = 0;                         // declared width (wire width + asm mod-wrap + mask)
	bool encoded = false;                      // biguint/array element → needs ARC4Encode
	bool masked = false;                       // unsigned sub-word (uint64 native) → mask to `bits`
};

struct FunctionReturnPlan
{
	awst::WType const* nativeType = nullptr;   // body / modifier-chain representation
	awst::WType const* internalType = nullptr; // caller representation (blob returns use offsets)
	awst::WType const* wireType = nullptr;     // outer ABI method representation
	std::vector<ReturnWireElem> elements;
};

ReturnWireElem planReturnElement(
	TypeMapper& _types,
	solidity::frontend::Type const* _solType,
	awst::WType const* _nativeType);

/// ABI return integers use canonical 256-bit two's complement when signed.
awst::WType const* abiReturnNativeType(
	TypeMapper& _types, solidity::frontend::Type const* _solType);

} // namespace puyasol::builder

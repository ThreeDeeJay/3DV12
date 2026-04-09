#pragma once
// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later
//
// DXBC container layout (D3D10/11/12, SM4/5/5.1):
//
//   DXBCHeader              (magic "DXBC", MD5 checksum, version, totalSize, chunkCount)
//   uint32_t chunkOffsets[] (chunkCount entries, from start of file)
//   -- chunks --
//   Each chunk: { uint32_t fourCC; uint32_t dataSize; uint8_t data[dataSize]; }
//
// Relevant fourCCs:
//   RDEF  – resource / binding declarations
//   ISGN  – input signature
//   OSGN  – output signature
//   SHDR  – shader bytecode (SM4/5.0)
//   SHEX  – shader bytecode (SM5.0 extended flags variant)
//   DXIL  – DXIL / LLVM bitcode (SM6+, requires DXC to modify)
//   STAT  – statistics
//   OSG5  – output signature for GS
//   SFI0  – shader feature info

#include <cstdint>
#include <vector>
#include <optional>
#include <string>

// ---------------------------------------------------------------------------
// DXBC on-disk structures (packed, little-endian)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)

struct DXBCHeader {
    uint32_t magic;        // 0x43425844 = "DXBC"
    uint32_t checksum[4];  // MD5 of content after this point
    uint32_t one;          // always 1
    uint32_t totalSize;    // bytes including header
    uint32_t chunkCount;
    // Immediately followed by uint32_t chunkOffsets[chunkCount]
    // Offsets are from the very start of the DXBC file.
};

struct DXBCChunkHeader {
    uint32_t fourCC;
    uint32_t dataSize; // bytes of data following this header
};

#pragma pack(pop)

// FourCC helpers
constexpr uint32_t FOURCC(char a, char b, char c, char d) {
    return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
}
constexpr uint32_t kFourCC_DXBC = FOURCC('D','X','B','C');
constexpr uint32_t kFourCC_RDEF = FOURCC('R','D','E','F');
constexpr uint32_t kFourCC_ISGN = FOURCC('I','S','G','N');
constexpr uint32_t kFourCC_OSGN = FOURCC('O','S','G','N');
constexpr uint32_t kFourCC_SHDR = FOURCC('S','H','D','R');
constexpr uint32_t kFourCC_SHEX = FOURCC('S','H','E','X');
constexpr uint32_t kFourCC_DXIL = FOURCC('D','X','I','L');
constexpr uint32_t kFourCC_STAT = FOURCC('S','T','A','T');

// ---------------------------------------------------------------------------
// SM4/5 opcode identifiers (D3D10_SB_OPCODE_TYPE)
// From d3d10tokenizedprogramformat.hpp (Windows SDK)
// ---------------------------------------------------------------------------
enum D3D10_SB_OPCODE : uint32_t {
    OP_ADD                  = 0,
    OP_DIV                  = 14,
    OP_DP4                  = 17,
    OP_MAD                  = 50,   // 0x32
    OP_MOV                  = 54,   // 0x36
    OP_MUL                  = 56,   // 0x38
    OP_RET                  = 62,   // 0x3E
    OP_SAMPLE               = 69,
    OP_DCL_RESOURCE         = 88,   // 0x58
    OP_DCL_CONSTANT_BUFFER  = 89,   // 0x59
    OP_DCL_SAMPLER          = 90,
    OP_DCL_INDEX_RANGE      = 91,
    OP_DCL_OUTPUT_TOPOLOGY  = 92,
    OP_DCL_INPUT_PRIMITIVE  = 93,
    OP_DCL_OUTPUT_VERTEX    = 94,
    OP_DCL_INPUT            = 95,   // 0x5F
    OP_DCL_INPUT_SGV        = 96,
    OP_DCL_INPUT_SIV        = 97,
    OP_DCL_INPUT_PS         = 98,
    OP_DCL_INPUT_PS_SGV     = 99,
    OP_DCL_INPUT_PS_SIV     = 100,
    OP_DCL_OUTPUT           = 101,  // 0x65
    OP_DCL_OUTPUT_SGV       = 102,
    OP_DCL_OUTPUT_SIV       = 103,  // 0x67
    OP_DCL_TEMPS            = 104,  // 0x68
    OP_DCL_INDEXABLE_TEMP   = 105,
    OP_DCL_GLOBAL_FLAGS     = 106,
};

// SM5 additional opcodes
enum D3D11_SB_OPCODE : uint32_t {
    OP_DCL_STREAM                      = 0x6B,
    OP_DCL_FUNCTION_BODY               = 0x6C,
    OP_DCL_FUNCTION_TABLE              = 0x6D,
    OP_DCL_INTERFACE                   = 0x6E,
    OP_DCL_INPUT_CONTROL_POINT_COUNT   = 0x6F,
    OP_DCL_OUTPUT_CONTROL_POINT_COUNT  = 0x70,
    OP_DCL_TESS_DOMAIN                 = 0x71,
    OP_DCL_TESS_PARTITIONING           = 0x72,
    OP_DCL_TESS_OUTPUT_PRIMITIVE       = 0x73,
    OP_DCL_HS_MAX_TESSFACTOR           = 0x74,
    OP_DCL_HS_FORK_PHASE_INSTANCE_COUNT= 0x75,
    OP_DCL_HS_JOIN_PHASE_INSTANCE_COUNT= 0x76,
    OP_DCL_THREAD_GROUP                = 0x77,
    OP_DCL_UAV_TYPED                   = 0x78,
    OP_DCL_UAV_RAW                     = 0x79,
    OP_DCL_UAV_STRUCTURED              = 0x7A,
    OP_DCL_THREAD_GROUP_SHARED_MEMORY_RAW        = 0x7B,
    OP_DCL_THREAD_GROUP_SHARED_MEMORY_STRUCTURED = 0x7C,
    OP_DCL_RESOURCE_RAW                = 0x7D,
    OP_DCL_RESOURCE_STRUCTURED         = 0x7E,
    OP_LD_UAV_TYPED                    = 0x7F,
    OP_STORE_UAV_TYPED                 = 0x80,
    OP_LD_RAW                          = 0x81,
    OP_STORE_RAW                       = 0x82,
    OP_LD_STRUCTURED                   = 0x83,
    OP_STORE_STRUCTURED                = 0x84,
    OP_ATOMIC_AND                      = 0x85,
    OP_DISPATCH                        = 0x8C,
};

// ---------------------------------------------------------------------------
// SM4/5 operand type identifiers (D3D10_SB_OPERAND_TYPE)
// ---------------------------------------------------------------------------
enum D3D10_SB_OPERAND_TYPE : uint32_t {
    OPT_TEMP             = 0,
    OPT_INPUT            = 1,
    OPT_OUTPUT           = 2,
    OPT_INDEXABLE_TEMP   = 3,
    OPT_IMMEDIATE32      = 4,
    OPT_IMMEDIATE64      = 5,
    OPT_SAMPLER          = 6,
    OPT_RESOURCE         = 7,
    OPT_CONSTANT_BUFFER  = 8,
    OPT_IMMEDIATE_CB     = 9,
    OPT_LABEL            = 10,
    OPT_INPUT_PRIMID     = 11,
    OPT_OUTPUT_DEPTH     = 12,
    OPT_NULL             = 13,
    OPT_RASTERIZER       = 14,
    OPT_OUTPUT_COVERAGE  = 15,
};

// Component selection modes
enum D3D10_SB_OPERAND_SELECTION_MODE : uint32_t {
    COMP_MASK   = 0,
    COMP_SWIZZLE= 1,
    COMP_SELECT1= 2,
};

// Standard xyzw swizzle pattern = 0b 11 10 01 00 = 0xE4
constexpr uint32_t SWIZZLE_XYZW = 0xE4u;
// Standard xyzw write mask (all 4 components)
constexpr uint32_t MASK_XYZW = 0xF;
constexpr uint32_t MASK_X    = 0x1;

// OperandToken0 index dimension
enum D3D10_SB_OPERAND_INDEX_DIMENSION : uint32_t {
    IDX_0D = 0,
    IDX_1D = 1,
    IDX_2D = 2,
    IDX_3D = 3,
};
// Index representation (within operand index slots)
enum D3D10_SB_INDEX_REPR : uint32_t {
    IDX_IMMED32       = 0,
    IDX_IMMED64       = 1,
    IDX_RELATIVE      = 2,
    IDX_IMMED32_PLUS  = 3,
    IDX_IMMED64_PLUS  = 4,
};
// Extended operand modifier token bit patterns (bits [5:3])
enum D3D10_SB_EXTENDED_MODIFIER : uint32_t {
    MOD_NONE    = 0,
    MOD_NEGATE  = 1,
    MOD_ABS     = 2,
    MOD_ABS_NEG = 3,
};

// ---------------------------------------------------------------------------
// Operand builder helpers
// ---------------------------------------------------------------------------

// Build OperandToken0 for a 4-component register (temp or output).
// swizzleOrMask: used as swizzle bits (for source) or mask bits (for dest).
// selMode: COMP_MASK for dest, COMP_SWIZZLE for source, COMP_SELECT1 for single-component src.
inline uint32_t MakeOperandToken0_Register(
    D3D10_SB_OPERAND_TYPE type,
    D3D10_SB_OPERAND_SELECTION_MODE selMode,
    uint32_t swizzleOrMask,
    D3D10_SB_OPERAND_INDEX_DIMENSION indexDim,
    bool extended = false)
{
    // [1:0]  = 2 (D3D10_SB_OPERAND_4_COMPONENT)
    // [3:2]  = selMode
    // [11:4] = swizzleOrMask
    // [19:12]= type
    // [21:20]= indexDim
    // [24:22]= index0 repr (IMMED32=0)
    // [27:25]= index1 repr if 2D
    // [31]   = extended flag
    uint32_t tok = 0;
    tok |= (2u << 0);
    tok |= ((uint32_t)selMode << 2);
    tok |= (swizzleOrMask << 4);
    tok |= ((uint32_t)type << 12);
    tok |= ((uint32_t)indexDim << 20);
    tok |= ((uint32_t)IDX_IMMED32 << 22); // index0: always immediate for our use
    if (indexDim == IDX_2D)
        tok |= ((uint32_t)IDX_IMMED32 << 25); // index1: also immediate
    if (extended)
        tok |= (1u << 31);
    return tok;
}

// Build an extended-operand negate modifier token (follows OperandToken0 when bit31=1)
inline uint32_t MakeNegateModToken()
{
    // ExtendedOperandToken:
    // [5:0]  = D3D10_SB_EXTENDED_OPERAND_MODIFIER (0x1 = modifier)
    // [13:6] = D3D10_SB_EXTENDED_OPERAND_MODIFIER_TYPE (1=negate,2=abs,3=absneg)
    // Bit pattern from spec: type=1 (EXTENDED_OPERAND_MODIFIER_TYPE), value=negate(1)
    // ExtendedOperandToken0: bits [5:3]=MOD_NEGATE(1), bits [2:0]=modifier-type(1)
    // Actually: bits [31]=0 (not further extended), bits [5:0]=0x06 (type=modifier, negate)
    // From d3d10tokenizedprogramformat.hpp:
    //   D3D10_SB_EXTENDED_OPERAND_MODIFIER = 1
    //   D3D10_SB_EXTENDED_OPERAND_MODIFIER_NEGATE_BIT = bit 6
    //   D3D10_SB_EXTENDED_OPERAND_MODIFIER_ABS_BIT    = bit 7
    // So a negate modifier token = 1 | (1<<6) = 0x41
    return 0x00000041u;
}

// ---------------------------------------------------------------------------
// OpcodeToken0 builder
// ---------------------------------------------------------------------------
inline uint32_t MakeOpcodeToken0(D3D10_SB_OPCODE op, uint32_t lenDWORDs,
                                  uint32_t extraBits = 0)
{
    // [10:0]  = opcode
    // [23:11] = opcode-specific (e.g. constant-buffer access type, saturate, etc.)
    // [27:24] = instruction length in DWORDs
    // [31]    = extended opcode token present (not used here)
    return ((uint32_t)op & 0x7FFu)
         | (extraBits & 0x00FFF800u)
         | ((lenDWORDs & 0xFu) << 24);
}

// DCL_CONSTANT_BUFFER access pattern (bit 11 of OpcodeToken0)
constexpr uint32_t DCL_CB_IMMEDIATE_INDEXED = 0;       // bit11=0
constexpr uint32_t DCL_CB_DYNAMIC_INDEXED   = (1<<11); // bit11=1

// ---------------------------------------------------------------------------
// High-level DXBC bytecode representation
// ---------------------------------------------------------------------------

struct DXBCBlob {
    std::vector<uint8_t> data;

    bool IsValid() const;

    // Returns pointer to a chunk's payload, nullptr if not found.
    const uint8_t* FindChunk(uint32_t fourCC, uint32_t* outSize = nullptr) const;

    // Locate the SHDR or SHEX chunk (the actual bytecode).
    const uint8_t* ShaderBytecodeChunk(uint32_t* outSize = nullptr) const;

    // True if this DXBC wraps DXIL (SM6+, contains DXIL chunk, not SHDR/SHEX).
    bool IsDXIL() const;

    // Shader type from the bytecode token (vertex=1, pixel=0, compute=5, etc.)
    // Returns 0xFF on failure.
    uint8_t ShaderType() const;

    // Total number of declared temp registers in the SHDR/SHEX chunk.
    uint32_t TempRegCount() const;

    // Number of declared constant buffers.
    uint32_t CBCount() const;

    // True if constant buffer slot <slot> is already declared.
    bool HasCBSlot(uint32_t slot) const;

    // Byte offset of the RET instruction (last instruction) within the
    // SHDR/SHEX payload, or npos if not found.
    static constexpr size_t npos = SIZE_MAX;
    size_t FindRetOffset() const;

    // Rebuild the DXBC container with a replaced SHDR/SHEX payload.
    // newShaderPayload is the new bytecode for that chunk (without chunk header).
    DXBCBlob WithReplacedShader(const std::vector<uint32_t>& newShaderPayload) const;

    // Recompute the MD5 checksum in the header.
    void RecomputeChecksum();
};

// ---------------------------------------------------------------------------
// Main DXBC stereo-injection routines
// ---------------------------------------------------------------------------
namespace DXBCPatcher {

// Validate that blob is a patchable DXBC (not DXIL, correct magic, etc.)
bool CanPatch(const DXBCBlob& blob, std::string* reason = nullptr);

// Inject stereo constant buffer declaration (dcl_constantbuffer cbN[1])
// into the shader bytecode after the last existing DCL instruction.
// Returns false if cbSlot is already declared.
bool InjectCBDecl(std::vector<uint32_t>& shaderTokens, uint32_t cbSlot,
                  uint32_t& inOutTempCount);

// Inject VS stereo position correction before the RET instruction:
//   add  rN.x, o_pos.w,  -cbS[0].y    // rN.x = pos.w  - convergence
//   mul  rN.x, rN.x,      cbS[0].x    // rN.x *= separation
//   mad  o_pos.x, rN.x,   cbS[0].z, o_pos.x
// Returns false if SV_Position output index cannot be determined.
bool InjectVSPositionCorrection(std::vector<uint32_t>& tokens,
                                uint32_t posOutputIndex,
                                uint32_t cbSlot,
                                uint32_t scratchReg);

// Inject PS/CS stereo eye-index constant (used by deferred lighting passes):
//   Writes eye index to a temp register from cbS[0].z.
// Returns the temp register index written, or UINT32_MAX on failure.
uint32_t InjectEyeIndexLoad(std::vector<uint32_t>& tokens,
                            uint32_t cbSlot,
                            uint32_t scratchReg);

// Patch a full DXBC blob with all applicable stereo corrections.
// Returns an empty blob on failure (IsDXIL, unsupported type, etc.)
DXBCBlob PatchBlob(const DXBCBlob& src, uint32_t cbSlot);

// Utility: read shader tokens (DWORD array) from a SHDR/SHEX payload.
std::vector<uint32_t> TokensFromPayload(const uint8_t* payload, uint32_t sizeBytes);

// Utility: serialise tokens back to bytes.
std::vector<uint8_t> PayloadFromTokens(const std::vector<uint32_t>& tokens);

} // namespace DXBCPatcher

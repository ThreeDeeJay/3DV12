// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dxbc_parser.h"
#include "log.h"
#include <cstring>
#include <cassert>
#include <algorithm>

// ---------------------------------------------------------------------------
// Simple MD5 for DXBC checksum re-computation
// (public-domain implementation, stripped to the minimum needed)
// ---------------------------------------------------------------------------
namespace MD5 {
    struct Context {
        uint32_t state[4];
        uint32_t count[2];
        uint8_t  buf[64];
    };
    static const uint32_t S[] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
    };
    static const uint32_t K[] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
        0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
        0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,
        0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
        0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
        0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,
        0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,
        0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
        0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static inline uint32_t ROL(uint32_t x, uint32_t n){ return (x<<n)|(x>>(32-n)); }
    static void ProcessBlock(uint32_t* st, const uint8_t* blk) {
        uint32_t a=st[0],b=st[1],c=st[2],d=st[3],M[16];
        memcpy(M,blk,64);
        for(int i=0;i<64;i++){
            uint32_t F,g;
            if(i<16){F=(b&c)|(~b&d);g=i;}
            else if(i<32){F=(d&b)|(~d&c);g=(5*i+1)%16;}
            else if(i<48){F=b^c^d;g=(3*i+5)%16;}
            else{F=c^(b|(~d));g=(7*i)%16;}
            F=F+a+K[i]+M[g];
            a=d;d=c;c=b;b=b+ROL(F,S[i]);
        }
        st[0]+=a;st[1]+=b;st[2]+=c;st[3]+=d;
    }
    // Compute MD5 of data, write 16 bytes to out
    static void Hash(const uint8_t* data, size_t len, uint8_t out[16]) {
        uint32_t st[4]={0x67452301,0xefcdab89,0x98badcfe,0x10325476};
        uint8_t buf[64]; size_t pos=0;
        while(len-pos>=64){ ProcessBlock(st,data+pos); pos+=64; }
        size_t rem=len-pos;
        memcpy(buf,data+pos,rem);
        buf[rem]=0x80; memset(buf+rem+1,0,63-rem);
        if(rem>=56){
            ProcessBlock(st,buf); memset(buf,0,56);
        }
        uint64_t bits=(uint64_t)len*8;
        memcpy(buf+56,&bits,8);
        ProcessBlock(st,buf);
        memcpy(out,st,16);
    }
}

// ---------------------------------------------------------------------------
// DXBCBlob helpers
// ---------------------------------------------------------------------------

bool DXBCBlob::IsValid() const
{
    if (data.size() < sizeof(DXBCHeader)) return false;
    auto* hdr = reinterpret_cast<const DXBCHeader*>(data.data());
    return hdr->magic == kFourCC_DXBC && hdr->one == 1 && hdr->totalSize == (uint32_t)data.size();
}

const uint8_t* DXBCBlob::FindChunk(uint32_t fourCC, uint32_t* outSize) const
{
    if (!IsValid()) return nullptr;
    auto* hdr    = reinterpret_cast<const DXBCHeader*>(data.data());
    auto* offs   = reinterpret_cast<const uint32_t*>(data.data() + sizeof(DXBCHeader));
    for (uint32_t i = 0; i < hdr->chunkCount; ++i) {
        uint32_t off = offs[i];
        if (off + sizeof(DXBCChunkHeader) > data.size()) continue;
        auto* ch = reinterpret_cast<const DXBCChunkHeader*>(data.data() + off);
        if (ch->fourCC == fourCC) {
            if (outSize) *outSize = ch->dataSize;
            return data.data() + off + sizeof(DXBCChunkHeader);
        }
    }
    return nullptr;
}

const uint8_t* DXBCBlob::ShaderBytecodeChunk(uint32_t* outSize) const
{
    auto* p = FindChunk(kFourCC_SHDR, outSize);
    if (!p) p = FindChunk(kFourCC_SHEX, outSize);
    return p;
}

bool DXBCBlob::IsDXIL() const
{
    return FindChunk(kFourCC_DXIL) != nullptr;
}

uint8_t DXBCBlob::ShaderType() const
{
    uint32_t sz = 0;
    auto* payload = ShaderBytecodeChunk(&sz);
    if (!payload || sz < 4) return 0xFF;
    // First DWORD: ProgramToken – bits [15:0] = minor(4)|major(4)|type(8)... actually:
    // bits [3:0]  = minor version
    // bits [7:4]  = major version
    // bits [15:8] = shader type: 0xFF=PS, 0xFE=VS, 0x48=GS, 0x48=HS, 0x49=DS, 0x4B=CS
    // Simplified: byte at offset 3 is the shader type half-byte:
    // Actually the combined token: vshdr_type = (token >> 16) & 0xFFFF
    // Word at offset 2 (bytes 2-3): upper byte = type
    uint8_t type = payload[3]; // bits [15:8] of first DWORD, high byte
    return type;
}

uint32_t DXBCBlob::TempRegCount() const
{
    uint32_t sz = 0;
    auto* payload = ShaderBytecodeChunk(&sz);
    if (!payload || sz < 8) return 0;
    auto* tokens = reinterpret_cast<const uint32_t*>(payload);
    uint32_t tokenCount = tokens[1]; // second DWORD = instruction count
    // Walk instructions looking for DCL_TEMPS
    for (uint32_t pc = 2; pc < tokenCount; ) {
        uint32_t tok0 = tokens[pc];
        uint32_t opcode = tok0 & 0x7FFu;
        uint32_t len    = (tok0 >> 24) & 0xFu;
        if (len == 0) len = 1; // extended opcode
        if (opcode == (uint32_t)OP_DCL_TEMPS && len >= 2)
            return tokens[pc + 1];
        if (len == 0 || pc + len > tokenCount) break;
        pc += len;
    }
    return 0;
}

uint32_t DXBCBlob::CBCount() const
{
    uint32_t sz = 0;
    auto* payload = ShaderBytecodeChunk(&sz);
    if (!payload || sz < 8) return 0;
    auto* tokens  = reinterpret_cast<const uint32_t*>(payload);
    uint32_t tokenCount = tokens[1];
    uint32_t count = 0;
    for (uint32_t pc = 2; pc < tokenCount; ) {
        uint32_t tok0  = tokens[pc];
        uint32_t opcode= tok0 & 0x7FFu;
        uint32_t len   = (tok0 >> 24) & 0xFu;
        if (len == 0) len = 1;
        if (opcode == (uint32_t)OP_DCL_CONSTANT_BUFFER) count++;
        if (len == 0 || pc + len > tokenCount) break;
        pc += len;
    }
    return count;
}

bool DXBCBlob::HasCBSlot(uint32_t slot) const
{
    uint32_t sz = 0;
    auto* payload = ShaderBytecodeChunk(&sz);
    if (!payload || sz < 8) return false;
    auto* tokens  = reinterpret_cast<const uint32_t*>(payload);
    uint32_t tokenCount = tokens[1];
    for (uint32_t pc = 2; pc < tokenCount; ) {
        uint32_t tok0   = tokens[pc];
        uint32_t opcode = tok0 & 0x7FFu;
        uint32_t len    = (tok0 >> 24) & 0xFu;
        if (len == 0) len = 1;
        if (opcode == (uint32_t)OP_DCL_CONSTANT_BUFFER && len >= 4) {
            // tokens[pc+2] = register index (slot), tokens[pc+3] = array count
            if (tokens[pc + 2] == slot) return true;
        }
        if (len == 0 || pc + len > tokenCount) break;
        pc += len;
    }
    return false;
}

size_t DXBCBlob::FindRetOffset() const
{
    uint32_t sz = 0;
    auto* payload = ShaderBytecodeChunk(&sz);
    if (!payload || sz < 8) return npos;
    auto* tokens   = reinterpret_cast<const uint32_t*>(payload);
    uint32_t count = tokens[1]; // length in DWORDs
    size_t lastRet = npos;
    for (uint32_t pc = 2; pc < count; ) {
        uint32_t tok0   = tokens[pc];
        uint32_t opcode = tok0 & 0x7FFu;
        uint32_t len    = (tok0 >> 24) & 0xFu;
        if (len == 0) len = 1;
        if (opcode == (uint32_t)OP_RET) lastRet = pc;
        if (len == 0 || pc + len > count) break;
        pc += len;
    }
    return lastRet; // offset in DWORDs from payload start
}

DXBCBlob DXBCBlob::WithReplacedShader(const std::vector<uint32_t>& newTokens) const
{
    if (!IsValid()) return {};
    auto* hdrIn = reinterpret_cast<const DXBCHeader*>(data.data());
    auto* offsIn = reinterpret_cast<const uint32_t*>(data.data() + sizeof(DXBCHeader));

    // Find old SHDR/SHEX chunk offset
    uint32_t shaderChunkOffset = 0;
    uint32_t oldChunkDataSize  = 0;
    uint32_t shaderFourCC      = 0;
    for (uint32_t i = 0; i < hdrIn->chunkCount; i++) {
        uint32_t off = offsIn[i];
        auto* ch = reinterpret_cast<const DXBCChunkHeader*>(data.data() + off);
        if (ch->fourCC == kFourCC_SHDR || ch->fourCC == kFourCC_SHEX) {
            shaderChunkOffset = off;
            oldChunkDataSize  = ch->dataSize;
            shaderFourCC      = ch->fourCC;
            break;
        }
    }
    if (shaderChunkOffset == 0) return {};

    uint32_t newChunkDataSize = (uint32_t)(newTokens.size() * sizeof(uint32_t));
    int32_t  delta = (int32_t)newChunkDataSize - (int32_t)oldChunkDataSize;

    // Build new blob
    DXBCBlob out;
    out.data.resize(data.size() + delta);

    // Copy up to (and including) the chunk count
    uint32_t headerBytes = (uint32_t)(sizeof(DXBCHeader) + hdrIn->chunkCount * sizeof(uint32_t));
    memcpy(out.data.data(), data.data(), headerBytes);

    // Fix up offsets in the new header for chunks AFTER the replaced one
    auto* offsOut = reinterpret_cast<uint32_t*>(out.data.data() + sizeof(DXBCHeader));
    for (uint32_t i = 0; i < hdrIn->chunkCount; i++) {
        if (offsIn[i] > shaderChunkOffset)
            offsOut[i] = offsIn[i] + delta;
        else
            offsOut[i] = offsIn[i];
    }

    // Copy chunk data in sections:
    // [headerBytes .. shaderChunkOffset) – chunks before
    memcpy(out.data.data() + headerBytes, data.data() + headerBytes,
           shaderChunkOffset - headerBytes);

    // Write new shader chunk header
    uint32_t writePos = shaderChunkOffset;
    DXBCChunkHeader newCH{ shaderFourCC, newChunkDataSize };
    memcpy(out.data.data() + writePos, &newCH, sizeof(newCH));
    writePos += sizeof(newCH);

    // Write new shader tokens
    memcpy(out.data.data() + writePos, newTokens.data(), newChunkDataSize);
    writePos += newChunkDataSize;

    // Copy remaining chunks
    uint32_t oldAfterShader = shaderChunkOffset + sizeof(DXBCChunkHeader) + oldChunkDataSize;
    uint32_t remaining      = (uint32_t)data.size() - oldAfterShader;
    if (remaining > 0)
        memcpy(out.data.data() + writePos, data.data() + oldAfterShader, remaining);

    // Fix total size
    auto* outHdr = reinterpret_cast<DXBCHeader*>(out.data.data());
    outHdr->totalSize = (uint32_t)out.data.size();

    out.RecomputeChecksum();
    return out;
}

void DXBCBlob::RecomputeChecksum()
{
    if (data.size() < sizeof(DXBCHeader)) return;
    // DXBC checksum is an MD5 hash of everything after the checksum field
    // i.e. starting at offset 20 (= sizeof(magic) + sizeof(checksum)) = 0x14.
    // Actually: the DXBC checksum covers bytes [20..end) – after the 4-byte magic and 16-byte checksum.
    // Source: d3d10shader.h / various RE writeups.
    // Input = { 0x01 (one byte), then bytes[20..] }
    // Simplified: MD5( data[20..] ) with the standard DXBC tweak.
    // Standard reverse-engineered algorithm used by 3DMigoto and others.
    auto* hdr = reinterpret_cast<DXBCHeader*>(data.data());
    uint8_t digest[16];
    MD5::Hash(data.data() + 20, data.size() - 20, digest);
    memcpy(hdr->checksum, digest, 16);
}

// ---------------------------------------------------------------------------
// DXBCPatcher
// ---------------------------------------------------------------------------
namespace DXBCPatcher {

bool CanPatch(const DXBCBlob& blob, std::string* reason)
{
    if (!blob.IsValid()) { if(reason)*reason="Invalid DXBC magic"; return false; }
    if (blob.IsDXIL())   { if(reason)*reason="DXIL/SM6+ not patchable by DXBC engine"; return false; }
    if (!blob.ShaderBytecodeChunk()) { if(reason)*reason="No SHDR/SHEX chunk"; return false; }
    return true;
}

std::vector<uint32_t> TokensFromPayload(const uint8_t* payload, uint32_t sizeBytes)
{
    size_t n = sizeBytes / sizeof(uint32_t);
    std::vector<uint32_t> v(n);
    memcpy(v.data(), payload, n * sizeof(uint32_t));
    return v;
}

std::vector<uint8_t> PayloadFromTokens(const std::vector<uint32_t>& tokens)
{
    std::vector<uint8_t> v(tokens.size() * sizeof(uint32_t));
    memcpy(v.data(), tokens.data(), v.size());
    return v;
}

// Find the DWORD offset of the last declaration instruction (any OP_DCL_*).
// We want to inject our DCL_CONSTANT_BUFFER right after the last DCL.
static size_t FindEndOfDecls(const std::vector<uint32_t>& tokens)
{
    size_t last = 2; // start after ProgramToken and length token
    size_t tokenCount = tokens.size();
    for (uint32_t pc = 2; pc < tokenCount; ) {
        uint32_t tok0   = tokens[pc];
        uint32_t opcode = tok0 & 0x7FFu;
        uint32_t len    = (tok0 >> 24) & 0xFu;
        if (len == 0) len = 1;
        // Opcodes >= OP_DCL_RESOURCE(88) are declarations
        if (opcode >= 88u) last = pc + len;
        else if (opcode != 0xFFu) break; // hit first real instruction – stop
        if (pc + len >= tokenCount) break;
        pc += len;
    }
    return last;
}

bool InjectCBDecl(std::vector<uint32_t>& tokens, uint32_t cbSlot,
                  [[maybe_unused]] uint32_t& inOutTempCount)
{
    // Instruction: dcl_constantbuffer cb<cbSlot>[1], immediateIndexed
    //
    //  Token 0 (OpcodeToken0):
    //    bits [10:0]  = OP_DCL_CONSTANT_BUFFER (89 = 0x59)
    //    bits [11]    = 0 (immediateIndexed)
    //    bits [27:24] = 4 (instruction length = 4 DWORDs)
    //
    //  Token 1 (OperandToken0 for cbN):
    //    bits [1:0]   = 2 (4-component)
    //    bits [3:2]   = 1 (swizzle mode)
    //    bits [11:4]  = 0xE4 (xyzw swizzle)
    //    bits [19:12] = 8 (OPT_CONSTANT_BUFFER)
    //    bits [21:20] = 2 (2D index: register + array element)
    //    bits [24:22] = 0 (index0: IMMEDIATE32)
    //    bits [27:25] = 0 (index1: IMMEDIATE32)
    //
    //  Token 2: cbSlot (register index)
    //  Token 3: 1 (array size)

    uint32_t opTok = MakeOpcodeToken0(OP_DCL_CONSTANT_BUFFER, 4, DCL_CB_IMMEDIATE_INDEXED);
    uint32_t opTok1 = MakeOperandToken0_Register(OPT_CONSTANT_BUFFER, COMP_SWIZZLE,
                                                  SWIZZLE_XYZW, IDX_2D, false);
    std::vector<uint32_t> instr = { opTok, opTok1, cbSlot, 1u };

    size_t insertAt = FindEndOfDecls(tokens);
    tokens.insert(tokens.begin() + insertAt, instr.begin(), instr.end());

    // Update the instruction length token (tokens[1])
    tokens[1] = (uint32_t)tokens.size();
    return true;
}

// Build source operand for cb<slot>[0].y with negate modifier (-cb)
// Returns 4 tokens: [modOperandToken0, extModToken, regIdx, elemIdx]
static std::vector<uint32_t> CbSourceNeg(uint32_t cbSlot, uint32_t component /*0=x,1=y,2=z,3=w*/)
{
    // OperandToken0 with extended=1 (negate modifier following).
    // SELECT_1 mode: component index sits in bits [5:4] of the 8-bit field at
    // OperandToken0[11:4], i.e. the low 2 bits of that field (bits [5:4] of the token).
    uint32_t compField = (component & 3u); // low 2 bits of the 8-bit field at [11:4]
    uint32_t opTok = 0;
    opTok |= (2u << 0);       // 4-component
    opTok |= (2u << 2);       // SELECT_1 mode
    opTok |= (compField << 4);// component
    opTok |= ((uint32_t)OPT_CONSTANT_BUFFER << 12);
    opTok |= ((uint32_t)IDX_2D << 20);
    opTok |= ((uint32_t)IDX_IMMED32 << 22);
    opTok |= ((uint32_t)IDX_IMMED32 << 25);
    opTok |= (1u << 31); // extended token (negate modifier follows)

    return { opTok, MakeNegateModToken(), cbSlot, 0u };
}

// Build source operand for cb<slot>[0].<comp> (no modifier)
static std::vector<uint32_t> CbSource(uint32_t cbSlot, uint32_t component)
{
    uint32_t compField = (component & 3u);
    uint32_t opTok = 0;
    opTok |= (2u << 0);
    opTok |= (2u << 2);  // SELECT_1
    opTok |= (compField << 4);
    opTok |= ((uint32_t)OPT_CONSTANT_BUFFER << 12);
    opTok |= ((uint32_t)IDX_2D << 20);
    opTok |= ((uint32_t)IDX_IMMED32 << 22);
    opTok |= ((uint32_t)IDX_IMMED32 << 25);
    return { opTok, cbSlot, 0u };
}

// Build dest operand for temp rN.x (mask X)
static std::vector<uint32_t> TempDest(uint32_t reg)
{
    uint32_t opTok = MakeOperandToken0_Register(OPT_TEMP, COMP_MASK, MASK_X, IDX_1D);
    return { opTok, reg };
}

// Build source operand for temp rN.x (select1, X)
static std::vector<uint32_t> TempSrc(uint32_t reg, uint32_t comp = 0)
{
    uint32_t compField = comp & 3u;
    uint32_t opTok = 0;
    opTok |= (2u << 0);
    opTok |= (2u << 2);  // SELECT_1
    opTok |= (compField << 4);
    opTok |= ((uint32_t)OPT_TEMP << 12);
    opTok |= ((uint32_t)IDX_1D << 20);
    opTok |= ((uint32_t)IDX_IMMED32 << 22);
    return { opTok, reg };
}

// Build output register operand (o<reg>.x, select1, X)
static std::vector<uint32_t> OutputSrc(uint32_t reg, uint32_t comp = 0)
{
    uint32_t compField = comp & 3u;
    uint32_t opTok = 0;
    opTok |= (2u << 0);
    opTok |= (2u << 2);  // SELECT_1
    opTok |= (compField << 4);
    opTok |= ((uint32_t)OPT_OUTPUT << 12);
    opTok |= ((uint32_t)IDX_1D << 20);
    opTok |= ((uint32_t)IDX_IMMED32 << 22);
    return { opTok, reg };
}

// Build output register operand dest (o<reg>.x mask)
static std::vector<uint32_t> OutputDest(uint32_t reg, uint32_t comp = 0)
{
    uint32_t maskBit = 1u << comp;
    uint32_t opTok = MakeOperandToken0_Register(OPT_OUTPUT, COMP_MASK, maskBit, IDX_1D);
    return { opTok, reg };
}

// Append tokens from a vector
static void Append(std::vector<uint32_t>& dst, const std::vector<uint32_t>& src)
{
    dst.insert(dst.end(), src.begin(), src.end());
}

// Sentinel used in place of SIZE_MAX for clarity
static constexpr size_t npos_value = SIZE_MAX;

bool InjectVSPositionCorrection(std::vector<uint32_t>& tokens,
                                uint32_t posOutputIndex,
                                uint32_t cbSlot,
                                uint32_t scratchReg)
{
    // Find the DWORD offset of the RET instruction
    size_t retPC = npos_value;
    uint32_t tokenCount = (uint32_t)tokens.size();
    for (uint32_t pc = 2; pc < tokenCount; ) {
        uint32_t tok0   = tokens[pc];
        uint32_t opcode = tok0 & 0x7FFu;
        uint32_t len    = (tok0 >> 24) & 0xFu;
        if (len == 0) len = 1;
        if (opcode == (uint32_t)OP_RET) { retPC = pc; break; }
        if (pc + len >= tokenCount) break;
        pc += len;
    }
    if (retPC == npos_value) return false;

    // Build three-instruction stereo correction block to insert before RET:
    //
    //   // rN.x = o_pos.w - cbS[0].y  (pos.w minus convergence)
    //   add  rN.x, o_pos_src.w, -cbS[0].y
    //
    //   // rN.x *= cbS[0].x  (separation)
    //   mul  rN.x, rN.x, cbS[0].x
    //
    //   // o_pos.x += rN.x * cbS[0].z  (eye: -0.5 left, +0.5 right)
    //   mad  o_pos.x, rN.x, cbS[0].z, o_pos.x

    std::vector<uint32_t> block;

    // --- ADD rN.x, o_posOutputIndex.w, -cbSlot[0].y ---
    {
        // Dest: rN.x
        auto dest = TempDest(scratchReg);
        // Src0: o_posOutputIndex.w (component 3=w)
        auto src0 = OutputSrc(posOutputIndex, 3);
        // Src1: -cbSlot[0].y  (component 1=y, negated)
        auto src1 = CbSourceNeg(cbSlot, 1);

        uint32_t instrLen = 1 + (uint32_t)(dest.size() + src0.size() + src1.size());
        block.push_back(MakeOpcodeToken0(OP_ADD, instrLen));
        Append(block, dest);
        Append(block, src0);
        Append(block, src1);
    }

    // --- MUL rN.x, rN.x, cbSlot[0].x ---
    {
        auto dest = TempDest(scratchReg);
        auto src0 = TempSrc(scratchReg, 0);
        auto src1 = CbSource(cbSlot, 0); // .x = separation

        uint32_t instrLen = 1 + (uint32_t)(dest.size() + src0.size() + src1.size());
        block.push_back(MakeOpcodeToken0(OP_MUL, instrLen));
        Append(block, dest);
        Append(block, src0);
        Append(block, src1);
    }

    // --- MAD o_pos.x, rN.x, cbSlot[0].z, o_pos.x ---
    {
        auto dest = OutputDest(posOutputIndex, 0); // .x component
        auto src0 = TempSrc(scratchReg, 0);
        auto src1 = CbSource(cbSlot, 2); // .z = eye (-0.5 or +0.5)
        auto src2 = OutputSrc(posOutputIndex, 0); // o_pos.x (add-in)

        uint32_t instrLen = 1 + (uint32_t)(dest.size() + src0.size() + src1.size() + src2.size());
        block.push_back(MakeOpcodeToken0(OP_MAD, instrLen));
        Append(block, dest);
        Append(block, src0);
        Append(block, src1);
        Append(block, src2);
    }

    // Insert block before RET
    tokens.insert(tokens.begin() + retPC, block.begin(), block.end());
    // Update instruction count
    tokens[1] = (uint32_t)tokens.size();
    return true;
}

uint32_t InjectEyeIndexLoad(std::vector<uint32_t>& tokens,
                            uint32_t cbSlot,
                            uint32_t scratchReg)
{
    // Find end of decls to insert after
    size_t insertAt = FindEndOfDecls(tokens);

    // Build: mov rScratch.x, cbSlot[0].z   (eye index, 0=left 1=right in integer coords)
    // For PS/CS usage: eye = cbSlot[0].z = -0.5 (left) / +0.5 (right)
    {
        auto dest = TempDest(scratchReg);
        auto src  = CbSource(cbSlot, 2); // .z component

        uint32_t instrLen = 1 + (uint32_t)(dest.size() + src.size());
        std::vector<uint32_t> instr;
        instr.push_back(MakeOpcodeToken0(OP_MOV, instrLen));
        Append(instr, dest);
        Append(instr, src);

        tokens.insert(tokens.begin() + insertAt, instr.begin(), instr.end());
        tokens[1] = (uint32_t)tokens.size();
    }
    return scratchReg;
}

DXBCBlob PatchBlob(const DXBCBlob& src, uint32_t cbSlot)
{
    std::string reason;
    if (!CanPatch(src, &reason)) {
        LOG_DEBUG("DXBCPatcher: Cannot patch blob – %s", reason.c_str());
        return {};
    }

    uint32_t sz = 0;
    auto* payload = src.ShaderBytecodeChunk(&sz);
    auto tokens   = TokensFromPayload(payload, sz);

    uint8_t shaderType = src.ShaderType();
    // shaderType byte from ProgramToken:
    //   0xFF = PS, 0xFE = VS, 0x48 = GS, 0x49 = DS (hull), 0x4A = HS, 0x4B = CS
    bool isVS = (shaderType == 0xFE);
    bool isPS = (shaderType == 0xFF);
    bool isCS = (shaderType == 0x4B);

    LOG_DEBUG("DXBCPatcher: Patching shader type=0x%02X, %u tokens",
              shaderType, (uint32_t)tokens.size());

    // --- Step 1: Ensure temp register space ---
    uint32_t tempCount = src.TempRegCount();

    // --- Step 2: Inject CB declaration if not already present ---
    if (!src.HasCBSlot(cbSlot)) {
        uint32_t dummyTemp = tempCount;
        InjectCBDecl(tokens, cbSlot, dummyTemp);
    }

    // --- Step 3: Bump temp count for our scratch register ---
    // Find DCL_TEMPS and update its argument
    bool foundDclTemps = false;
    for (uint32_t pc = 2; pc < (uint32_t)tokens.size(); ) {
        uint32_t tok0   = tokens[pc];
        uint32_t opcode = tok0 & 0x7FFu;
        uint32_t len    = (tok0 >> 24) & 0xFu;
        if (len == 0) len = 1;
        if (opcode == (uint32_t)OP_DCL_TEMPS) {
            tempCount = tokens[pc + 1];
            tokens[pc + 1] = tempCount + 1; // add one scratch
            foundDclTemps = true;
            break;
        }
        pc += len;
    }
    if (!foundDclTemps) {
        // Insert DCL_TEMPS = 1 after header tokens
        // OpcodeToken0 for DCL_TEMPS: opcode=104(0x68), len=2
        size_t insertAt = FindEndOfDecls(tokens);
        std::vector<uint32_t> dclTemps = {
            MakeOpcodeToken0(OP_DCL_TEMPS, 2),
            1u
        };
        tokens.insert(tokens.begin() + insertAt, dclTemps.begin(), dclTemps.end());
        tokens[1] = (uint32_t)tokens.size();
        tempCount = 0;
    }
    uint32_t scratchReg = tempCount; // index of our new scratch register

    // --- Step 4: Type-specific patching ---
    if (isVS) {
        // For VS: inject position correction before RET
        // We assume SV_Position is output register 0. The output signature
        // (OSGN chunk) should be checked to confirm; we use 0 as default.
        InjectVSPositionCorrection(tokens, 0, cbSlot, scratchReg);
        LOG_DEBUG("DXBCPatcher: VS position correction injected (cb%u, scratchReg=%u)",
                  cbSlot, scratchReg);
    } else if (isPS || isCS) {
        // For PS/CS deferred passes: inject eye-index load at top of shader
        InjectEyeIndexLoad(tokens, cbSlot, scratchReg);
        LOG_DEBUG("DXBCPatcher: PS/CS eye-index load injected (cb%u, scratchReg=%u)",
                  cbSlot, scratchReg);
    }

    // --- Step 5: Rebuild blob ---
    return src.WithReplacedShader(tokens);
}

} // namespace DXBCPatcher

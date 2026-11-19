/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal SPIR-V module builder.
 *
 * Instructions go into sections that are concatenated in the order the
 * specification requires. Types and constants are interned: asking twice
 * for "uint" or "uint 4" returns the same id. */
#ifndef PS5C_SPV_H
#define PS5C_SPV_H

#include "base/base.h"

typedef enum spv_section {
    SPV_SEC_CAPABILITY,
    SPV_SEC_EXTENSION,
    SPV_SEC_IMPORT,
    SPV_SEC_MEMORY_MODEL,
    SPV_SEC_ENTRY,
    SPV_SEC_EXEC_MODE,
    SPV_SEC_DEBUG,
    SPV_SEC_ANNOTATION,
    SPV_SEC_GLOBAL, /* types, constants, global variables */
    SPV_SEC_FUNCTION,
    SPV_SEC_COUNT
} spv_section;

typedef struct spv_words {
    uint32_t *data;
    size_t len, cap;
} spv_words;

typedef struct spv_intern {
    uint32_t key[8]; /* opcode + operands identifying the type/constant */
    uint32_t len;
    uint32_t id;
} spv_intern;

typedef struct spv_builder {
    spv_words sec[SPV_SEC_COUNT];
    uint32_t next_id;
    spv_intern *interned;
    size_t intern_len, intern_cap;
    uint32_t *intern_slots; /* entry index + 1; zero is empty, no pointer survives realloc */
    size_t intern_slot_cap; /* power of two, at most half full */
    uint32_t glsl_ext; /* GLSL.std.450 import */
    uint32_t function_vars_at; /* position in SPV_SEC_FUNCTION where OpVariables of the current function go */
    spv_words fn_vars;         /* OpVariables of the current function (must lead its first block) */
    spv_words fn_prologue;     /* initialization after every local, including locals discovered later */
    bool failed;               /* out of memory */
    bool exact;                /* float add/sub/mul/div/dot get NoContraction (no host fma fusion) */
} spv_builder;

void spv_init(spv_builder *b);
void spv_free(spv_builder *b);
uint32_t spv_id(spv_builder *b);
/* Emits opcode with `n` operand words into a section. */
void spv_emit(spv_builder *b, spv_section s, uint32_t op, const uint32_t *operands, uint32_t n);
/* Literal string operand words (null-terminated, padded); returns word count. */
uint32_t spv_string(const char *str, uint32_t *out, uint32_t max_words);
/* Final module (header + sections). Caller frees *out. */
bool spv_finish(spv_builder *b, uint32_t **out, size_t *words);

/* Types (interned) */
uint32_t spv_type_void(spv_builder *b);
uint32_t spv_type_bool(spv_builder *b);
uint32_t spv_type_int(spv_builder *b, uint32_t width, bool is_signed);
uint32_t spv_type_float(spv_builder *b, uint32_t width);
uint32_t spv_type_vector(spv_builder *b, uint32_t component, uint32_t count);
uint32_t spv_type_array(spv_builder *b, uint32_t element, uint32_t length_id);
uint32_t spv_type_runtime_array(spv_builder *b, uint32_t element); /* not interned: needs its own decorations */
uint32_t spv_type_pointer(spv_builder *b, uint32_t storage_class, uint32_t pointee);
uint32_t spv_type_function(spv_builder *b, uint32_t ret, const uint32_t *params, uint32_t n);
/* OpTypeImage operands after the result id: sampled type, dim, depth, arrayed, ms, sampled, format */
uint32_t spv_type_image(spv_builder *b, const uint32_t operands[7]);
uint32_t spv_type_sampled_image(spv_builder *b, uint32_t image);

/* Constants (interned) */
uint32_t spv_const_u32(spv_builder *b, uint32_t v);
uint32_t spv_const_i32(spv_builder *b, int32_t v);
uint32_t spv_const_f32(spv_builder *b, float v);
uint32_t spv_const_bool(spv_builder *b, bool v);
/* Recover a literal uint32 without adding types/constants or changing IDs. */
bool spv_const_u32_value(const spv_builder *b, uint32_t id, uint32_t *value);
uint32_t spv_const_composite(spv_builder *b, uint32_t type, const uint32_t *parts, uint32_t n);

/* Function-body helpers */
uint32_t spv_op(spv_builder *b, uint32_t op, uint32_t type, const uint32_t *args, uint32_t n); /* returns result id */
void spv_op_void(spv_builder *b, uint32_t op, const uint32_t *args, uint32_t n);
uint32_t spv_ext(spv_builder *b, uint32_t type, uint32_t glsl_op, const uint32_t *args, uint32_t n);
void spv_label(spv_builder *b, uint32_t id);
/* Function-scope variable, hoisted to the start of the current function. */
uint32_t spv_local_var(spv_builder *b, uint32_t pointer_type);
/* Code placed right after all function variables, even locals discovered
 * after emitting part of this prologue. Initialization order is preserved. */
uint32_t spv_prologue_op(spv_builder *b, uint32_t op, uint32_t type, const uint32_t *args, uint32_t n);
void spv_prologue_void(spv_builder *b, uint32_t op, const uint32_t *args, uint32_t n);
void spv_begin_function(spv_builder *b, uint32_t ret, uint32_t fn_type, uint32_t id);
void spv_end_function(spv_builder *b);

/* Decorations */
void spv_decorate(spv_builder *b, uint32_t target, uint32_t decoration, const uint32_t *args, uint32_t n);
void spv_member_decorate(spv_builder *b, uint32_t type, uint32_t member, uint32_t decoration, const uint32_t *args,
                         uint32_t n);

/* SPIR-V constants used by the translator */
enum {
    SpvOpExtInstImport = 11, SpvOpExtInst = 12, SpvOpMemoryModel = 14, SpvOpEntryPoint = 15, SpvOpExecutionMode = 16,
    SpvOpCapability = 17, SpvOpTypeVoid = 19, SpvOpTypeBool = 20, SpvOpTypeInt = 21, SpvOpTypeFloat = 22,
    SpvOpTypeVector = 23, SpvOpTypeImage = 25, SpvOpTypeSampler = 26, SpvOpTypeSampledImage = 27,
    SpvOpTypeArray = 28, SpvOpTypeRuntimeArray = 29, SpvOpTypeStruct = 30, SpvOpTypePointer = 32,
    SpvOpTypeFunction = 33, SpvOpConstantTrue = 41, SpvOpConstantFalse = 42, SpvOpConstant = 43,
    SpvOpConstantComposite = 44, SpvOpFunction = 54, SpvOpFunctionEnd = 56, SpvOpVariable = 59, SpvOpImageTexelPointer = 60, SpvOpLoad = 61,
    SpvOpStore = 62, SpvOpAccessChain = 65, SpvOpArrayLength = 68, SpvOpDecorate = 71, SpvOpMemberDecorate = 72,
    SpvOpVectorExtractDynamic = 77, SpvOpVectorShuffle = 79, SpvOpCompositeConstruct = 80, SpvOpCompositeExtract = 81,
    SpvOpSampledImage = 86, SpvOpImageSampleImplicitLod = 87, SpvOpImageSampleExplicitLod = 88,
    SpvOpImage = 100, SpvOpImageFetch = 95, SpvOpImageRead = 98, SpvOpImageWrite = 99, SpvOpImageQuerySizeLod = 103, SpvOpImageQuerySize = 104, SpvOpImageQueryLod = 105, SpvOpImageQueryLevels = 106, SpvOpImageGather = 96,
    SpvOpConvertFToU = 109, SpvOpConvertFToS = 110, SpvOpConvertSToF = 111, SpvOpConvertUToF = 112,
    SpvOpUConvert = 113, SpvOpSConvert = 114, SpvOpFConvert = 115, SpvOpBitcast = 124, SpvOpSNegate = 126,
    SpvOpFNegate = 127, SpvOpIAdd = 128, SpvOpFAdd = 129, SpvOpISub = 130, SpvOpFSub = 131, SpvOpIMul = 132,
    SpvOpFMul = 133, SpvOpUDiv = 134, SpvOpSDiv = 135, SpvOpFDiv = 136, SpvOpUMod = 137, SpvOpFMod = 141,
    SpvOpIAddCarry = 149, SpvOpISubBorrow = 150, SpvOpUMulExtended = 151, SpvOpSMulExtended = 152,
    SpvOpIsNan = 156, SpvOpIsInf = 157, SpvOpLogicalEqual = 164, SpvOpLogicalNotEqual = 165,
    SpvOpLogicalOr = 166, SpvOpLogicalAnd = 167, SpvOpLogicalNot = 168, SpvOpSelect = 169, SpvOpIEqual = 170,
    SpvOpINotEqual = 171, SpvOpUGreaterThan = 172, SpvOpSGreaterThan = 173, SpvOpUGreaterThanEqual = 174,
    SpvOpSGreaterThanEqual = 175, SpvOpULessThan = 176, SpvOpSLessThan = 177, SpvOpULessThanEqual = 178,
    SpvOpSLessThanEqual = 179, SpvOpFOrdEqual = 180, SpvOpFUnordEqual = 181, SpvOpFOrdNotEqual = 182,
    SpvOpFUnordNotEqual = 183, SpvOpFOrdLessThan = 184, SpvOpFUnordLessThan = 185, SpvOpFOrdGreaterThan = 186,
    SpvOpFUnordGreaterThan = 187, SpvOpFOrdLessThanEqual = 188, SpvOpFUnordLessThanEqual = 189,
    SpvOpFOrdGreaterThanEqual = 190, SpvOpFUnordGreaterThanEqual = 191, SpvOpShiftRightLogical = 194,
    SpvOpShiftRightArithmetic = 195, SpvOpShiftLeftLogical = 196, SpvOpBitwiseOr = 197, SpvOpBitwiseXor = 198,
    SpvOpBitwiseAnd = 199, SpvOpNot = 200, SpvOpBitFieldInsert = 201, SpvOpBitFieldSExtract = 202,
    SpvOpBitFieldUExtract = 203, SpvOpBitReverse = 204, SpvOpBitCount = 205, SpvOpDPdx = 207, SpvOpDPdy = 208,
    SpvOpDot = 148, SpvOpControlBarrier = 224, SpvOpMemoryBarrier = 225, SpvOpAtomicLoad = 227, SpvOpAtomicStore = 228, SpvOpAtomicExchange = 229, SpvOpAtomicCompareExchange = 230,
    SpvOpAtomicIAdd = 234, SpvOpAtomicISub = 235, SpvOpAtomicSMin = 236, SpvOpAtomicUMin = 237, SpvOpAtomicSMax = 238,
    SpvOpAtomicUMax = 239, SpvOpAtomicAnd = 240, SpvOpAtomicOr = 241, SpvOpAtomicXor = 242, SpvOpEmitVertex = 218, SpvOpEndPrimitive = 219, SpvOpPhi = 245, SpvOpLoopMerge = 246,
    SpvOpSelectionMerge = 247, SpvOpLabel = 248, SpvOpBranch = 249, SpvOpBranchConditional = 250,
    SpvOpSwitch = 251, SpvOpKill = 252, SpvOpReturn = 253, SpvOpReturnValue = 254, SpvOpUnreachable = 255,
    SpvOpGroupNonUniformElect = 333, SpvOpGroupNonUniformAll = 334, SpvOpGroupNonUniformAny = 335,
    SpvOpGroupNonUniformBroadcastFirst = 338, SpvOpGroupNonUniformQuadBroadcast = 365, SpvOpGroupNonUniformShuffle = 345, SpvOpGroupNonUniformBallot = 339,
    SpvOpDemoteToHelperInvocationEXT = 5380,
    SpvOpBeginInvocationInterlockEXT = 5364, SpvOpEndInvocationInterlockEXT = 5365,
};

enum {
    SpvCapabilityMatrix = 0, SpvCapabilityShader = 1, SpvCapabilityGeometry = 2, SpvCapabilityFloat16 = 9,
    SpvCapabilityFloat64 = 10, SpvCapabilityInt64 = 11, SpvCapabilityInt16 = 22, SpvCapabilityImageQuery = 50, SpvCapabilityImageGatherExtended = 25,
    SpvCapabilityInt64Atomics = 12, SpvCapabilityWorkgroupMemoryExplicitLayoutKHR = 4428,
    SpvCapabilityFragmentShaderPixelInterlockEXT = 5378,
    SpvCapabilityStorageImageReadWithoutFormat = 55, SpvCapabilityStorageImageWriteWithoutFormat = 56, SpvCapabilityGroupNonUniform = 61,
    SpvCapabilityGroupNonUniformVote = 62, SpvCapabilityGroupNonUniformQuad = 68, SpvCapabilityGroupNonUniformShuffle = 65, SpvCapabilityGroupNonUniformBallot = 64,
};

enum {
    SpvStorageClassUniformConstant = 0, SpvStorageClassInput = 1, SpvStorageClassUniform = 2,
    SpvStorageClassOutput = 3, SpvStorageClassWorkgroup = 4, SpvStorageClassPrivate = 6,
    SpvStorageClassFunction = 7, SpvStorageClassPushConstant = 9, SpvStorageClassImage = 11,
    SpvStorageClassStorageBuffer = 12,
};

enum {
    SpvDecorationBlock = 2, SpvDecorationArrayStride = 6, SpvDecorationBuiltIn = 11, SpvDecorationFlat = 14,
    SpvDecorationInvariant = 18, SpvDecorationNoContraction = 42,
    SpvDecorationNonWritable = 24, SpvDecorationLocation = 30, SpvDecorationBinding = 33,
    SpvDecorationAliased = 20,
    SpvDecorationDescriptorSet = 34, SpvDecorationOffset = 35,
};

enum {
    SpvBuiltInPosition = 0, SpvBuiltInPointSize = 1, SpvBuiltInFragCoord = 15, SpvBuiltInFrontFacing = 17,
    SpvBuiltInLayer = 9, SpvBuiltInFragDepth = 22, SpvBuiltInWorkgroupId = 26, SpvBuiltInLocalInvocationId = 27,
    SpvBuiltInGlobalInvocationId = 28, SpvBuiltInSubgroupLocalInvocationId = 41, SpvBuiltInVertexIndex = 42,
    SpvBuiltInInstanceIndex = 43,
};

enum {
    SpvExecutionModelVertex = 0, SpvExecutionModelGeometry = 3, SpvExecutionModelFragment = 4,
    SpvExecutionModelGLCompute = 5,
};

enum {
    SpvExecutionModeInvocations = 0, SpvExecutionModeOriginUpperLeft = 7, SpvExecutionModeDepthReplacing = 12, SpvExecutionModeLocalSize = 17,
    SpvExecutionModeInputTriangles = 22, SpvExecutionModeOutputVertices = 26, SpvExecutionModeOutputTriangleStrip = 29,
    SpvExecutionModePixelInterlockOrderedEXT = 5366,
};

enum { SpvScopeSubgroup = 3 };
enum { SpvSelectionControlNone = 0, SpvLoopControlNone = 0, SpvFunctionControlNone = 0 };
enum { SpvDimDim2D = 1, SpvDimDim3D = 2, SpvDimBuffer = 5 };
enum { SpvImageFormatUnknown = 0, SpvImageFormatR32i = 24, SpvImageFormatR32ui = 33 };

/* GLSL.std.450 */
enum {
    GLSLstd450Round = 1, GLSLstd450RoundEven = 2, GLSLstd450Trunc = 3, GLSLstd450FAbs = 4, GLSLstd450SAbs = 5,
    GLSLstd450Floor = 8, GLSLstd450Ceil = 9, GLSLstd450Fract = 10, GLSLstd450Sin = 13, GLSLstd450Cos = 14,
    GLSLstd450Pow = 26, GLSLstd450Exp2 = 29, GLSLstd450Log2 = 30, GLSLstd450Sqrt = 31, GLSLstd450InverseSqrt = 32,
    GLSLstd450FMin = 37, GLSLstd450UMin = 38, GLSLstd450SMin = 39, GLSLstd450FMax = 40, GLSLstd450UMax = 41,
    GLSLstd450SMax = 42, GLSLstd450FClamp = 43, GLSLstd450UClamp = 44, GLSLstd450SClamp = 45, GLSLstd450FMix = 46,
    GLSLstd450Fma = 50, GLSLstd450Ldexp = 53, GLSLstd450PackSnorm2x16 = 56, GLSLstd450PackUnorm2x16 = 57, GLSLstd450PackHalf2x16 = 58, GLSLstd450UnpackSnorm2x16 = 60, GLSLstd450UnpackUnorm2x16 = 61,
    GLSLstd450UnpackHalf2x16 = 62,
    GLSLstd450FindILsb = 73, GLSLstd450FindSMsb = 74, GLSLstd450FindUMsb = 75, GLSLstd450NMin = 79,
    GLSLstd450NMax = 80,
};

#endif

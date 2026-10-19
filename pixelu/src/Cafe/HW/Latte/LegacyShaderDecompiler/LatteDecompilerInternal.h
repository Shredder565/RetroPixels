// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Decoded program representation and shared state of the legacy decompiler.
// These types are private to the LegacyShaderDecompiler directory (GLSL and
// MSL back ends). Field names and layout are relied on by both emitters.

// one slot of an ALU instruction group
struct LatteDecompilerALUInstruction
{
	struct LatteDecompilerCFInstruction* cfInstruction{}; // owning clause
	bool isOP3{};
	uint32 opcode{};
	uint32 instructionGroupIndex{}; // group number inside the clause
	uint8 indexMode{}; // which AR component (or loop index) relative operands use
	uint8 omod{};
	// result
	uint32 destGpr{};
	uint8 destRel{};
	uint8 destElem{};
	uint8 destClamp{};
	uint8 writeMask{}; // 0 means the result only goes to PV/PS
	uint8 updateExecuteMask{};
	uint8 updatePredicate{};
	// inputs; sel == 0xFFFFFFFF marks an operand slot the encoding does not have
	struct
	{
		uint32 sel{};
		uint8 rel{};
		uint8 abs{};
		uint8 neg{};
		uint8 chan{};
	}sourceOperand[3];
	// literal constants of the group this instruction belongs to
	union
	{
		uint32 w[4];
		float f[4];
	}literalData;
	// filled while decoding
	uint8 aluUnit{}; // 0..3 vector unit x/y/z/w (PV), 4 = scalar trans unit (PS)
	uint8 indexInGroup{};
	bool isLastInstructionOfGroup{};
};

// one entry of a TEX/VTX clause
struct LatteDecompilerTEXInstruction
{
	struct LatteDecompilerCFInstruction* cfInstruction{};
	uint32 opcode{};
	// common to all fetch kinds
	sint32 srcGpr;
	sint32 dstGpr;
	sint8 dstSel[4];
	// texture sample / vertex fetch fields
	struct
	{
		sint32 textureIndex{}; // resource id (texture unit or vertex buffer id)
		sint32 samplerIndex{};
		uint32 offset{}; // byte offset for vertex fetches
		sint8 srcSel[4]{};
		sint8 offsetX{};
		sint8 offsetY{};
		sint8 offsetZ{};
		bool unnormalized[4]{}; // coordinate given in texels instead of [0,1]
		sint8 lodBias{}; // fixed point, 1/16 steps
	}textureFetch;
	// scatter buffer read fields
	struct
	{
		uint32 arrayBase{};
		sint8 srcSelX{};
		Latte::E_HWFMT format{};
		uint8 nfa{};
		uint8 isSigned{};
	}memRead;
};

// one control flow instruction, with the clause it references (if any)
struct LatteDecompilerCFInstruction
{
	uint32 type{}; // GPU7_CF_INST_*
	uint32 cfAddr{}; // index of this CF slot in the program
	// clause location
	uint32 addr{};
	sint32 count{};
	// a clause holds either ALU or TEX entries, never both
	std::vector<LatteDecompilerALUInstruction> instructionsALU;
	std::vector<LatteDecompilerTEXInstruction> instructionsTEX;
	// constant cache setup of ALU clauses
	uint32 cBank0Index{};
	uint32 cBank1Index{};
	uint32 cBank0AddrBase{};
	uint32 cBank1AddrBase{};
	// export fields
	uint32 exportType{};
	uint8  exportComponentSel[4]{};
	uint32 exportBurstCount{};
	// memory export fields
	uint32 memWriteArraySize{};
	uint8 memWriteCompMask{};
	uint8 memWriteElemSize{};
	// shared by exports and memory exports
	uint32 exportArrayBase{};
	uint32 exportSourceGPR{};
	// condition and stack
	uint32 cfCond{};
	uint32 popCount{};
	// filled by the analyzer
	bool modifiesPredicate{};
	bool modifiesActiveMask{};
	uint32 numPredInstructions{};
	sint32 activeStackDepth{}; // execution mask stack level this instruction runs at

	LatteDecompilerCFInstruction() = default;
	~LatteDecompilerCFInstruction()
	{
		cemu_assert_debug(instructionsALU.empty() || instructionsTEX.empty());
	}
	LatteDecompilerCFInstruction(const LatteDecompilerCFInstruction& other) = default;
	LatteDecompilerCFInstruction(LatteDecompilerCFInstruction&& other) = default;
	LatteDecompilerCFInstruction& operator=(LatteDecompilerCFInstruction&& other) = default;
};

// a CALL target and the CF instructions up to its RETURN
struct LatteDecompilerSubroutineInfo
{
	uint32 cfAddr;
	std::vector<LatteDecompilerCFInstruction> instructions;
};

// records which entries of a constant file / constant buffer are read
struct LatteDecompilerBufferAccessTracker
{
	bool hasStaticIndexAccess{false};
	bool hasDynamicIndexAccess{false};
	sint32 highestAccessDynamicIndex{0};
	sint32 highestAccessStaticIndex{0};

	// index counts vec4 entries
	void TrackAccess(sint32 index, bool isDynamicIndex)
	{
		bool& seen = isDynamicIndex ? hasDynamicIndexAccess : hasStaticIndexAccess;
		sint32& highest = isDynamicIndex ? highestAccessDynamicIndex : highestAccessStaticIndex;
		seen = true;
		highest = std::max(highest, index);
	}

	// number of vec4 entries that must be made available to the shader
	sint32 DetermineSize(uint64 shaderBaseHash, sint32 maximumSize) const
	{
		// a few known shaders index dynamically but only ever touch a small range
		switch (shaderBaseHash)
		{
		case 0x8ff56afdf1a2f837: return 24; // XCX text
		case 0x37b9100c1310d3bb: return 24; // BotW UI backdrop
		case 0xf7ba548c1fefe24a: return 30; // BotW UI backdrop
		default: break;
		}
		if (hasDynamicIndexAccess)
			return maximumSize;
		if (!hasStaticIndexAccess)
			return 1; // never return an empty array
		return highestAccessStaticIndex + 1;
	}

	bool HasAccess() const
	{
		return hasStaticIndexAccess || hasDynamicIndexAccess;
	}

	bool HasRelativeAccess() const
	{
		return hasDynamicIndexAccess;
	}
};

// all state of one decompilation run
struct LatteDecompilerShaderContext
{
	LatteDecompilerOutput_t* output;
	LatteDecompilerShader* shader;
	LatteConst::ShaderType shaderType;
	const class LatteDecompilerOptions* options;
	uint32* contextRegisters; // raw register file
	struct LatteContextRegister* contextRegistersNew; // same memory, typed view
	uint64 shaderBaseHash;
	StringBuf* shaderSource;
	std::vector<LatteDecompilerCFInstruction> cfInstructions; // main program
	LatteFetchShader* fetchShader{}; // vertex shaders only
	LatteParsedGSCopyShader* parsedGSCopyShader; // geometry shaders only
	bool hasError;
	// register representation chosen after analysis
	struct
	{
		uint8 defaultDataType; // LATTE_DECOMPILER_DTYPE_*
		bool genFloatReg; // declare R*f registers
		bool genIntReg; // declare R*i registers
		bool useArrayGPRs; // registers live in one indexable array
	}typeTracker;
	// facts gathered by the analyzer
	struct
	{
		bool hasStreamoutEnable{};
		bool hasLoops{};
		// vertex stage
		bool isPointsPrimitive{};
		bool outputPointSize{};
		std::bitset<256> inputAttributSemanticMask; // indexed by semantic id
		// uniform usage
		LatteDecompilerBufferAccessTracker uniformRegisterAccessTracker;
		LatteDecompilerBufferAccessTracker uniformBufferAccessTracker[LATTE_NUM_MAX_UNIFORM_BUFFERS];
		// storage buffers
		bool hasSSBORead;
		bool hasSSBOWrite;
		// textures
		std::bitset<LATTE_NUM_MAX_TEX_UNITS> texUnitUsesTexelCoordinates;
		bool hasCubeMapTexture;
		bool hasGradientLookup;
		// registers and misc
		bool usesRelativeGPRRead;
		bool usesRelativeGPRWrite;
		uint8 gprUseMask[(LATTE_NUM_GPR + 7) / 8]; // GPRs accessed with a fixed index
		bool hasStreamoutWrite;
		bool hasRedcCUBE;
		bool hasFragCoordAccess{false};
		bool modifiesPixelActiveState; // any instruction that changes the execution mask
		bool usesIntegerValues;
		sint32 activeStackMaxDepth;
		bool writesPointSize{};
		bool useSSBOForStreamout{};
		uint32 numEmitVertex{}; // geometry shader EMIT_VERTEX count
	}analyzer;

	// set while a subroutine body is being emitted
	bool isSubroutine;
	LatteDecompilerSubroutineInfo* subroutineInfo;

	// emitter state
	bool hasUniformVarBlock;
	sint32 currentBindingPointVK{};
	sint32 currentBufferBindingPointMTL{};
	sint32 currentTextureBindingPointMTL{};
	struct ALUClauseTemporariesState* aluPVPSState{nullptr}; // used by the MSL emitter

	std::vector<LatteDecompilerSubroutineInfo> list_subroutines;
};

// stages, called in this order by LatteDecompiler.cpp
void LatteDecompiler_analyze(LatteDecompilerShaderContext* shaderContext, LatteDecompilerShader* shader);
void LatteDecompiler_analyzeDataTypes(LatteDecompilerShaderContext* shaderContext);
void LatteDecompiler_emitGLSLShader(LatteDecompilerShaderContext* shaderContext, LatteDecompilerShader* shader);
#ifdef ENABLE_METAL
void LatteDecompiler_emitMSLShader(LatteDecompilerShaderContext* shaderContext, LatteDecompilerShader* shader);
#endif

void LatteDecompiler_cleanup(LatteDecompilerShaderContext* shaderContext);

// maps a pixel shader export slot to the color attachment it lands in (-1 if none)
sint32 LatteDecompiler_getColorOutputIndexFromExportIndex(LatteDecompilerShaderContext* shaderContext, sint32 exportIndex);

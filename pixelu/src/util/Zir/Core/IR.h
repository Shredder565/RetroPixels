// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include <optional>
#include <initializer_list>

using f32 = float;
using f64 = double;

// IR consistency check that is active in all builds
inline void zpir_debug_assert(bool _cond)
{
	if (!_cond)
		assert_dbg();
}

// ZpIR: a small SSA-style intermediate representation. A function is a set
// of basic blocks; each block owns its virtual registers, its constants and
// a singly linked list of instructions.
//
// An IRReg below 0x8000 names a virtual register of the block, from 0x8000
// upwards it names constant (index - 0x8000) of the block.
namespace ZpIR
{
	enum class DataType : uint8
	{
		NONE = 0x00,
		U8 = 1,
		S8 = 2,
		U16 = 3,
		S16 = 4,
		U32 = 5,
		S32 = 6,
		U64 = 7,
		S64 = 8,
		F32 = 0x10,
		F64 = 0x11,
		POINTER = 0x20, // host pointer width
		BOOL = 0x30,
	};

	typedef uint16 IRReg;
	typedef uint64 LocationSymbolName;
	typedef uint32 ZpIRPhysicalReg;

	inline constexpr uint16 kConstRegFlag = 0x8000;

	inline bool isRegVar(IRReg r) { return r < kConstRegFlag; }
	inline bool isConstVar(IRReg r) { return r >= kConstRegFlag; }
	inline uint16 getRegIndex(IRReg r) { return (uint16)(r & (kConstRegFlag - 1)); }

	namespace IR
	{
		enum class OpCode : uint8
		{
			UNDEF = 0,
			MOV,
			// arithmetic
			ADD,
			SUB,
			MUL,
			DIV,
			// conversion
			BITCAST, // reinterpret the bits as another type of the same size
			SWAP_ENDIAN,
			CONVERT_INT_TO_FLOAT,
			CONVERT_FLOAT_TO_INT,
			// external data
			IMPORT_SINGLE, // legacy single-register import
			IMPORT, // read 1-4 registers from an external location
			EXPORT, // write 1-4 registers to an external location
			// vector
			EXTRACT_ELEMENT,
		};

		// operand layout of an instruction
		enum class OpForm : uint8
		{
			NONE = 0,
			RR = 1, // rA, rB
			RRR = 2, // rA, rB, rC
			IMPORT_SINGLE = 3,
			IMPORT = 4,
			EXPORT = 5,
		};

		// common header of all instructions
		class __InsBase
		{
		public:
			OpCode opcode;
			OpForm opform;
			__InsBase* next;

		protected:
			__InsBase(OpCode opcode, OpForm opform) : opcode(opcode), opform(opform), next(nullptr) {}
		};

		// adds form queries to the header: getIfForm() casts when the form matches
		template<typename TInstr, OpForm TOpForm>
		class __InsBaseWithForm : public __InsBase
		{
		public:
			static const OpForm getForm()
			{
				return TOpForm;
			}

			static TInstr* getIfForm(__InsBase* instructionBase)
			{
				return instructionBase->opform == TOpForm ? static_cast<TInstr*>(instructionBase) : nullptr;
			}

		protected:
			__InsBaseWithForm(OpCode opcode) : __InsBase(opcode, TOpForm) {}
		};

		// rA = op(rB); some opcodes treat rA as an operand too
		class InsRR : public __InsBaseWithForm<InsRR, OpForm::RR>
		{
		public:
			InsRR(OpCode opcode, IRReg rA, IRReg rB) : __InsBaseWithForm(opcode), rA(rA), rB(rB) {}

			IRReg rA;
			IRReg rB;
		};

		// rA = op(rB, rC)
		class InsRRR : public __InsBaseWithForm<InsRRR, OpForm::RRR>
		{
		public:
			InsRRR(OpCode opcode, IRReg rA, IRReg rB, IRReg rC) : __InsBaseWithForm(opcode), rA(rA), rB(rB), rC(rC) {}

			IRReg rA;
			IRReg rB;
			IRReg rC;
		};

		// shared storage of IMPORT / EXPORT: up to four registers
		template<typename TInstr, OpForm TOpForm>
		class InsRegisterList : public __InsBaseWithForm<TInstr, TOpForm>
		{
		public:
			uint16 count{ 0 };
			IRReg regArray[4]{};

		protected:
			InsRegisterList(OpCode opcode, std::initializer_list<IRReg> regs) : __InsBaseWithForm<TInstr, TOpForm>(opcode)
			{
				for (IRReg r : regs)
					regArray[count++] = r;
			}

			InsRegisterList(OpCode opcode, std::span<IRReg> regs) : __InsBaseWithForm<TInstr, TOpForm>(opcode)
			{
				zpir_debug_assert(regs.size() <= 4);
				for (IRReg r : regs)
					regArray[count++] = r;
			}
		};

		class InsEXPORT : public InsRegisterList<InsEXPORT, OpForm::EXPORT>
		{
		public:
			InsEXPORT(LocationSymbolName exportSymbol, IRReg r0) : InsRegisterList(OpCode::EXPORT, { r0 }), exportSymbol(exportSymbol) {}
			InsEXPORT(LocationSymbolName exportSymbol, IRReg r0, IRReg r1) : InsRegisterList(OpCode::EXPORT, { r0, r1 }), exportSymbol(exportSymbol) {}
			InsEXPORT(LocationSymbolName exportSymbol, IRReg r0, IRReg r1, IRReg r2) : InsRegisterList(OpCode::EXPORT, { r0, r1, r2 }), exportSymbol(exportSymbol) {}
			InsEXPORT(LocationSymbolName exportSymbol, IRReg r0, IRReg r1, IRReg r2, IRReg r3) : InsRegisterList(OpCode::EXPORT, { r0, r1, r2, r3 }), exportSymbol(exportSymbol) {}
			InsEXPORT(LocationSymbolName exportSymbol, std::span<IRReg> regs) : InsRegisterList(OpCode::EXPORT, regs), exportSymbol(exportSymbol) {}

			LocationSymbolName exportSymbol;
		};

		class InsIMPORT : public InsRegisterList<InsIMPORT, OpForm::IMPORT>
		{
		public:
			InsIMPORT(LocationSymbolName importSymbol, IRReg r0) : InsRegisterList(OpCode::IMPORT, { r0 }), importSymbol(importSymbol) {}
			InsIMPORT(LocationSymbolName importSymbol, IRReg r0, IRReg r1) : InsRegisterList(OpCode::IMPORT, { r0, r1 }), importSymbol(importSymbol) {}
			InsIMPORT(LocationSymbolName importSymbol, IRReg r0, IRReg r1, IRReg r2) : InsRegisterList(OpCode::IMPORT, { r0, r1, r2 }), importSymbol(importSymbol) {}
			InsIMPORT(LocationSymbolName importSymbol, IRReg r0, IRReg r1, IRReg r2, IRReg r3) : InsRegisterList(OpCode::IMPORT, { r0, r1, r2, r3 }), importSymbol(importSymbol) {}
			InsIMPORT(LocationSymbolName importSymbol, std::span<IRReg> regs) : InsRegisterList(OpCode::IMPORT, regs), importSymbol(importSymbol) {}

			LocationSymbolName importSymbol;
		};
	};

	// virtual register of a block
	struct IRRegDef
	{
		static constexpr ZpIRPhysicalReg kNoPhysicalRegister = std::numeric_limits<ZpIRPhysicalReg>::max();

		IRRegDef(DataType type, uint8 elementCount) : type(type), elementCount(elementCount) {}

		DataType type;
		uint8 elementCount; // 1 for scalars
		ZpIRPhysicalReg physicalRegister{ kNoPhysicalRegister }; // set by a register allocation pass

		bool hasAssignedPhysicalRegister() const
		{
			return physicalRegister != kNoPhysicalRegister;
		}

		void assignPhysicalRegister(ZpIRPhysicalReg physReg)
		{
			physicalRegister = physReg;
		}
	};

	// constant of a block (scalar)
	struct IRRegConstDef
	{
		IRRegConstDef() : value_u64(0) {}

		IRRegConstDef& setU32(uint32 v) { value_u32 = v; type = DataType::U32; return *this; }
		IRRegConstDef& setS32(sint32 v) { value_s32 = v; type = DataType::S32; return *this; }
		IRRegConstDef& setF32(f32 v) { value_f32 = v; type = DataType::F32; return *this; }
		IRRegConstDef& setPtr(void* v) { value_ptr = v; type = DataType::POINTER; return *this; }
		// raw 32-bit pattern with an explicit type
		IRRegConstDef& setRaw(uint32 v, DataType regType) { value_u32 = v; type = regType; return *this; }

		DataType type{ DataType::NONE };
		union
		{
			uint32 value_u32;
			sint32 value_s32;
			sint64 value_s64;
			uint64 value_u64;
			void* value_ptr;
			f32 value_f32;
			f64 value_f64;
		};
	};

	struct ZpIRBasicBlock
	{
		friend class ZpIRBuilder;

		// register that receives a value from a predecessor block
		struct IRBBImport
		{
			IRBBImport(IRReg reg, LocationSymbolName name) : reg(reg), name(name) {}
			IRReg reg;
			LocationSymbolName name;
		};

		// register whose value is handed to successor blocks
		struct IRBBExport
		{
			IRBBExport(IRReg reg, LocationSymbolName name) : reg(reg), name(name) {}
			IRReg reg;
			LocationSymbolName name;
		};

		IR::__InsBase* m_instructionFirst{};
		IR::__InsBase* m_instructionLast{};
		std::vector<IRRegDef> m_regs;
		std::vector<IRRegConstDef> m_consts;
		std::vector<IRBBImport> m_imports;
		std::vector<IRBBExport> m_exports;
		ZpIRBasicBlock* m_branchNotTaken{ nullptr }; // fall-through successor
		ZpIRBasicBlock* m_branchTaken{ nullptr };

		void* m_workbuffer{}; // scratch pointer for passes

		void appendInstruction(IR::__InsBase* ins)
		{
			ins->next = nullptr;
			if (m_instructionLast)
				m_instructionLast->next = ins;
			else
				m_instructionFirst = ins;
			m_instructionLast = ins;
		}

		IRReg createRegister(DataType type, uint8 elementCount = 1)
		{
			const size_t index = m_regs.size();
			cemu_assert_debug(index < kConstRegFlag);
			m_regs.emplace_back(type, elementCount);
			return (IRReg)index;
		}

		IRReg createConstantU32(uint32 value) { return pushConstant(IRRegConstDef().setU32(value)); }
		IRReg createTypedConstant(uint32 value, DataType type) { return pushConstant(IRRegConstDef().setRaw(value, type)); }
		IRReg createConstantS32(uint32 value) { return pushConstant(IRRegConstDef().setS32((sint32)value)); }
		IRReg createConstantF32(f32 value) { return pushConstant(IRRegConstDef().setF32(value)); }
		IRReg createConstantPointer(void* value) { return pushConstant(IRRegConstDef().setPtr(value)); }

		void addImport(IRReg reg, LocationSymbolName importName)
		{
			m_imports.emplace_back(reg, importName);
		}

		void addExport(IRReg reg, LocationSymbolName importName)
		{
			m_exports.emplace_back(reg, importName);
		}

		// only one pass may own the work buffer at a time
		void setWorkbuffer(void* buffer)
		{
			if (buffer && m_workbuffer)
				assert_dbg();
			m_workbuffer = buffer;
		}

		void* getWorkbuffer()
		{
			return m_workbuffer;
		}

		DataType getRegType(IRReg reg)
		{
			if (isConstVar(reg))
			{
				const uint16 index = getRegIndex(reg);
				cemu_assert_debug(index < m_consts.size());
				return m_consts[index].type;
			}
			return m_regs[reg].type;
		}

		// constant definition, nullptr for virtual registers or unknown constants
		IRRegConstDef* getConstant(IRReg reg)
		{
			if (!isConstVar(reg))
				return nullptr;
			const uint16 index = getRegIndex(reg);
			if (index >= m_consts.size())
				return nullptr;
			return &m_consts[index];
		}

		// integer constant as signed 32-bit (U32 and small U64 constants only)
		std::optional<sint32> getConstantS32(IRReg reg)
		{
			IRRegConstDef* c = getConstant(reg);
			if (!c)
				return std::nullopt;
			switch (c->type)
			{
			case DataType::U32:
				return (sint32)c->value_u32;
			case DataType::U64:
				if (c->value_u64 >= 0x80000000ull)
					assert_dbg();
				return (sint32)c->value_u64;
			default:
				assert_dbg();
				return std::nullopt;
			}
		}

		std::optional<uint64> getConstantU64(IRReg reg)
		{
			IRRegConstDef* c = getConstant(reg);
			if (!c)
				return std::nullopt;
			if (c->type != DataType::U64)
			{
				assert_dbg();
				return std::nullopt;
			}
			return c->value_u64;
		}

	private:
		IRReg pushConstant(const IRRegConstDef& c)
		{
			const size_t index = m_consts.size();
			cemu_assert_debug(index < kConstRegFlag);
			m_consts.push_back(c);
			return (IRReg)(kConstRegFlag + (uint16)index);
		}
	};

	struct ZpIRFunction
	{
		std::vector<ZpIRBasicBlock*> m_basicBlocks;
		std::vector<ZpIRBasicBlock*> m_entryBlocks;
		std::vector<ZpIRBasicBlock*> m_exitBlocks;

		struct
		{
			bool registersAllocated{ false };
		}state;
	};

	// location names used by shader programs
	namespace ShaderSubset
	{
		// Encoded as a LocationSymbolName: kind in bits 56-63, index A in bits 0-15,
		// index B in bits 16-31.
		namespace detail
		{
			inline LocationSymbolName packLocation(uint8 kind, uint16 a, uint16 b)
			{
				return ((uint64)kind << 56) | ((uint64)b << 16) | (uint64)a;
			}

			inline const char kChannelNames[4] = { 'x', 'y', 'z', 'w' };
		}

		// uniform register (index = register * 4 + channel) or vertex attribute channel
		class ShaderImportLocation
		{
			enum LOC_TYPE : uint8
			{
				LOC_TYPE_UNIFORM_REGISTER = 1,
				LOC_TYPE_UNIFORM_BUFFER = 2,
				LOC_TYPE_ATTRIBUTE = 3,
			};

		public:
			ShaderImportLocation() = default;
			ShaderImportLocation(LocationSymbolName loc) : m_kind((LOC_TYPE)(loc >> 56)), m_a((uint16)loc), m_b((uint16)(loc >> 16)) {}

			ShaderImportLocation& SetUniformRegister(uint16 index)
			{
				return set(LOC_TYPE_UNIFORM_REGISTER, index, 0);
			}

			ShaderImportLocation& SetVertexAttribute(uint16 attributeIndex, uint16 channelIndex)
			{
				return set(LOC_TYPE_ATTRIBUTE, attributeIndex, channelIndex);
			}

			bool IsUniformRegister() const { return m_kind == LOC_TYPE_UNIFORM_REGISTER; }
			bool IsVertexAttribute() const { return m_kind == LOC_TYPE_ATTRIBUTE; }

			void GetUniformRegister(uint16& index) { index = m_a; }

			void GetVertexAttribute(uint16& attributeIndex, uint16& channelIndex) const
			{
				attributeIndex = m_a;
				channelIndex = m_b;
			}

			operator LocationSymbolName() const { return detail::packLocation(m_kind, m_a, m_b); }

			std::string GetDebugName()
			{
				if (IsUniformRegister())
					return fmt::format("UniformReg[{0}].{1}", m_a >> 2, detail::kChannelNames[m_a & 3]);
				if (IsVertexAttribute())
					return fmt::format("VertexAttribute[{0}].{1}", m_a, detail::kChannelNames[m_b & 3]);
				return "Unknown";
			}

		private:
			ShaderImportLocation& set(LOC_TYPE kind, uint16 a, uint16 b)
			{
				m_kind = kind;
				m_a = a;
				m_b = b;
				return *this;
			}

			LOC_TYPE m_kind{};
			uint16 m_a{};
			uint16 m_b{};
		};

		// vertex position or an output attribute (parameter)
		class ShaderExportLocation
		{
			enum LOC_TYPE : uint8
			{
				LOC_TYPE_POSITION = 1,
				LOC_TYPE_OUTPUT = 2,
			};

		public:
			ShaderExportLocation() = default;
			ShaderExportLocation(LocationSymbolName loc) : m_kind((LOC_TYPE)(loc >> 56)), m_a((uint16)loc), m_b((uint16)(loc >> 16)) {}

			ShaderExportLocation& SetPosition()
			{
				m_kind = LOC_TYPE_POSITION;
				m_a = 0;
				m_b = 0;
				return *this;
			}

			ShaderExportLocation& SetOutputAttribute(uint16 attributeIndex)
			{
				m_kind = LOC_TYPE_OUTPUT;
				m_a = attributeIndex;
				m_b = 0;
				return *this;
			}

			bool IsPosition() const { return m_kind == LOC_TYPE_POSITION; }
			bool IsOutputAttribute() const { return m_kind == LOC_TYPE_OUTPUT; }

			void GetOutputAttribute(uint16& attributeIndex) const { attributeIndex = m_a; }

			operator LocationSymbolName() const { return detail::packLocation(m_kind, m_a, m_b); }

			std::string GetDebugName()
			{
				if (IsPosition())
					return "Position";
				if (IsOutputAttribute())
					return fmt::format("Output[{}]", m_a);
				return "Unknown";
			}

		private:
			LOC_TYPE m_kind{};
			uint16 m_a{};
			uint16 m_b{};
		};
	};
}

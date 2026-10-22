// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "util/Zir/Core/IR.h"

namespace ZpIR
{
	// Appends instructions, registers and constants to one basic block.
	class BasicBlockBuilder
	{
	public:
		BasicBlockBuilder(ZpIRBasicBlock* basicBlock) : m_block(basicBlock) {}

		IRReg createReg(DataType type, uint8 elementCount = 1)
		{
			return m_block->createRegister(type, elementCount);
		}

		// also stores the new register in r
		IRReg createReg(IRReg& r, DataType type, uint8 elementCount = 1)
		{
			r = m_block->createRegister(type, elementCount);
			return r;
		}

		// not supported, use emit()
		void append(IR::__InsBase* ins)
		{
			(void)ins;
			assert_dbg();
		}

		void emit(IR::__InsBase* ins)
		{
			m_block->appendInstruction(ins);
		}

		void emit_EXPORT(LocationSymbolName exportSymbolName, IRReg r0)
		{
			emit(new IR::InsEXPORT(exportSymbolName, r0));
		}

		void emit_EXPORT(LocationSymbolName exportSymbolName, std::span<IRReg> regs)
		{
			emit(new IR::InsEXPORT(exportSymbolName, regs));
		}

		void emit_IMPORT(LocationSymbolName importSymbolName, IRReg r0)
		{
			emit(new IR::InsIMPORT(importSymbolName, r0));
		}

		// rA receives the result of op(rB)
		void emit_RR(IR::OpCode opcode, IRReg rA, IRReg rB)
		{
			emit(new IR::InsRR(opcode, rA, rB));
		}

		// creates the result register
		IRReg emit_RR(IR::OpCode opcode, DataType resultType, IRReg rB)
		{
			const IRReg result = createReg(resultType);
			emit_RR(opcode, result, rB);
			return result;
		}

		// rA receives the result of op(rB, rC)
		void emit_RRR(IR::OpCode opcode, IRReg rA, IRReg rB, IRReg rC)
		{
			emit(new IR::InsRRR(opcode, rA, rB, rC));
		}

		IRReg emit_RRR(IR::OpCode opcode, DataType resultType, IRReg rB, IRReg rC)
		{
			const IRReg result = createReg(resultType);
			emit_RRR(opcode, result, rB, rC);
			return result;
		}

		IRReg createConstU32(uint32 v) { return m_block->createConstantU32(v); }
		IRReg createTypedConst(uint32 v, DataType type) { return m_block->createTypedConstant(v, type); }
		IRReg createConstS32(uint32 v) { return m_block->createConstantS32(v); }
		IRReg createConstF32(f32 v) { return m_block->createConstantF32(v); }
		IRReg createConstPointer(void* v) { return m_block->createConstantPointer(v); }

		DataType getRegType(IRReg reg)
		{
			return m_block->getRegType(reg);
		}

		void addImport(IRReg reg, LocationSymbolName importSymbolName)
		{
			m_block->addImport(reg, importSymbolName);
		}

	private:
		ZpIRBasicBlock* m_block;
	};

	// Builds a function block by block. Blocks are named; branch targets refer
	// to names and are resolved by finish().
	class ZpIRBuilder
	{
	public:
		typedef uint64 BlockBranchTarget;

		static const inline BlockBranchTarget INVALID_BLOCK_NAME = 0xFFFFFFFFFFFFFFFFull;

		// per-block data kept in the block's work buffer until finish()
		struct BasicBlockWorkbuffer
		{
			BlockBranchTarget name{ INVALID_BLOCK_NAME };
			BlockBranchTarget targetBranchNotTaken{ INVALID_BLOCK_NAME };
			BlockBranchTarget targetBranchTaken{ INVALID_BLOCK_NAME };
		};

		void beginBlock(BlockBranchTarget name)
		{
			auto* block = new ZpIRBasicBlock();
			auto* info = new BasicBlockWorkbuffer();
			info->name = name;
			block->setWorkbuffer(info);
			m_current = block;
			m_blocks.push_back(block);
			m_blocksByName.emplace(name, block);
		}

		ZpIRBasicBlock* endBlock()
		{
			ZpIRBasicBlock* block = m_current;
			auto* info = static_cast<BasicBlockWorkbuffer*>(block->getWorkbuffer());
			info->targetBranchNotTaken = std::exchange(m_pendingNotTaken, INVALID_BLOCK_NAME);
			info->targetBranchTaken = std::exchange(m_pendingTaken, INVALID_BLOCK_NAME);
			m_current = nullptr;
			return block;
		}

		// links the blocks and returns the function (all blocks in creation order)
		ZpIRFunction* finish()
		{
			if (m_current)
				assert_dbg(); // endBlock() missing
			auto* func = new ZpIRFunction();
			for (ZpIRBasicBlock* block : m_blocks)
			{
				auto* info = static_cast<BasicBlockWorkbuffer*>(block->getWorkbuffer());
				block->m_branchNotTaken = resolve(info->targetBranchNotTaken);
				block->m_branchTaken = resolve(info->targetBranchTaken);
				block->setWorkbuffer(nullptr);
				delete info;
				func->m_basicBlocks.push_back(block);
			}
			return func;
		}

		IRReg createBlockRegister(DataType type, uint8 elementCount = 1) { return m_current->createRegister(type, elementCount); }
		IRReg createConstU32(uint32 v) { return m_current->createConstantU32(v); }
		IRReg createConstPointer(void* v) { return m_current->createConstantPointer(v); }
		IRReg createConstPointerV(size_t v) { return m_current->createConstantPointer((void*)v); }

		void addImport(IRReg reg, LocationSymbolName importName) { m_current->addImport(reg, importName); }
		void addExport(IRReg reg, LocationSymbolName importName) { m_current->addExport(reg, importName); }

		void setBlockTargetBranchTaken(BlockBranchTarget target)
		{
			if (m_pendingTaken != INVALID_BLOCK_NAME)
				assert_dbg();
			m_pendingTaken = target;
		}

		void setBlockTargetBranchNotTaken(BlockBranchTarget target)
		{
			if (m_pendingNotTaken != INVALID_BLOCK_NAME)
				assert_dbg();
			m_pendingNotTaken = target;
		}

	private:
		ZpIRBasicBlock* resolve(BlockBranchTarget name)
		{
			if (name == INVALID_BLOCK_NAME)
				return nullptr;
			auto it = m_blocksByName.find(name);
			if (it == m_blocksByName.end())
			{
				assert_dbg(); // branch to a block that was never created
				return nullptr;
			}
			return it->second;
		}

		ZpIRBasicBlock* m_current{};
		std::vector<ZpIRBasicBlock*> m_blocks;
		std::unordered_map<BlockBranchTarget, ZpIRBasicBlock*> m_blocksByName;
		BlockBranchTarget m_pendingNotTaken{ INVALID_BLOCK_NAME };
		BlockBranchTarget m_pendingTaken{ INVALID_BLOCK_NAME };
	};
}

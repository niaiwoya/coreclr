// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

/*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XX                                                                           XX
XX                        ARM Code Generator                                 XX
XX                                                                           XX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
*/
#include "jitpch.h"
#ifdef _MSC_VER
#pragma hdrstop
#endif

#ifndef LEGACY_BACKEND // This file is ONLY used for the RyuJIT backend that uses the linear scan register allocator

#ifdef _TARGET_ARM_
#include "codegen.h"
#include "lower.h"
#include "gcinfo.h"
#include "emit.h"

//------------------------------------------------------------------------
// genCallFinally: Generate a call to the finally block.
//
BasicBlock* CodeGen::genCallFinally(BasicBlock* block)
{
    BasicBlock* bbFinallyRet = nullptr;

    // We don't have retless calls, since we use the BBJ_ALWAYS to point at a NOP pad where
    // we would have otherwise created retless calls.
    assert(block->isBBCallAlwaysPair());

    assert(block->bbNext != NULL);
    assert(block->bbNext->bbJumpKind == BBJ_ALWAYS);
    assert(block->bbNext->bbJumpDest != NULL);
    assert(block->bbNext->bbJumpDest->bbFlags & BBF_FINALLY_TARGET);

    bbFinallyRet = block->bbNext->bbJumpDest;
    bbFinallyRet->bbFlags |= BBF_JMP_TARGET;

    // Load the address where the finally funclet should return into LR.
    // The funclet prolog/epilog will do "push {lr}" / "pop {pc}" to do the return.
    getEmitter()->emitIns_R_L(INS_movw, EA_4BYTE_DSP_RELOC, bbFinallyRet, REG_LR);
    getEmitter()->emitIns_R_L(INS_movt, EA_4BYTE_DSP_RELOC, bbFinallyRet, REG_LR);

    // Jump to the finally BB
    inst_JMP(EJ_jmp, block->bbJumpDest);

    // The BBJ_ALWAYS is used because the BBJ_CALLFINALLY can't point to the
    // jump target using bbJumpDest - that is already used to point
    // to the finally block. So just skip past the BBJ_ALWAYS unless the
    // block is RETLESS.
    assert(!(block->bbFlags & BBF_RETLESS_CALL));
    assert(block->isBBCallAlwaysPair());
    return block->bbNext;
}

//------------------------------------------------------------------------
// genEHCatchRet:
void CodeGen::genEHCatchRet(BasicBlock* block)
{
    getEmitter()->emitIns_R_L(INS_movw, EA_4BYTE_DSP_RELOC, block->bbJumpDest, REG_INTRET);
    getEmitter()->emitIns_R_L(INS_movt, EA_4BYTE_DSP_RELOC, block->bbJumpDest, REG_INTRET);
}

//------------------------------------------------------------------------
// instGen_Set_Reg_To_Imm: Move an immediate value into an integer register.
//
void CodeGen::instGen_Set_Reg_To_Imm(emitAttr size, regNumber reg, ssize_t imm, insFlags flags)
{
    // reg cannot be a FP register
    assert(!genIsValidFloatReg(reg));

    if (!compiler->opts.compReloc)
    {
        size = EA_SIZE(size); // Strip any Reloc flags from size if we aren't doing relocs
    }

    if (EA_IS_RELOC(size))
    {
        getEmitter()->emitIns_R_I(INS_movw, size, reg, imm);
        getEmitter()->emitIns_R_I(INS_movt, size, reg, imm);
    }
    else if (imm == 0)
    {
        instGen_Set_Reg_To_Zero(size, reg, flags);
    }
    else
    {
        if (arm_Valid_Imm_For_Mov(imm))
        {
            getEmitter()->emitIns_R_I(INS_mov, size, reg, imm, flags);
        }
        else // We have to use a movw/movt pair of instructions
        {
            ssize_t imm_lo16 = (imm & 0xffff);
            ssize_t imm_hi16 = (imm >> 16) & 0xffff;

            assert(arm_Valid_Imm_For_Mov(imm_lo16));
            assert(imm_hi16 != 0);

            getEmitter()->emitIns_R_I(INS_movw, size, reg, imm_lo16);

            // If we've got a low register, the high word is all bits set,
            // and the high bit of the low word is set, we can sign extend
            // halfword and save two bytes of encoding. This can happen for
            // small magnitude negative numbers 'n' for -32768 <= n <= -1.

            if (getEmitter()->isLowRegister(reg) && (imm_hi16 == 0xffff) && ((imm_lo16 & 0x8000) == 0x8000))
            {
                getEmitter()->emitIns_R_R(INS_sxth, EA_2BYTE, reg, reg);
            }
            else
            {
                getEmitter()->emitIns_R_I(INS_movt, size, reg, imm_hi16);
            }

            if (flags == INS_FLAGS_SET)
                getEmitter()->emitIns_R_R(INS_mov, size, reg, reg, INS_FLAGS_SET);
        }
    }

    regTracker.rsTrackRegIntCns(reg, imm);
}

//------------------------------------------------------------------------
// genSetRegToConst: Generate code to set a register 'targetReg' of type 'targetType'
//    to the constant specified by the constant (GT_CNS_INT or GT_CNS_DBL) in 'tree'.
//
// Notes:
//    This does not call genProduceReg() on the target register.
//
void CodeGen::genSetRegToConst(regNumber targetReg, var_types targetType, GenTreePtr tree)
{
    switch (tree->gtOper)
    {
        case GT_CNS_INT:
        {
            // relocatable values tend to come down as a CNS_INT of native int type
            // so the line between these two opcodes is kind of blurry
            GenTreeIntConCommon* con    = tree->AsIntConCommon();
            ssize_t              cnsVal = con->IconValue();

            bool needReloc = compiler->opts.compReloc && tree->IsIconHandle();
            if (needReloc)
            {
                instGen_Set_Reg_To_Imm(EA_HANDLE_CNS_RELOC, targetReg, cnsVal);
                regTracker.rsTrackRegTrash(targetReg);
            }
            else
            {
                genSetRegToIcon(targetReg, cnsVal, targetType);
            }
        }
        break;

        case GT_CNS_DBL:
        {
            GenTreeDblCon* dblConst   = tree->AsDblCon();
            double         constValue = dblConst->gtDblCon.gtDconVal;
            // TODO-ARM-CQ: Do we have a faster/smaller way to generate 0.0 in thumb2 ISA ?
            if (targetType == TYP_FLOAT)
            {
                // Get a temp integer register
                regNumber tmpReg = tree->GetSingleTempReg();

                float f = forceCastToFloat(constValue);
                genSetRegToIcon(tmpReg, *((int*)(&f)));
                getEmitter()->emitIns_R_R(INS_vmov_i2f, EA_4BYTE, targetReg, tmpReg);
            }
            else
            {
                assert(targetType == TYP_DOUBLE);

                unsigned* cv = (unsigned*)&constValue;

                // Get two temp integer registers
                regNumber tmpReg1 = tree->ExtractTempReg();
                regNumber tmpReg2 = tree->GetSingleTempReg();

                genSetRegToIcon(tmpReg1, cv[0]);
                genSetRegToIcon(tmpReg2, cv[1]);

                getEmitter()->emitIns_R_R_R(INS_vmov_i2d, EA_8BYTE, targetReg, tmpReg1, tmpReg2);
            }
        }
        break;

        default:
            unreached();
    }
}

//------------------------------------------------------------------------
// genCodeForBinary: Generate code for many binary arithmetic operators
// This method is expected to have called genConsumeOperands() before calling it.
//
// Arguments:
//    treeNode - The binary operation for which we are generating code.
//
// Return Value:
//    None.
//
// Notes:
//    Mul and div are not handled here.
//    See the assert below for the operators that are handled.

void CodeGen::genCodeForBinary(GenTree* treeNode)
{
    const genTreeOps oper       = treeNode->OperGet();
    regNumber        targetReg  = treeNode->gtRegNum;
    var_types        targetType = treeNode->TypeGet();
    emitter*         emit       = getEmitter();

    assert(oper == GT_ADD || oper == GT_SUB || oper == GT_MUL || oper == GT_ADD_LO || oper == GT_ADD_HI ||
           oper == GT_SUB_LO || oper == GT_SUB_HI || oper == GT_OR || oper == GT_XOR || oper == GT_AND);

    GenTreePtr op1 = treeNode->gtGetOp1();
    GenTreePtr op2 = treeNode->gtGetOp2();

    instruction ins = genGetInsForOper(oper, targetType);

    // The arithmetic node must be sitting in a register (since it's not contained)
    noway_assert(targetReg != REG_NA);

    if ((oper == GT_ADD_LO || oper == GT_SUB_LO))
    {
        // During decomposition, all operands become reg
        assert(!op1->isContained() && !op2->isContained());
        emit->emitIns_R_R_R(ins, emitTypeSize(treeNode), treeNode->gtRegNum, op1->gtRegNum, op2->gtRegNum,
                            INS_FLAGS_SET);
    }
    else
    {
        regNumber r = emit->emitInsTernary(ins, emitTypeSize(treeNode), treeNode, op1, op2);
        assert(r == targetReg);
    }

    genProduceReg(treeNode);
}

//------------------------------------------------------------------------
// genReturn: Generates code for return statement.
//            In case of struct return, delegates to the genStructReturn method.
//
// Arguments:
//    treeNode - The GT_RETURN or GT_RETFILT tree node.
//
// Return Value:
//    None
//
void CodeGen::genReturn(GenTreePtr treeNode)
{
    assert(treeNode->OperGet() == GT_RETURN || treeNode->OperGet() == GT_RETFILT);
    GenTreePtr op1        = treeNode->gtGetOp1();
    var_types  targetType = treeNode->TypeGet();

    // A void GT_RETFILT is the end of a finally. For non-void filter returns we need to load the result in the return
    // register, if it's not already there. The processing is the same as GT_RETURN. For filters, the IL spec says the
    // result is type int32. Further, the only legal values are 0 or 1; the use of other values is "undefined".
    assert(!treeNode->OperIs(GT_RETFILT) || (targetType == TYP_VOID) || (targetType == TYP_INT));

#ifdef DEBUG
    if (targetType == TYP_VOID)
    {
        assert(op1 == nullptr);
    }
#endif

    if (treeNode->TypeGet() == TYP_LONG)
    {
        assert(op1 != nullptr);
        noway_assert(op1->OperGet() == GT_LONG);
        GenTree* loRetVal = op1->gtGetOp1();
        GenTree* hiRetVal = op1->gtGetOp2();
        noway_assert((loRetVal->gtRegNum != REG_NA) && (hiRetVal->gtRegNum != REG_NA));

        genConsumeReg(loRetVal);
        genConsumeReg(hiRetVal);
        if (loRetVal->gtRegNum != REG_LNGRET_LO)
        {
            inst_RV_RV(ins_Copy(targetType), REG_LNGRET_LO, loRetVal->gtRegNum, TYP_INT);
        }
        if (hiRetVal->gtRegNum != REG_LNGRET_HI)
        {
            inst_RV_RV(ins_Copy(targetType), REG_LNGRET_HI, hiRetVal->gtRegNum, TYP_INT);
        }
    }
    else
    {
        if (varTypeIsStruct(treeNode))
        {
            NYI_ARM("struct return");
        }
        else if (targetType != TYP_VOID)
        {
            assert(op1 != nullptr);
            noway_assert(op1->gtRegNum != REG_NA);

            // !! NOTE !! genConsumeReg will clear op1 as GC ref after it has
            // consumed a reg for the operand. This is because the variable
            // is dead after return. But we are issuing more instructions
            // like "profiler leave callback" after this consumption. So
            // if you are issuing more instructions after this point,
            // remember to keep the variable live up until the new method
            // exit point where it is actually dead.
            genConsumeReg(op1);

            regNumber retReg = varTypeIsFloating(treeNode) ? REG_FLOATRET : REG_INTRET;
            if (op1->gtRegNum != retReg)
            {
                inst_RV_RV(ins_Move_Extend(targetType, true), retReg, op1->gtRegNum, targetType);
            }
        }
    }
}

//------------------------------------------------------------------------
// genLockedInstructions: Generate code for the locked operations.
//
// Notes:
//    Handles GT_LOCKADD, GT_XCHG, GT_XADD nodes.
//
void CodeGen::genLockedInstructions(GenTreeOp* treeNode)
{
    NYI("genLockedInstructions");
}

//--------------------------------------------------------------------------------------
// genLclHeap: Generate code for localloc
//
// Description:
//      There are 2 ways depending from build version to generate code for localloc:
//          1) For debug build where memory should be initialized we generate loop
//             which invoke push {tmpReg} N times.
//          2) Fore /o build  However, we tickle the pages to ensure that SP is always
//             valid and is in sync with the "stack guard page". Amount of iteration
//             is N/PAGE_SIZE.
//
// Comments:
//      There can be some optimization:
//          1) It's not needed to generate loop for zero size allocation
//          2) For small allocation (less than 4 store) we unroll loop
//          3) For allocation less than PAGE_SIZE and when it's not needed to initialize
//             memory to zero, we can just increment SP.
//
// Notes: Size N should be aligned to STACK_ALIGN before any allocation
//
void CodeGen::genLclHeap(GenTreePtr tree)
{
    assert(tree->OperGet() == GT_LCLHEAP);

    GenTreePtr size = tree->gtOp.gtOp1;
    noway_assert((genActualType(size->gtType) == TYP_INT) || (genActualType(size->gtType) == TYP_I_IMPL));

    // Result of localloc will be returned in regCnt.
    // Also it used as temporary register in code generation
    // for storing allocation size
    regNumber   regCnt          = tree->gtRegNum;
    regNumber   pspSymReg       = REG_NA;
    var_types   type            = genActualType(size->gtType);
    emitAttr    easz            = emitTypeSize(type);
    BasicBlock* endLabel        = nullptr;
    BasicBlock* loop            = nullptr;
    unsigned    stackAdjustment = 0;

#ifdef DEBUG
    // Verify ESP
    if (compiler->opts.compStackCheckOnRet)
    {
        noway_assert(compiler->lvaReturnEspCheck != 0xCCCCCCCC &&
                     compiler->lvaTable[compiler->lvaReturnEspCheck].lvDoNotEnregister &&
                     compiler->lvaTable[compiler->lvaReturnEspCheck].lvOnFrame);
        getEmitter()->emitIns_S_R(INS_cmp, EA_PTRSIZE, REG_SPBASE, compiler->lvaReturnEspCheck, 0);

        BasicBlock*  esp_check = genCreateTempLabel();
        emitJumpKind jmpEqual  = genJumpKindForOper(GT_EQ, CK_SIGNED);
        inst_JMP(jmpEqual, esp_check);
        getEmitter()->emitIns(INS_BREAKPOINT);
        genDefineTempLabel(esp_check);
    }
#endif

    noway_assert(isFramePointerUsed()); // localloc requires Frame Pointer to be established since SP changes
    noway_assert(genStackLevel == 0);   // Can't have anything on the stack

    // Whether method has PSPSym.
    bool hasPspSym;
#if FEATURE_EH_FUNCLETS
    hasPspSym = (compiler->lvaPSPSym != BAD_VAR_NUM);
#else
    hasPspSym = false;
#endif

    // Check to 0 size allocations
    // size_t amount = 0;
    if (size->IsCnsIntOrI())
    {
        // If size is a constant, then it must be contained.
        assert(size->isContained());

        // If amount is zero then return null in regCnt
        size_t amount = size->gtIntCon.gtIconVal;
        if (amount == 0)
        {
            instGen_Set_Reg_To_Zero(EA_PTRSIZE, regCnt);
            goto BAILOUT;
        }
    }
    else
    {
        // If 0 bail out by returning null in regCnt
        genConsumeRegAndCopy(size, regCnt);
        endLabel = genCreateTempLabel();
        getEmitter()->emitIns_R_R(INS_TEST, easz, regCnt, regCnt);
        emitJumpKind jmpEqual = genJumpKindForOper(GT_EQ, CK_SIGNED);
        inst_JMP(jmpEqual, endLabel);
    }

    stackAdjustment = 0;
#if FEATURE_EH_FUNCLETS
    // If we have PSPsym, then need to re-locate it after localloc.
    if (hasPspSym)
    {
        stackAdjustment += STACK_ALIGN;

        // Save a copy of PSPSym
        pspSymReg = tree->ExtractTempReg();
        getEmitter()->emitIns_R_S(ins_Load(TYP_I_IMPL), EA_PTRSIZE, pspSymReg, compiler->lvaPSPSym, 0);
    }
#endif

#if FEATURE_FIXED_OUT_ARGS
    // If we have an outgoing arg area then we must adjust the SP by popping off the
    // outgoing arg area. We will restore it right before we return from this method.
    if (compiler->lvaOutgoingArgSpaceSize > 0)
    {
        assert((compiler->lvaOutgoingArgSpaceSize % STACK_ALIGN) == 0); // This must be true for the stack to remain
                                                                        // aligned
        inst_RV_IV(INS_add, REG_SPBASE, compiler->lvaOutgoingArgSpaceSize, EA_PTRSIZE);
        stackAdjustment += compiler->lvaOutgoingArgSpaceSize;
    }
#endif

    // Put aligned allocation size to regCnt
    if (size->IsCnsIntOrI())
    {
        // 'amount' is the total number of bytes to localloc to properly STACK_ALIGN
        size_t amount = size->gtIntCon.gtIconVal;
        amount        = AlignUp(amount, STACK_ALIGN);

        // For small allocations we will generate up to four stp instructions
        size_t cntStackAlignedWidthItems = (amount >> STACK_ALIGN_SHIFT);
        if (cntStackAlignedWidthItems <= 4)
        {
            instGen_Set_Reg_To_Zero(EA_PTRSIZE, regCnt);

            while (cntStackAlignedWidthItems != 0)
            {
                inst_IV(INS_push, (unsigned)genRegMask(regCnt));
                cntStackAlignedWidthItems -= 1;
            }

            goto ALLOC_DONE;
        }
        else if (!compiler->info.compInitMem && (amount < compiler->eeGetPageSize())) // must be < not <=
        {
            // Since the size is a page or less, simply adjust the SP value
            // The SP might already be in the guard page, must touch it BEFORE
            // the alloc, not after.
            getEmitter()->emitIns_R_R_I(INS_ldr, EA_4BYTE, regCnt, REG_SP, 0);
            inst_RV_IV(INS_sub, REG_SP, amount, EA_PTRSIZE);
            goto ALLOC_DONE;
        }

        // regCnt will be the total number of bytes to locAlloc
        genSetRegToIcon(regCnt, amount, ((int)amount == amount) ? TYP_INT : TYP_LONG);
    }
    else
    {
        // Round up the number of bytes to allocate to a STACK_ALIGN boundary.
        inst_RV_IV(INS_add, regCnt, (STACK_ALIGN - 1), emitActualTypeSize(type));
        inst_RV_IV(INS_AND, regCnt, ~(STACK_ALIGN - 1), emitActualTypeSize(type));
    }

    // Allocation
    if (compiler->info.compInitMem)
    {
        // At this point 'regCnt' is set to the total number of bytes to locAlloc.
        // Since we have to zero out the allocated memory AND ensure that RSP is always valid
        // by tickling the pages, we will just push 0's on the stack.

        regNumber regTmp = tree->ExtractTempReg();
        instGen_Set_Reg_To_Zero(EA_PTRSIZE, regTmp);

        // Loop:
        BasicBlock* loop = genCreateTempLabel();
        genDefineTempLabel(loop);

        noway_assert(STACK_ALIGN == 8);
        inst_IV(INS_push, (unsigned)genRegMask(regTmp));
        inst_IV(INS_push, (unsigned)genRegMask(regTmp));

        // If not done, loop
        // Note that regCnt is the number of bytes to stack allocate.
        assert(genIsValidIntReg(regCnt));
        getEmitter()->emitIns_R_I(INS_sub, EA_PTRSIZE, regCnt, STACK_ALIGN, INS_FLAGS_SET);
        emitJumpKind jmpNotEqual = genJumpKindForOper(GT_NE, CK_SIGNED);
        inst_JMP(jmpNotEqual, loop);
    }
    else
    {
        // At this point 'regCnt' is set to the total number of bytes to locAlloc.
        //
        // We don't need to zero out the allocated memory. However, we do have
        // to tickle the pages to ensure that SP is always valid and is
        // in sync with the "stack guard page".  Note that in the worst
        // case SP is on the last byte of the guard page.  Thus you must
        // touch SP+0 first not SP+0x1000.
        //
        // Another subtlety is that you don't want SP to be exactly on the
        // boundary of the guard page because PUSH is predecrement, thus
        // call setup would not touch the guard page but just beyond it
        //
        // Note that we go through a few hoops so that SP never points to
        // illegal pages at any time during the ticking process
        //
        //       subs  regCnt, SP, regCnt      // regCnt now holds ultimate SP
        //       jb    Loop                    // result is smaller than orignial SP (no wrap around)
        //       mov   regCnt, #0              // Overflow, pick lowest possible value
        //
        //  Loop:
        //       ldr   regTmp, [SP + 0]        // tickle the page - read from the page
        //       sub   regTmp, SP, PAGE_SIZE   // decrement SP by PAGE_SIZE
        //       cmp   regTmp, regCnt
        //       jb    Done
        //       mov   SP, regTmp
        //       j     Loop
        //
        //  Done:
        //       mov   SP, regCnt
        //

        // Setup the regTmp
        regNumber regTmp = tree->ExtractTempReg();

        BasicBlock* loop = genCreateTempLabel();
        BasicBlock* done = genCreateTempLabel();

        //       subs  regCnt, SP, regCnt      // regCnt now holds ultimate SP
        getEmitter()->emitIns_R_R_R(INS_sub, EA_PTRSIZE, regCnt, REG_SPBASE, regCnt, INS_FLAGS_SET);

        inst_JMP(EJ_vc, loop); // branch if the V flag is not set

        // Ups... Overflow, set regCnt to lowest possible value
        instGen_Set_Reg_To_Zero(EA_PTRSIZE, regCnt);

        genDefineTempLabel(loop);

        // tickle the page - Read from the updated SP - this triggers a page fault when on the guard page
        getEmitter()->emitIns_R_R_I(INS_ldr, EA_4BYTE, regTmp, REG_SPBASE, 0);

        // decrement SP by PAGE_SIZE
        getEmitter()->emitIns_R_R_I(INS_sub, EA_PTRSIZE, regTmp, REG_SPBASE, compiler->eeGetPageSize());

        getEmitter()->emitIns_R_R(INS_cmp, EA_PTRSIZE, regTmp, regCnt);
        emitJumpKind jmpLTU = genJumpKindForOper(GT_LT, CK_UNSIGNED);
        inst_JMP(jmpLTU, done);

        // Update SP to be at the next page of stack that we will tickle
        getEmitter()->emitIns_R_R(INS_mov, EA_PTRSIZE, REG_SPBASE, regCnt);

        // Jump to loop and tickle new stack address
        inst_JMP(EJ_jmp, loop);

        // Done with stack tickle loop
        genDefineTempLabel(done);

        // Now just move the final value to SP
        getEmitter()->emitIns_R_R(INS_mov, EA_PTRSIZE, REG_SPBASE, regCnt);
    }

ALLOC_DONE:
    // Re-adjust SP to allocate PSPSym and out-going arg area
    if (stackAdjustment != 0)
    {
        assert((stackAdjustment % STACK_ALIGN) == 0); // This must be true for the stack to remain aligned
        assert(stackAdjustment > 0);
        getEmitter()->emitIns_R_R_I(INS_sub, EA_PTRSIZE, REG_SPBASE, REG_SPBASE, (int)stackAdjustment);

#if FEATURE_EH_FUNCLETS
        // Write PSPSym to its new location.
        if (hasPspSym)
        {
            assert(genIsValidIntReg(pspSymReg));
            getEmitter()->emitIns_S_R(ins_Store(TYP_I_IMPL), EA_PTRSIZE, pspSymReg, compiler->lvaPSPSym, 0);
        }
#endif
        // Return the stackalloc'ed address in result register.
        // regCnt = RSP + stackAdjustment.
        getEmitter()->emitIns_R_R_I(INS_add, EA_PTRSIZE, regCnt, REG_SPBASE, (int)stackAdjustment);
    }
    else // stackAdjustment == 0
    {
        // Move the final value of SP to regCnt
        inst_RV_RV(INS_mov, regCnt, REG_SPBASE);
    }

BAILOUT:
    if (endLabel != nullptr)
        genDefineTempLabel(endLabel);

    // Write the lvaLocAllocSPvar stack frame slot
    if (compiler->lvaLocAllocSPvar != BAD_VAR_NUM)
    {
        getEmitter()->emitIns_S_R(ins_Store(TYP_I_IMPL), EA_PTRSIZE, regCnt, compiler->lvaLocAllocSPvar, 0);
    }

#if STACK_PROBES
    if (compiler->opts.compNeedStackProbes)
    {
        genGenerateStackProbe();
    }
#endif

#ifdef DEBUG
    // Update new ESP
    if (compiler->opts.compStackCheckOnRet)
    {
        noway_assert(compiler->lvaReturnEspCheck != 0xCCCCCCCC &&
                     compiler->lvaTable[compiler->lvaReturnEspCheck].lvDoNotEnregister &&
                     compiler->lvaTable[compiler->lvaReturnEspCheck].lvOnFrame);
        getEmitter()->emitIns_S_R(ins_Store(TYP_I_IMPL), EA_PTRSIZE, regCnt, compiler->lvaReturnEspCheck, 0);
    }
#endif

    genProduceReg(tree);
}

//------------------------------------------------------------------------
// genTableBasedSwitch: generate code for a switch statement based on a table of ip-relative offsets
//
void CodeGen::genTableBasedSwitch(GenTree* treeNode)
{
    genConsumeOperands(treeNode->AsOp());
    regNumber idxReg  = treeNode->gtOp.gtOp1->gtRegNum;
    regNumber baseReg = treeNode->gtOp.gtOp2->gtRegNum;

    getEmitter()->emitIns_R_ARX(INS_ldr, EA_4BYTE, REG_PC, baseReg, idxReg, TARGET_POINTER_SIZE, 0);
}

//------------------------------------------------------------------------
// genJumpTable: emits the table and an instruction to get the address of the first element
//
void CodeGen::genJumpTable(GenTree* treeNode)
{
    noway_assert(compiler->compCurBB->bbJumpKind == BBJ_SWITCH);
    assert(treeNode->OperGet() == GT_JMPTABLE);

    unsigned     jumpCount = compiler->compCurBB->bbJumpSwt->bbsCount;
    BasicBlock** jumpTable = compiler->compCurBB->bbJumpSwt->bbsDstTab;
    unsigned     jmpTabBase;

    jmpTabBase = getEmitter()->emitBBTableDataGenBeg(jumpCount, false);

    JITDUMP("\n      J_M%03u_DS%02u LABEL   DWORD\n", Compiler::s_compMethodsCount, jmpTabBase);

    for (unsigned i = 0; i < jumpCount; i++)
    {
        BasicBlock* target = *jumpTable++;
        noway_assert(target->bbFlags & BBF_JMP_TARGET);

        JITDUMP("            DD      L_M%03u_BB%02u\n", Compiler::s_compMethodsCount, target->bbNum);

        getEmitter()->emitDataGenData(i, target);
    }

    getEmitter()->emitDataGenEnd();

    getEmitter()->emitIns_R_D(INS_movw, EA_HANDLE_CNS_RELOC, jmpTabBase, treeNode->gtRegNum);
    getEmitter()->emitIns_R_D(INS_movt, EA_HANDLE_CNS_RELOC, jmpTabBase, treeNode->gtRegNum);

    genProduceReg(treeNode);
}

//------------------------------------------------------------------------
// genGetInsForOper: Return instruction encoding of the operation tree.
//
instruction CodeGen::genGetInsForOper(genTreeOps oper, var_types type)
{
    instruction ins;

    if (varTypeIsFloating(type))
        return CodeGen::ins_MathOp(oper, type);

    switch (oper)
    {
        case GT_ADD:
            ins = INS_add;
            break;
        case GT_AND:
            ins = INS_AND;
            break;
        case GT_MUL:
            ins = INS_MUL;
            break;
        case GT_DIV:
            ins = INS_sdiv;
            break;
        case GT_LSH:
            ins = INS_SHIFT_LEFT_LOGICAL;
            break;
        case GT_NEG:
            ins = INS_rsb;
            break;
        case GT_NOT:
            ins = INS_NOT;
            break;
        case GT_OR:
            ins = INS_OR;
            break;
        case GT_RSH:
            ins = INS_SHIFT_RIGHT_ARITHM;
            break;
        case GT_RSZ:
            ins = INS_SHIFT_RIGHT_LOGICAL;
            break;
        case GT_SUB:
            ins = INS_sub;
            break;
        case GT_XOR:
            ins = INS_XOR;
            break;
        case GT_ROR:
            ins = INS_ror;
            break;
        case GT_ADD_LO:
            ins = INS_add;
            break;
        case GT_ADD_HI:
            ins = INS_adc;
            break;
        case GT_SUB_LO:
            ins = INS_sub;
            break;
        case GT_SUB_HI:
            ins = INS_sbc;
            break;
        case GT_LSH_HI:
            ins = INS_SHIFT_LEFT_LOGICAL;
            break;
        case GT_RSH_LO:
            ins = INS_SHIFT_RIGHT_LOGICAL;
            break;
        default:
            unreached();
            break;
    }
    return ins;
}

// Generates CpBlk code by performing a loop unroll
// Preconditions:
//  The size argument of the CpBlk node is a constant and <= 64 bytes.
//  This may seem small but covers >95% of the cases in several framework assemblies.
void CodeGen::genCodeForCpBlkUnroll(GenTreeBlk* cpBlkNode)
{
    NYI_ARM("genCodeForCpBlkUnroll");
}

// Generate code for InitBlk by performing a loop unroll
// Preconditions:
//   a) Both the size and fill byte value are integer constants.
//   b) The size of the struct to initialize is smaller than INITBLK_UNROLL_LIMIT bytes.
void CodeGen::genCodeForInitBlkUnroll(GenTreeBlk* initBlkNode)
{
    NYI_ARM("genCodeForInitBlkUnroll");
}

//------------------------------------------------------------------------
// genCodeForNegNot: Produce code for a GT_NEG/GT_NOT node.
//
// Arguments:
//    tree - the node
//
void CodeGen::genCodeForNegNot(GenTree* tree)
{
    assert(tree->OperIs(GT_NEG, GT_NOT));

    var_types targetType = tree->TypeGet();

    assert(!tree->OperIs(GT_NOT) || !varTypeIsFloating(targetType));

    regNumber   targetReg = tree->gtRegNum;
    instruction ins       = genGetInsForOper(tree->OperGet(), targetType);

    // The arithmetic node must be sitting in a register (since it's not contained)
    assert(!tree->isContained());
    // The dst can only be a register.
    assert(targetReg != REG_NA);

    GenTreePtr operand = tree->gtGetOp1();
    assert(!operand->isContained());
    // The src must be a register.
    regNumber operandReg = genConsumeReg(operand);

    if (ins == INS_vneg)
    {
        getEmitter()->emitIns_R_R(ins, emitTypeSize(tree), targetReg, operandReg);
    }
    else
    {
        getEmitter()->emitIns_R_R_I(ins, emitTypeSize(tree), targetReg, operandReg, 0);
    }

    genProduceReg(tree);
}

// Generate code for CpObj nodes wich copy structs that have interleaved
// GC pointers.
// For this case we'll generate a sequence of loads/stores in the case of struct
// slots that don't contain GC pointers.  The generated code will look like:
// ldr tempReg, [R13, #8]
// str tempReg, [R14, #8]
//
// In the case of a GC-Pointer we'll call the ByRef write barrier helper
// who happens to use the same registers as the previous call to maintain
// the same register requirements and register killsets:
// bl CORINFO_HELP_ASSIGN_BYREF
//
// So finally an example would look like this:
// ldr tempReg, [R13, #8]
// str tempReg, [R14, #8]
// bl CORINFO_HELP_ASSIGN_BYREF
// ldr tempReg, [R13, #8]
// str tempReg, [R14, #8]
// bl CORINFO_HELP_ASSIGN_BYREF
// ldr tempReg, [R13, #8]
// str tempReg, [R14, #8]
void CodeGen::genCodeForCpObj(GenTreeObj* cpObjNode)
{
    GenTreePtr dstAddr       = cpObjNode->Addr();
    GenTreePtr source        = cpObjNode->Data();
    var_types  srcAddrType   = TYP_BYREF;
    bool       sourceIsLocal = false;
    regNumber  dstReg        = REG_NA;
    regNumber  srcReg        = REG_NA;

    assert(source->isContained());
    if (source->gtOper == GT_IND)
    {
        GenTree* srcAddr = source->gtGetOp1();
        assert(!srcAddr->isContained());
        srcAddrType = srcAddr->TypeGet();
    }
    else
    {
        noway_assert(source->IsLocal());
        sourceIsLocal = true;
    }

    bool dstOnStack = dstAddr->OperIsLocalAddr();

#ifdef DEBUG
    assert(!dstAddr->isContained());

    // This GenTree node has data about GC pointers, this means we're dealing
    // with CpObj.
    assert(cpObjNode->gtGcPtrCount > 0);
#endif // DEBUG

    // Consume the operands and get them into the right registers.
    // They may now contain gc pointers (depending on their type; gcMarkRegPtrVal will "do the right thing").
    genConsumeBlockOp(cpObjNode, REG_WRITE_BARRIER_DST_BYREF, REG_WRITE_BARRIER_SRC_BYREF, REG_NA);
    gcInfo.gcMarkRegPtrVal(REG_WRITE_BARRIER_SRC_BYREF, srcAddrType);
    gcInfo.gcMarkRegPtrVal(REG_WRITE_BARRIER_DST_BYREF, dstAddr->TypeGet());

    // Temp register used to perform the sequence of loads and stores.
    regNumber tmpReg = cpObjNode->ExtractTempReg();
    assert(genIsValidIntReg(tmpReg));

    unsigned slots = cpObjNode->gtSlots;
    emitter* emit  = getEmitter();

    BYTE* gcPtrs = cpObjNode->gtGcPtrs;

    // If we can prove it's on the stack we don't need to use the write barrier.
    emitAttr attr = EA_PTRSIZE;
    if (dstOnStack)
    {
        for (unsigned i = 0; i < slots; ++i)
        {
            if (gcPtrs[i] == GCT_GCREF)
                attr = EA_GCREF;
            else if (gcPtrs[i] == GCT_BYREF)
                attr = EA_BYREF;
            emit->emitIns_R_R_I(INS_ldr, attr, tmpReg, REG_WRITE_BARRIER_SRC_BYREF, TARGET_POINTER_SIZE,
                                INS_FLAGS_DONT_CARE, INS_OPTS_LDST_POST_INC);
            emit->emitIns_R_R_I(INS_str, attr, tmpReg, REG_WRITE_BARRIER_DST_BYREF, TARGET_POINTER_SIZE,
                                INS_FLAGS_DONT_CARE, INS_OPTS_LDST_POST_INC);
        }
    }
    else
    {
        unsigned gcPtrCount = cpObjNode->gtGcPtrCount;

        unsigned i = 0;
        while (i < slots)
        {
            switch (gcPtrs[i])
            {
                case TYPE_GC_NONE:
                    emit->emitIns_R_R_I(INS_ldr, attr, tmpReg, REG_WRITE_BARRIER_SRC_BYREF, TARGET_POINTER_SIZE,
                                        INS_FLAGS_DONT_CARE, INS_OPTS_LDST_POST_INC);
                    emit->emitIns_R_R_I(INS_str, attr, tmpReg, REG_WRITE_BARRIER_DST_BYREF, TARGET_POINTER_SIZE,
                                        INS_FLAGS_DONT_CARE, INS_OPTS_LDST_POST_INC);
                    break;

                default:
                    // In the case of a GC-Pointer we'll call the ByRef write barrier helper
                    genEmitHelperCall(CORINFO_HELP_ASSIGN_BYREF, 0, EA_PTRSIZE);

                    gcPtrCount--;
                    break;
            }
            ++i;
        }
        assert(gcPtrCount == 0);
    }

    // Clear the gcInfo for registers of source and dest.
    // While we normally update GC info prior to the last instruction that uses them,
    // these actually live into the helper call.
    gcInfo.gcMarkRegSetNpt(RBM_WRITE_BARRIER_SRC_BYREF | RBM_WRITE_BARRIER_DST_BYREF);
}

//------------------------------------------------------------------------
// genCodeForShiftLong: Generates the code sequence for a GenTree node that
// represents a three operand bit shift or rotate operation (<<Hi, >>Lo).
//
// Arguments:
//    tree - the bit shift node (that specifies the type of bit shift to perform).
//
// Assumptions:
//    a) All GenTrees are register allocated.
//    b) The shift-by-amount in tree->gtOp.gtOp2 is a contained constant
//
void CodeGen::genCodeForShiftLong(GenTreePtr tree)
{
    // Only the non-RMW case here.
    genTreeOps oper = tree->OperGet();
    assert(oper == GT_LSH_HI || oper == GT_RSH_LO);

    GenTree* operand = tree->gtOp.gtOp1;
    assert(operand->OperGet() == GT_LONG);
    assert(operand->gtOp.gtOp1->isUsedFromReg());
    assert(operand->gtOp.gtOp2->isUsedFromReg());

    GenTree* operandLo = operand->gtGetOp1();
    GenTree* operandHi = operand->gtGetOp2();

    regNumber regLo = operandLo->gtRegNum;
    regNumber regHi = operandHi->gtRegNum;

    genConsumeOperands(tree->AsOp());

    var_types   targetType = tree->TypeGet();
    instruction ins        = genGetInsForOper(oper, targetType);

    GenTreePtr shiftBy = tree->gtGetOp2();

    assert(shiftBy->isContainedIntOrIImmed());

    unsigned int count = shiftBy->AsIntConCommon()->IconValue();

    regNumber regResult = (oper == GT_LSH_HI) ? regHi : regLo;

    if (regResult != tree->gtRegNum)
    {
        inst_RV_RV(INS_mov, tree->gtRegNum, regResult, targetType);
    }

    if (oper == GT_LSH_HI)
    {
        inst_RV_SH(ins, EA_4BYTE, tree->gtRegNum, count);
        getEmitter()->emitIns_R_R_R_I(INS_OR, EA_4BYTE, tree->gtRegNum, tree->gtRegNum, regLo, 32 - count,
                                      INS_FLAGS_DONT_CARE, INS_OPTS_LSR);
    }
    else
    {
        assert(oper == GT_RSH_LO);
        inst_RV_SH(INS_SHIFT_RIGHT_LOGICAL, EA_4BYTE, tree->gtRegNum, count);
        getEmitter()->emitIns_R_R_R_I(INS_OR, EA_4BYTE, tree->gtRegNum, tree->gtRegNum, regHi, 32 - count,
                                      INS_FLAGS_DONT_CARE, INS_OPTS_LSL);
    }

    genProduceReg(tree);
}

//------------------------------------------------------------------------
// genCodeForLclVar: Produce code for a GT_LCL_VAR node.
//
// Arguments:
//    tree - the GT_LCL_VAR node
//
void CodeGen::genCodeForLclVar(GenTreeLclVar* tree)
{
    // lcl_vars are not defs
    assert((tree->gtFlags & GTF_VAR_DEF) == 0);

    bool isRegCandidate = compiler->lvaTable[tree->gtLclNum].lvIsRegCandidate();

    if (isRegCandidate && !(tree->gtFlags & GTF_VAR_DEATH))
    {
        assert((tree->InReg()) || (tree->gtFlags & GTF_SPILLED));
    }

    // If this is a register candidate that has been spilled, genConsumeReg() will
    // reload it at the point of use.  Otherwise, if it's not in a register, we load it here.

    if (!tree->InReg() && !(tree->gtFlags & GTF_SPILLED))
    {
        assert(!isRegCandidate);
        getEmitter()->emitIns_R_S(ins_Load(tree->TypeGet()), emitTypeSize(tree), tree->gtRegNum, tree->gtLclNum, 0);
        genProduceReg(tree);
    }
}

//------------------------------------------------------------------------
// genCodeForStoreLclFld: Produce code for a GT_STORE_LCL_FLD node.
//
// Arguments:
//    tree - the GT_STORE_LCL_FLD node
//
void CodeGen::genCodeForStoreLclFld(GenTreeLclFld* tree)
{
    var_types targetType = tree->TypeGet();
    regNumber targetReg  = tree->gtRegNum;
    emitter*  emit       = getEmitter();

    noway_assert(targetType != TYP_STRUCT);

    // record the offset
    unsigned offset = tree->gtLclOffs;

    // We must have a stack store with GT_STORE_LCL_FLD
    noway_assert(!tree->InReg());
    noway_assert(targetReg == REG_NA);

    unsigned varNum = tree->gtLclNum;
    assert(varNum < compiler->lvaCount);
    LclVarDsc* varDsc = &(compiler->lvaTable[varNum]);

    // Ensure that lclVar nodes are typed correctly.
    assert(!varDsc->lvNormalizeOnStore() || targetType == genActualType(varDsc->TypeGet()));

    GenTreePtr  data = tree->gtOp1->gtEffectiveVal();
    instruction ins  = ins_Store(targetType);
    emitAttr    attr = emitTypeSize(targetType);
    if (data->isContainedIntOrIImmed())
    {
        assert(data->IsIntegralConst(0));
        NYI_ARM("st.lclFld contained operand");
    }
    else
    {
        assert(!data->isContained());
        genConsumeReg(data);
        emit->emitIns_S_R(ins, attr, data->gtRegNum, varNum, offset);
    }

    genUpdateLife(tree);
    varDsc->lvRegNum = REG_STK;
}

//------------------------------------------------------------------------
// genCodeForStoreLclVar: Produce code for a GT_STORE_LCL_VAR node.
//
// Arguments:
//    tree - the GT_STORE_LCL_VAR node
//
void CodeGen::genCodeForStoreLclVar(GenTreeLclVar* tree)
{
    var_types targetType = tree->TypeGet();
    regNumber targetReg  = tree->gtRegNum;
    emitter*  emit       = getEmitter();

    unsigned varNum = tree->gtLclNum;
    assert(varNum < compiler->lvaCount);
    LclVarDsc* varDsc = &(compiler->lvaTable[varNum]);

    // Ensure that lclVar nodes are typed correctly.
    assert(!varDsc->lvNormalizeOnStore() || targetType == genActualType(varDsc->TypeGet()));

    GenTreePtr data = tree->gtOp1->gtEffectiveVal();

    // var = call, where call returns a multi-reg return value
    // case is handled separately.
    if (data->gtSkipReloadOrCopy()->IsMultiRegCall())
    {
        genMultiRegCallStoreToLocal(tree);
    }
    else if (tree->TypeGet() == TYP_LONG)
    {
        genStoreLongLclVar(tree);
    }
    else
    {
        genConsumeRegs(data);

        regNumber dataReg = REG_NA;
        if (data->isContainedIntOrIImmed())
        {
            assert(data->IsIntegralConst(0));
            NYI_ARM("st.lclVar contained operand");
        }
        else
        {
            assert(!data->isContained());
            dataReg = data->gtRegNum;
        }
        assert(dataReg != REG_NA);

        if (targetReg == REG_NA) // store into stack based LclVar
        {
            inst_set_SV_var(tree);

            instruction ins  = ins_Store(targetType);
            emitAttr    attr = emitTypeSize(targetType);

            emit->emitIns_S_R(ins, attr, dataReg, varNum, /* offset */ 0);

            genUpdateLife(tree);

            varDsc->lvRegNum = REG_STK;
        }
        else // store into register (i.e move into register)
        {
            if (dataReg != targetReg)
            {
                // Assign into targetReg when dataReg (from op1) is not the same register
                inst_RV_RV(ins_Copy(targetType), targetReg, dataReg, targetType);
            }
            genProduceReg(tree);
        }
    }
}

//------------------------------------------------------------------------
// genLeaInstruction: Produce code for a GT_LEA subnode.
//
void CodeGen::genLeaInstruction(GenTreeAddrMode* lea)
{
    emitAttr size = emitTypeSize(lea);
    genConsumeOperands(lea);

    if (lea->Base() && lea->Index())
    {
        regNumber baseReg  = lea->Base()->gtRegNum;
        regNumber indexReg = lea->Index()->gtRegNum;
        getEmitter()->emitIns_R_ARX(INS_lea, size, lea->gtRegNum, baseReg, indexReg, lea->gtScale, lea->gtOffset);
    }
    else if (lea->Base())
    {
        regNumber baseReg = lea->Base()->gtRegNum;
        getEmitter()->emitIns_R_AR(INS_lea, size, lea->gtRegNum, baseReg, lea->gtOffset);
    }
    else if (lea->Index())
    {
        assert(!"Should we see a baseless address computation during CodeGen for ARM32?");
    }

    genProduceReg(lea);
}

//------------------------------------------------------------------------
// genCodeForDivMod: Produce code for a GT_DIV/GT_UDIV/GT_MOD/GT_UMOD node.
//
// Arguments:
//    tree - the node
//
void CodeGen::genCodeForDivMod(GenTreeOp* tree)
{
    assert(tree->OperIs(GT_DIV, GT_UDIV, GT_MOD, GT_UMOD));

    // We shouldn't be seeing GT_MOD on float/double args as it should get morphed into a
    // helper call by front-end. Similarly we shouldn't be seeing GT_UDIV and GT_UMOD
    // on float/double args.
    noway_assert(tree->OperIs(GT_DIV) || !varTypeIsFloating(tree));

    var_types targetType = tree->TypeGet();
    regNumber targetReg  = tree->gtRegNum;
    emitter*  emit       = getEmitter();

    genConsumeOperands(tree);

    noway_assert(targetReg != REG_NA);

    GenTreePtr  dst    = tree;
    GenTreePtr  src1   = tree->gtGetOp1();
    GenTreePtr  src2   = tree->gtGetOp2();
    instruction ins    = genGetInsForOper(tree->OperGet(), targetType);
    emitAttr    attr   = emitTypeSize(tree);
    regNumber   result = REG_NA;

    // dst can only be a reg
    assert(!dst->isContained());

    // src can be only reg
    assert(!src1->isContained() || !src2->isContained());

    if (varTypeIsFloating(targetType))
    {
        // Floating point divide never raises an exception

        emit->emitIns_R_R_R(ins, attr, dst->gtRegNum, src1->gtRegNum, src2->gtRegNum);
    }
    else // an signed integer divide operation
    {
        // TODO-ARM-Bug: handle zero division exception.

        emit->emitIns_R_R_R(ins, attr, dst->gtRegNum, src1->gtRegNum, src2->gtRegNum);
    }

    genProduceReg(tree);
}

//------------------------------------------------------------------------
// genCodeForCompare: Produce code for a GT_EQ/GT_NE/GT_LT/GT_LE/GT_GE/GT_GT node.
//
// Arguments:
//    tree - the node
//
void CodeGen::genCodeForCompare(GenTreeOp* tree)
{
    // TODO-ARM-CQ: Check if we can use the currently set flags.
    // TODO-ARM-CQ: Check for the case where we can simply transfer the carry bit to a register
    //         (signed < or >= where targetReg != REG_NA)

    GenTreePtr op1 = tree->gtOp1->gtEffectiveVal();
    GenTreePtr op2 = tree->gtOp2->gtEffectiveVal();

    if (varTypeIsLong(op1))
    {
#ifdef DEBUG
        // The result of an unlowered long compare on a 32-bit target must either be
        // a) materialized into a register, or
        // b) unused.
        //
        // A long compare that has a result that is used but not materialized into a register should
        // have been handled by Lowering::LowerCompare.

        LIR::Use use;
        assert((tree->gtRegNum != REG_NA) || !LIR::AsRange(compiler->compCurBB).TryGetUse(tree, &use));
#endif
        genCompareLong(tree);
    }
    else
    {
        regNumber targetReg = tree->gtRegNum;
        emitter*  emit      = getEmitter();
        emitAttr  cmpAttr;

        genConsumeIfReg(op1);
        genConsumeIfReg(op2);

        if (varTypeIsFloating(op1))
        {
            assert(op1->TypeGet() == op2->TypeGet());
            instruction ins = INS_vcmp;
            cmpAttr         = emitTypeSize(op1->TypeGet());
            emit->emitInsBinary(ins, cmpAttr, op1, op2);
            // vmrs with register 0xf has special meaning of transferring flags
            emit->emitIns_R(INS_vmrs, EA_4BYTE, REG_R15);
        }
        else
        {
            var_types op1Type = op1->TypeGet();
            var_types op2Type = op2->TypeGet();
            assert(!varTypeIsFloating(op2Type));
            instruction ins = INS_cmp;
            if (op1Type == op2Type)
            {
                cmpAttr = emitTypeSize(op1Type);
            }
            else
            {
                var_types cmpType    = TYP_INT;
                bool      op1Is64Bit = (varTypeIsLong(op1Type) || op1Type == TYP_REF);
                bool      op2Is64Bit = (varTypeIsLong(op2Type) || op2Type == TYP_REF);
                NYI_IF(op1Is64Bit || op2Is64Bit, "Long compare");
                assert(!op1->isUsedFromMemory() || op1Type == op2Type);
                assert(!op2->isUsedFromMemory() || op1Type == op2Type);
                cmpAttr = emitTypeSize(cmpType);
            }
            emit->emitInsBinary(ins, cmpAttr, op1, op2);
        }

        // Are we evaluating this into a register?
        if (targetReg != REG_NA)
        {
            genSetRegToCond(targetReg, tree);
            genProduceReg(tree);
        }
    }
}

//------------------------------------------------------------------------
// genCodeForJcc: Produce code for a GT_JCC node.
//
// Arguments:
//    tree - the node
//
void CodeGen::genCodeForJcc(GenTreeJumpCC* tree)
{
    assert(compiler->compCurBB->bbJumpKind == BBJ_COND);

    CompareKind  compareKind = ((tree->gtFlags & GTF_UNSIGNED) != 0) ? CK_UNSIGNED : CK_SIGNED;
    emitJumpKind jumpKind    = genJumpKindForOper(tree->gtCondition, compareKind);

    inst_JMP(jumpKind, compiler->compCurBB->bbJumpDest);
}

//------------------------------------------------------------------------
// genCodeForReturnTrap: Produce code for a GT_RETURNTRAP node.
//
// Arguments:
//    tree - the GT_RETURNTRAP node
//
void CodeGen::genCodeForReturnTrap(GenTreeOp* tree)
{
    assert(tree->OperGet() == GT_RETURNTRAP);

    // this is nothing but a conditional call to CORINFO_HELP_STOP_FOR_GC
    // based on the contents of 'data'

    GenTree* data = tree->gtOp1->gtEffectiveVal();
    genConsumeIfReg(data);
    GenTreeIntCon cns = intForm(TYP_INT, 0);
    getEmitter()->emitInsBinary(INS_cmp, emitTypeSize(TYP_INT), data, &cns);

    BasicBlock* skipLabel = genCreateTempLabel();

    emitJumpKind jmpEqual = genJumpKindForOper(GT_EQ, CK_SIGNED);
    inst_JMP(jmpEqual, skipLabel);
    // emit the call to the EE-helper that stops for GC (or other reasons)

    genEmitHelperCall(CORINFO_HELP_STOP_FOR_GC, 0, EA_UNKNOWN);
    genDefineTempLabel(skipLabel);
}

//------------------------------------------------------------------------
// genCodeForStoreInd: Produce code for a GT_STOREIND node.
//
// Arguments:
//    tree - the GT_STOREIND node
//
void CodeGen::genCodeForStoreInd(GenTreeStoreInd* tree)
{
    GenTree*  data       = tree->Data();
    GenTree*  addr       = tree->Addr();
    var_types targetType = tree->TypeGet();
    emitter*  emit       = getEmitter();

    assert(!varTypeIsFloating(targetType) || (targetType == data->TypeGet()));

    GCInfo::WriteBarrierForm writeBarrierForm = gcInfo.gcIsWriteBarrierCandidate(tree, data);
    if (writeBarrierForm != GCInfo::WBF_NoBarrier)
    {
        // data and addr must be in registers.
        // Consume both registers so that any copies of interfering
        // registers are taken care of.
        genConsumeOperands(tree);

#if NOGC_WRITE_BARRIERS
        NYI_ARM("NOGC_WRITE_BARRIERS");
#else
        // At this point, we should not have any interference.
        // That is, 'data' must not be in REG_ARG_0,
        //  as that is where 'addr' must go.
        noway_assert(data->gtRegNum != REG_ARG_0);

        // addr goes in REG_ARG_0
        if (addr->gtRegNum != REG_ARG_0)
        {
            inst_RV_RV(INS_mov, REG_ARG_0, addr->gtRegNum, addr->TypeGet());
        }

        // data goes in REG_ARG_1
        if (data->gtRegNum != REG_ARG_1)
        {
            inst_RV_RV(INS_mov, REG_ARG_1, data->gtRegNum, data->TypeGet());
        }
#endif // NOGC_WRITE_BARRIERS

        genGCWriteBarrier(tree, writeBarrierForm);
    }
    else // A normal store, not a WriteBarrier store
    {
        bool reverseOps  = ((tree->gtFlags & GTF_REVERSE_OPS) != 0);
        bool dataIsUnary = false;

        // We must consume the operands in the proper execution order,
        // so that liveness is updated appropriately.
        if (!reverseOps)
        {
            genConsumeAddress(addr);
        }

        if (!data->isContained())
        {
            genConsumeRegs(data);
        }

        if (reverseOps)
        {
            genConsumeAddress(addr);
        }

        emit->emitInsLoadStoreOp(ins_Store(targetType), emitTypeSize(tree), data->gtRegNum, tree);
    }
}

//------------------------------------------------------------------------
// genCompareLong: Generate code for comparing two longs when the result of the compare
// is manifested in a register.
//
// Arguments:
//    treeNode - the compare tree
//
// Return Value:
//    None.
//
// Comments:
// For long compares, we need to compare the high parts of operands first, then the low parts.
// If the high compare is false, we do not need to compare the low parts. For less than and
// greater than, if the high compare is true, we can assume the entire compare is true.
//
void CodeGen::genCompareLong(GenTreePtr treeNode)
{
    assert(treeNode->OperIsCompare());

    GenTreeOp* tree = treeNode->AsOp();
    GenTreePtr op1  = tree->gtOp1;
    GenTreePtr op2  = tree->gtOp2;

    assert(varTypeIsLong(op1->TypeGet()));
    assert(varTypeIsLong(op2->TypeGet()));

    regNumber targetReg = treeNode->gtRegNum;

    genConsumeOperands(tree);

    GenTreePtr loOp1 = op1->gtGetOp1();
    GenTreePtr hiOp1 = op1->gtGetOp2();
    GenTreePtr loOp2 = op2->gtGetOp1();
    GenTreePtr hiOp2 = op2->gtGetOp2();

    // Create compare for the high parts
    instruction ins     = INS_cmp;
    var_types   cmpType = TYP_INT;
    emitAttr    cmpAttr = emitTypeSize(cmpType);

    // Emit the compare instruction
    getEmitter()->emitInsBinary(ins, cmpAttr, hiOp1, hiOp2);

    // If the result is not being materialized in a register, we're done.
    if (targetReg == REG_NA)
    {
        return;
    }

    BasicBlock* labelTrue  = genCreateTempLabel();
    BasicBlock* labelFalse = genCreateTempLabel();
    BasicBlock* labelNext  = genCreateTempLabel();

    genJccLongHi(tree->gtOper, labelTrue, labelFalse, tree->IsUnsigned());
    getEmitter()->emitInsBinary(ins, cmpAttr, loOp1, loOp2);
    genJccLongLo(tree->gtOper, labelTrue, labelFalse);

    genDefineTempLabel(labelFalse);
    getEmitter()->emitIns_R_I(INS_mov, emitActualTypeSize(tree->gtType), tree->gtRegNum, 0);
    getEmitter()->emitIns_J(INS_b, labelNext);

    genDefineTempLabel(labelTrue);
    getEmitter()->emitIns_R_I(INS_mov, emitActualTypeSize(tree->gtType), tree->gtRegNum, 1);

    genDefineTempLabel(labelNext);

    genProduceReg(tree);
}

void CodeGen::genJccLongHi(genTreeOps cmp, BasicBlock* jumpTrue, BasicBlock* jumpFalse, bool isUnsigned)
{
    if (cmp != GT_NE)
    {
        jumpFalse->bbFlags |= BBF_JMP_TARGET | BBF_HAS_LABEL;
    }

    switch (cmp)
    {
        case GT_EQ:
            inst_JMP(EJ_ne, jumpFalse);
            break;

        case GT_NE:
            inst_JMP(EJ_ne, jumpTrue);
            break;

        case GT_LT:
        case GT_LE:
            if (isUnsigned)
            {
                inst_JMP(EJ_hi, jumpFalse);
                inst_JMP(EJ_lo, jumpTrue);
            }
            else
            {
                inst_JMP(EJ_gt, jumpFalse);
                inst_JMP(EJ_lt, jumpTrue);
            }
            break;

        case GT_GE:
        case GT_GT:
            if (isUnsigned)
            {
                inst_JMP(EJ_lo, jumpFalse);
                inst_JMP(EJ_hi, jumpTrue);
            }
            else
            {
                inst_JMP(EJ_lt, jumpFalse);
                inst_JMP(EJ_gt, jumpTrue);
            }
            break;

        default:
            noway_assert(!"expected a comparison operator");
    }
}

void CodeGen::genJccLongLo(genTreeOps cmp, BasicBlock* jumpTrue, BasicBlock* jumpFalse)
{
    switch (cmp)
    {
        case GT_EQ:
            inst_JMP(EJ_eq, jumpTrue);
            break;

        case GT_NE:
            inst_JMP(EJ_ne, jumpTrue);
            break;

        case GT_LT:
            inst_JMP(EJ_lo, jumpTrue);
            break;

        case GT_LE:
            inst_JMP(EJ_ls, jumpTrue);
            break;

        case GT_GE:
            inst_JMP(EJ_hs, jumpTrue);
            break;

        case GT_GT:
            inst_JMP(EJ_hi, jumpTrue);
            break;

        default:
            noway_assert(!"expected comparison");
    }
}

//------------------------------------------------------------------------
// genSetRegToCond: Generate code to materialize a condition into a register.
//
// Arguments:
//   dstReg - The target register to set to 1 or 0
//   tree - The GenTree Relop node that was used to set the Condition codes
//
// Return Value: none
//
// Preconditions:
//    The condition codes must already have been appropriately set.
//
void CodeGen::genSetRegToCond(regNumber dstReg, GenTreePtr tree)
{
    // Emit code like that:
    //   ...
    //   bgt True
    //   movs rD, #0
    //   b Next
    // True:
    //   movs rD, #1
    // Next:
    //   ...

    CompareKind  compareKind = ((tree->gtFlags & GTF_UNSIGNED) != 0) ? CK_UNSIGNED : CK_SIGNED;
    emitJumpKind jmpKind     = genJumpKindForOper(tree->gtOper, compareKind);

    BasicBlock* labelTrue = genCreateTempLabel();
    getEmitter()->emitIns_J(emitter::emitJumpKindToIns(jmpKind), labelTrue);

    getEmitter()->emitIns_R_I(INS_mov, emitActualTypeSize(tree->gtType), dstReg, 0);

    BasicBlock* labelNext = genCreateTempLabel();
    getEmitter()->emitIns_J(INS_b, labelNext);

    genDefineTempLabel(labelTrue);
    getEmitter()->emitIns_R_I(INS_mov, emitActualTypeSize(tree->gtType), dstReg, 1);
    genDefineTempLabel(labelNext);
}

//------------------------------------------------------------------------
// genLongToIntCast: Generate code for long to int casts.
//
// Arguments:
//    cast - The GT_CAST node
//
// Return Value:
//    None.
//
// Assumptions:
//    The cast node and its sources (via GT_LONG) must have been assigned registers.
//    The destination cannot be a floating point type or a small integer type.
//
void CodeGen::genLongToIntCast(GenTree* cast)
{
    assert(cast->OperGet() == GT_CAST);

    GenTree* src = cast->gtGetOp1();
    noway_assert(src->OperGet() == GT_LONG);

    genConsumeRegs(src);

    var_types srcType  = ((cast->gtFlags & GTF_UNSIGNED) != 0) ? TYP_ULONG : TYP_LONG;
    var_types dstType  = cast->CastToType();
    regNumber loSrcReg = src->gtGetOp1()->gtRegNum;
    regNumber hiSrcReg = src->gtGetOp2()->gtRegNum;
    regNumber dstReg   = cast->gtRegNum;

    assert((dstType == TYP_INT) || (dstType == TYP_UINT));
    assert(genIsValidIntReg(loSrcReg));
    assert(genIsValidIntReg(hiSrcReg));
    assert(genIsValidIntReg(dstReg));

    if (cast->gtOverflow())
    {
        //
        // Generate an overflow check for [u]long to [u]int casts:
        //
        // long  -> int  - check if the upper 33 bits are all 0 or all 1
        //
        // ulong -> int  - check if the upper 33 bits are all 0
        //
        // long  -> uint - check if the upper 32 bits are all 0
        // ulong -> uint - check if the upper 32 bits are all 0
        //

        if ((srcType == TYP_LONG) && (dstType == TYP_INT))
        {
            BasicBlock* allOne  = genCreateTempLabel();
            BasicBlock* success = genCreateTempLabel();

            inst_RV_RV(INS_tst, loSrcReg, loSrcReg, TYP_INT, EA_4BYTE);
            emitJumpKind JmpNegative = genJumpKindForOper(GT_LT, CK_LOGICAL);
            inst_JMP(JmpNegative, allOne);
            inst_RV_RV(INS_tst, hiSrcReg, hiSrcReg, TYP_INT, EA_4BYTE);
            emitJumpKind jmpNotEqualL = genJumpKindForOper(GT_NE, CK_LOGICAL);
            genJumpToThrowHlpBlk(jmpNotEqualL, SCK_OVERFLOW);
            inst_JMP(EJ_jmp, success);

            genDefineTempLabel(allOne);
            inst_RV_IV(INS_cmp, hiSrcReg, -1, EA_4BYTE);
            emitJumpKind jmpNotEqualS = genJumpKindForOper(GT_NE, CK_SIGNED);
            genJumpToThrowHlpBlk(jmpNotEqualS, SCK_OVERFLOW);

            genDefineTempLabel(success);
        }
        else
        {
            if ((srcType == TYP_ULONG) && (dstType == TYP_INT))
            {
                inst_RV_RV(INS_tst, loSrcReg, loSrcReg, TYP_INT, EA_4BYTE);
                emitJumpKind JmpNegative = genJumpKindForOper(GT_LT, CK_LOGICAL);
                genJumpToThrowHlpBlk(JmpNegative, SCK_OVERFLOW);
            }

            inst_RV_RV(INS_tst, hiSrcReg, hiSrcReg, TYP_INT, EA_4BYTE);
            emitJumpKind jmpNotEqual = genJumpKindForOper(GT_NE, CK_LOGICAL);
            genJumpToThrowHlpBlk(jmpNotEqual, SCK_OVERFLOW);
        }
    }

    if (dstReg != loSrcReg)
    {
        inst_RV_RV(INS_mov, dstReg, loSrcReg, TYP_INT, EA_4BYTE);
    }

    genProduceReg(cast);
}

//------------------------------------------------------------------------
// genIntToFloatCast: Generate code to cast an int/long to float/double
//
// Arguments:
//    treeNode - The GT_CAST node
//
// Return Value:
//    None.
//
// Assumptions:
//    Cast is a non-overflow conversion.
//    The treeNode must have an assigned register.
//    SrcType= int32/uint32/int64/uint64 and DstType=float/double.
//
void CodeGen::genIntToFloatCast(GenTreePtr treeNode)
{
    // int --> float/double conversions are always non-overflow ones
    assert(treeNode->OperGet() == GT_CAST);
    assert(!treeNode->gtOverflow());

    regNumber targetReg = treeNode->gtRegNum;
    assert(genIsValidFloatReg(targetReg));

    GenTreePtr op1 = treeNode->gtOp.gtOp1;
    assert(!op1->isContained());             // Cannot be contained
    assert(genIsValidIntReg(op1->gtRegNum)); // Must be a valid int reg.

    var_types dstType = treeNode->CastToType();
    var_types srcType = op1->TypeGet();
    assert(!varTypeIsFloating(srcType) && varTypeIsFloating(dstType));

    // force the srcType to unsigned if GT_UNSIGNED flag is set
    if (treeNode->gtFlags & GTF_UNSIGNED)
    {
        srcType = genUnsignedType(srcType);
    }

    // We should never see a srcType whose size is neither EA_4BYTE or EA_8BYTE
    // For conversions from small types (byte/sbyte/int16/uint16) to float/double,
    // we expect the front-end or lowering phase to have generated two levels of cast.
    //
    emitAttr srcSize = EA_ATTR(genTypeSize(srcType));
    noway_assert((srcSize == EA_4BYTE) || (srcSize == EA_8BYTE));

    instruction insVcvt = INS_invalid;

    if (dstType == TYP_DOUBLE)
    {
        if (srcSize == EA_4BYTE)
        {
            insVcvt = (varTypeIsUnsigned(srcType)) ? INS_vcvt_u2d : INS_vcvt_i2d;
        }
        else
        {
            assert(srcSize == EA_8BYTE);
            NYI_ARM("Casting int64/uint64 to double in genIntToFloatCast");
        }
    }
    else
    {
        assert(dstType == TYP_FLOAT);
        if (srcSize == EA_4BYTE)
        {
            insVcvt = (varTypeIsUnsigned(srcType)) ? INS_vcvt_u2f : INS_vcvt_i2f;
        }
        else
        {
            assert(srcSize == EA_8BYTE);
            NYI_ARM("Casting int64/uint64 to float in genIntToFloatCast");
        }
    }

    genConsumeOperands(treeNode->AsOp());

    assert(insVcvt != INS_invalid);
    getEmitter()->emitIns_R_R(INS_vmov_i2f, srcSize, treeNode->gtRegNum, op1->gtRegNum);
    getEmitter()->emitIns_R_R(insVcvt, srcSize, treeNode->gtRegNum, treeNode->gtRegNum);

    genProduceReg(treeNode);
}

//------------------------------------------------------------------------
// genFloatToIntCast: Generate code to cast float/double to int/long
//
// Arguments:
//    treeNode - The GT_CAST node
//
// Return Value:
//    None.
//
// Assumptions:
//    Cast is a non-overflow conversion.
//    The treeNode must have an assigned register.
//    SrcType=float/double and DstType= int32/uint32/int64/uint64
//
void CodeGen::genFloatToIntCast(GenTreePtr treeNode)
{
    // we don't expect to see overflow detecting float/double --> int type conversions here
    // as they should have been converted into helper calls by front-end.
    assert(treeNode->OperGet() == GT_CAST);
    assert(!treeNode->gtOverflow());

    regNumber targetReg = treeNode->gtRegNum;
    assert(genIsValidIntReg(targetReg)); // Must be a valid int reg.

    GenTreePtr op1 = treeNode->gtOp.gtOp1;
    assert(!op1->isContained());               // Cannot be contained
    assert(genIsValidFloatReg(op1->gtRegNum)); // Must be a valid float reg.

    var_types dstType = treeNode->CastToType();
    var_types srcType = op1->TypeGet();
    assert(varTypeIsFloating(srcType) && !varTypeIsFloating(dstType));

    // We should never see a dstType whose size is neither EA_4BYTE or EA_8BYTE
    // For conversions to small types (byte/sbyte/int16/uint16) from float/double,
    // we expect the front-end or lowering phase to have generated two levels of cast.
    //
    emitAttr dstSize = EA_ATTR(genTypeSize(dstType));
    noway_assert((dstSize == EA_4BYTE) || (dstSize == EA_8BYTE));

    instruction insVcvt = INS_invalid;

    if (srcType == TYP_DOUBLE)
    {
        if (dstSize == EA_4BYTE)
        {
            insVcvt = (varTypeIsUnsigned(dstType)) ? INS_vcvt_d2u : INS_vcvt_d2i;
        }
        else
        {
            assert(dstSize == EA_8BYTE);
            NYI_ARM("Casting double to int64/uint64 in genIntToFloatCast");
        }
    }
    else
    {
        assert(srcType == TYP_FLOAT);
        if (dstSize == EA_4BYTE)
        {
            insVcvt = (varTypeIsUnsigned(dstType)) ? INS_vcvt_f2u : INS_vcvt_f2i;
        }
        else
        {
            assert(dstSize == EA_8BYTE);
            NYI_ARM("Casting float to int64/uint64 in genIntToFloatCast");
        }
    }

    genConsumeOperands(treeNode->AsOp());

    assert(insVcvt != INS_invalid);
    getEmitter()->emitIns_R_R(insVcvt, dstSize, op1->gtRegNum, op1->gtRegNum);
    getEmitter()->emitIns_R_R(INS_vmov_f2i, dstSize, treeNode->gtRegNum, op1->gtRegNum);

    genProduceReg(treeNode);
}

//------------------------------------------------------------------------
// genEmitHelperCall: Emit a call to a helper function.
//
void CodeGen::genEmitHelperCall(unsigned helper, int argSize, emitAttr retSize, regNumber callTargetReg /*= REG_NA */)
{
    // Can we call the helper function directly

    void *addr = NULL, **pAddr = NULL;

#if defined(DEBUG) && defined(PROFILING_SUPPORTED)
    // Don't ask VM if it hasn't requested ELT hooks
    if (!compiler->compProfilerHookNeeded && compiler->opts.compJitELTHookEnabled &&
        (helper == CORINFO_HELP_PROF_FCN_ENTER || helper == CORINFO_HELP_PROF_FCN_LEAVE ||
         helper == CORINFO_HELP_PROF_FCN_TAILCALL))
    {
        addr = compiler->compProfilerMethHnd;
    }
    else
#endif
    {
        addr = compiler->compGetHelperFtn((CorInfoHelpFunc)helper, (void**)&pAddr);
    }

    if (!addr || !arm_Valid_Imm_For_BL((ssize_t)addr))
    {
        if (callTargetReg == REG_NA)
        {
            // If a callTargetReg has not been explicitly provided, we will use REG_DEFAULT_HELPER_CALL_TARGET, but
            // this is only a valid assumption if the helper call is known to kill REG_DEFAULT_HELPER_CALL_TARGET.
            callTargetReg = REG_DEFAULT_HELPER_CALL_TARGET;
        }

        // Load the address into a register and call through a register
        if (addr)
        {
            instGen_Set_Reg_To_Imm(EA_HANDLE_CNS_RELOC, callTargetReg, (ssize_t)addr);
        }
        else
        {
            getEmitter()->emitIns_R_AI(INS_ldr, EA_PTR_DSP_RELOC, callTargetReg, (ssize_t)pAddr);
            regTracker.rsTrackRegTrash(callTargetReg);
        }

        getEmitter()->emitIns_Call(emitter::EC_INDIR_R, compiler->eeFindHelper(helper),
                                   INDEBUG_LDISASM_COMMA(nullptr) NULL, // addr
                                   argSize, retSize, gcInfo.gcVarPtrSetCur, gcInfo.gcRegGCrefSetCur,
                                   gcInfo.gcRegByrefSetCur,
                                   BAD_IL_OFFSET, // ilOffset
                                   callTargetReg, // ireg
                                   REG_NA, 0, 0,  // xreg, xmul, disp
                                   false,         // isJump
                                   emitter::emitNoGChelper(helper),
                                   (CorInfoHelpFunc)helper == CORINFO_HELP_PROF_FCN_LEAVE);
    }
    else
    {
        getEmitter()->emitIns_Call(emitter::EC_FUNC_TOKEN, compiler->eeFindHelper(helper),
                                   INDEBUG_LDISASM_COMMA(nullptr) addr, argSize, retSize, gcInfo.gcVarPtrSetCur,
                                   gcInfo.gcRegGCrefSetCur, gcInfo.gcRegByrefSetCur, BAD_IL_OFFSET, REG_NA, REG_NA, 0,
                                   0,     /* ilOffset, ireg, xreg, xmul, disp */
                                   false, /* isJump */
                                   emitter::emitNoGChelper(helper),
                                   (CorInfoHelpFunc)helper == CORINFO_HELP_PROF_FCN_LEAVE);
    }

    regTracker.rsTrashRegSet(RBM_CALLEE_TRASH);
    regTracker.rsTrashRegsForGCInterruptability();
}

//------------------------------------------------------------------------
// genStoreLongLclVar: Generate code to store a non-enregistered long lclVar
//
// Arguments:
//    treeNode - A TYP_LONG lclVar node.
//
// Return Value:
//    None.
//
// Assumptions:
//    'treeNode' must be a TYP_LONG lclVar node for a lclVar that has NOT been promoted.
//    Its operand must be a GT_LONG node.
//
void CodeGen::genStoreLongLclVar(GenTree* treeNode)
{
    emitter* emit = getEmitter();

    GenTreeLclVarCommon* lclNode = treeNode->AsLclVarCommon();
    unsigned             lclNum  = lclNode->gtLclNum;
    LclVarDsc*           varDsc  = &(compiler->lvaTable[lclNum]);
    assert(varDsc->TypeGet() == TYP_LONG);
    assert(!varDsc->lvPromoted);
    GenTreePtr op1 = treeNode->gtOp.gtOp1;
    noway_assert(op1->OperGet() == GT_LONG || op1->OperGet() == GT_MUL_LONG);
    genConsumeRegs(op1);

    if (op1->OperGet() == GT_LONG)
    {
        // Definitions of register candidates will have been lowered to 2 int lclVars.
        assert(!treeNode->InReg());

        GenTreePtr loVal = op1->gtGetOp1();
        GenTreePtr hiVal = op1->gtGetOp2();

        // NYI: Contained immediates.
        NYI_IF((loVal->gtRegNum == REG_NA) || (hiVal->gtRegNum == REG_NA),
               "Store of long lclVar with contained immediate");

        emit->emitIns_S_R(ins_Store(TYP_INT), EA_4BYTE, loVal->gtRegNum, lclNum, 0);
        emit->emitIns_S_R(ins_Store(TYP_INT), EA_4BYTE, hiVal->gtRegNum, lclNum, genTypeSize(TYP_INT));
    }
    else if (op1->OperGet() == GT_MUL_LONG)
    {
        assert((op1->gtFlags & GTF_MUL_64RSLT) != 0);

        // Stack store
        getEmitter()->emitIns_S_R(ins_Store(TYP_INT), emitTypeSize(TYP_INT), REG_LNGRET_LO, lclNum, 0);
        getEmitter()->emitIns_S_R(ins_Store(TYP_INT), emitTypeSize(TYP_INT), REG_LNGRET_HI, lclNum,
                                  genTypeSize(TYP_INT));
    }
}

#endif // _TARGET_ARM_

#endif // !LEGACY_BACKEND

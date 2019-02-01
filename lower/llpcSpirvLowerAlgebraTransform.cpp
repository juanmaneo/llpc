/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  llpcSpirvLowerAlgebraTransform.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerAlgebraTransform.
 ***********************************************************************************************************************
 */
#include "hex_float.h"

#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Analysis/ConstantFolding.h"

#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcSpirvLowerAlgebraTransform.h"

#define DEBUG_TYPE "llpc-spirv-lower-algebra-transform"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char SpirvLowerAlgebraTransform::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of SPIR-V lowering opertions for algebraic transformation.
ModulePass* CreateSpirvLowerAlgebraTransform(bool enableConstFolding , bool enableFloatOpt)
{
    return new SpirvLowerAlgebraTransform(enableConstFolding, enableFloatOpt);
}

// =====================================================================================================================
SpirvLowerAlgebraTransform::SpirvLowerAlgebraTransform(
    bool enableConstFolding, // Whether enable constant folding
    bool enableFloatOpt)     // Whether enable floating point optimization
    :
    SpirvLower(ID),
    m_enableConstFolding(enableConstFolding),
    m_enableFloatOpt(enableFloatOpt),
    m_changed(false)
{
    initializeSpirvLowerAlgebraTransformPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
bool SpirvLowerAlgebraTransform::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Algebra-Transform\n");

    SpirvLower::Init(&module);
    m_changed = false;

#if VKI_KHR_SHADER_FLOAT_CONTROLS
    auto pFpControlFlags = &m_pContext->GetShaderResourceUsage(m_shaderStage)->builtInUsage.common;
    if (m_enableConstFolding && (pFpControlFlags->denormFlushToZero != 0))
    {
        // Do constant folding if we need flush denorm to zero.
        auto& targetLibInfo = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
        auto& dataLayout = m_pModule->getDataLayout();

        for (auto& block : *m_pEntryPoint)
        {
            for (auto instIter = block.begin(), instEnd = block.end(); instIter != instEnd;)
            {
                Instruction* pInst = &(*instIter++);

                // DCE instruction if trivially dead.
                if (isInstructionTriviallyDead(pInst, &targetLibInfo))
                {
                    LLVM_DEBUG(dbgs() << "Algebriac transform: DCE: " << *pInst << '\n');
                    pInst->eraseFromParent();
                    m_changed = true;
                    continue;
                }

                // Skip Constant folding if it isn't floating point const expression
                auto pDestType = pInst->getType();
                if (pInst->use_empty() ||
                    (pInst->getNumOperands() == 0) ||
                    (pDestType->isFPOrFPVectorTy() == false) ||
                    (isa<Constant>(pInst->getOperand(0))== false))
                {
                    continue;
                }

                // ConstantProp instruction if trivially constant.
                if (Constant* pConst = ConstantFoldInstruction(pInst, dataLayout, &targetLibInfo))
                {
                    LLVM_DEBUG(dbgs() << "Algebriac transform: constant folding: " << *pConst << " from: " << *pInst
                        << '\n');
                    if ((pDestType->isHalfTy() && ((pFpControlFlags->denormFlushToZero & SPIRVTW_16Bit) != 0)) ||
                        (pDestType->isFloatTy() && ((pFpControlFlags->denormFlushToZero & SPIRVTW_32Bit) != 0)) ||
                        (pDestType->isDoubleTy() && ((pFpControlFlags->denormFlushToZero & SPIRVTW_64Bit) != 0)))
                    {
                        // Replace denorm value with zero
                        if (pConst->isFiniteNonZeroFP() && (pConst->isNormalFP() == false))
                        {
                            pConst = ConstantFP::get(pDestType, 0.0);
                        }
                    }

                    pInst->replaceAllUsesWith(pConst);
                    if (isInstructionTriviallyDead(pInst, &targetLibInfo))
                    {
                        pInst->eraseFromParent();
                    }

                    m_changed = true;
                    continue;
                }

                if (isa<CallInst>(pInst))
                {
                    CallInst* pCallInst = cast<CallInst>(pInst);
                    // LLVM inline pass will do constant folding for _Z14unpackHalf2x16i.
                    // To support floating control correctly, we have to do it by ourselves.
                    if (((pFpControlFlags->denormFlushToZero & SPIRVTW_16Bit) != 0) &&
                        (pCallInst->getCalledFunction()->getName() == "_Z14unpackHalf2x16i"))
                    {
                        auto pSrc = pCallInst->getOperand(0);
                        if (isa<ConstantInt>(pSrc))
                        {
                            auto pConst = cast<ConstantInt>(pSrc);
                            uint64_t constVal = pConst->getZExtValue();
                            APFloat fVal0(APFloat::IEEEhalf(), APInt(16, constVal & 0xFFFF));
                            APFloat fVal1(APFloat::IEEEhalf(), APInt(16, (constVal >> 16) & 0xFFFF));

                            // Flush denorm input value to zero
                            if (fVal0.isDenormal())
                            {
                                fVal0 = APFloat(APFloat::IEEEhalf(), 0);
                            }

                            if (fVal1.isDenormal())
                            {
                                fVal1 = APFloat(APFloat::IEEEhalf(), 0);
                            }

                            bool looseInfo = false;
                            fVal0.convert(APFloat::IEEEsingle(), APFloatBase::rmTowardZero, &looseInfo);
                            fVal1.convert(APFloat::IEEEsingle(), APFloatBase::rmTowardZero, &looseInfo);

                            Constant* constVals[] =
                            {
                                ConstantFP::get(m_pContext->FloatTy(), fVal0.convertToFloat()),
                                ConstantFP::get(m_pContext->FloatTy(), fVal1.convertToFloat())
                            };

                            Constant* pConstVals = ConstantVector::get(constVals);
                            pInst->replaceAllUsesWith(pConstVals);
                            LLVM_DEBUG(dbgs() << "Algebriac transform: constant folding: " << *pConstVals << " from: "
                                << *pInst  << '\n');

                            if (isInstructionTriviallyDead(pInst, &targetLibInfo))
                            {
                                pInst->eraseFromParent();
                            }

                            m_changed = true;
                            continue;
                        }
                    }
                }
            }
        }
    }
#endif

    if (m_enableFloatOpt)
    {
        visit(m_pModule);
    }

    return m_changed;
}

// =====================================================================================================================
// Visits binary operator instruction.
void SpirvLowerAlgebraTransform::visitBinaryOperator(
    llvm::BinaryOperator& binaryOp)  // Binary operator instructions
{
    Instruction::BinaryOps opCode = binaryOp.getOpcode();

    auto pSrc1 = binaryOp.getOperand(0);
    auto pSrc2 = binaryOp.getOperand(1);
    bool src1IsConstZero = isa<ConstantAggregateZero>(pSrc1) ||
                          (isa<ConstantFP>(pSrc1) && cast<ConstantFP>(pSrc1)->isZero());
    bool src2IsConstZero = isa<ConstantAggregateZero>(pSrc2) ||
                          (isa<ConstantFP>(pSrc2) && cast<ConstantFP>(pSrc2)->isZero());
    Value* pDest = nullptr;

    if (opCode == Instruction::FAdd)
    {
        // Recursively find backward if the operand "does not" specify contract flags
        auto fastMathFlags = binaryOp.getFastMathFlags();
        if (fastMathFlags.allowContract())
        {
            bool hasNoContract = IsOperandNoContract(pSrc1) || IsOperandNoContract(pSrc2);
            bool allowContract = !hasNoContract;

            // Reassocation and contract should be same
            fastMathFlags.setAllowReassoc(allowContract);
            fastMathFlags.setAllowContract(allowContract);
            binaryOp.copyFastMathFlags(fastMathFlags);
        }
    }

#if VKI_KHR_SHADER_FLOAT_CONTROLS
    auto pFpControlFlags = &m_pContext->GetShaderResourceUsage(m_shaderStage)->builtInUsage.common;

    // NOTE: We can't skip following floating operations if we need flush denorm or preserve NAN.
    if ((pFpControlFlags->denormFlushToZero == 0) && (pFpControlFlags->signedZeroInfNanPreserve == 0))
#endif
    {
        switch (opCode)
        {
        case Instruction::FAdd:
            {
                if (src1IsConstZero)
                {
                    pDest = pSrc2;
                }
                else if (src2IsConstZero)
                {
                    pDest = pSrc1;
                }

                break;
            }
        case Instruction::FMul:
            {
                if (src1IsConstZero)
                {
                    pDest = pSrc1;
                }
                else if (src2IsConstZero)
                {
                    pDest = pSrc2;
                }
                break;
            }
        case Instruction::FDiv:
            {
                if (src1IsConstZero && (src2IsConstZero == false))
                {
                    pDest = pSrc1;
                }
                break;
            }
        case Instruction::FSub:
            {
                if (src2IsConstZero)
                {
                    pDest = pSrc1;
                }
                break;
            }
        default:
            {
                break;
            }
        }

        if (pDest != nullptr)
        {
            m_changed = true;
            binaryOp.replaceAllUsesWith(pDest);
            binaryOp.dropAllReferences();
            binaryOp.eraseFromParent();
        }
    }

    // Replace fdiv with function call if it isn't optimized
    if ((opCode == Instruction::FDiv) && (pDest == nullptr) && (pSrc1 != nullptr) && (pSrc2 != nullptr))
    {
        BuiltinFuncMangleInfo Info("fdiv");
        Type* argTypes[] = { pSrc1->getType(), pSrc2->getType() };
        Value* args[] = { pSrc1, pSrc2 };
        auto mangledName = SPIRV::mangleBuiltin("fdiv", argTypes, &Info);
        auto pFDiv = EmitCall(m_pModule, mangledName, binaryOp.getType(), args, NoAttrib, &binaryOp);

        binaryOp.replaceAllUsesWith(pFDiv);
        binaryOp.dropAllReferences();
        binaryOp.eraseFromParent();

        m_changed = true;
    }
}

// =====================================================================================================================
// Recursively finds backward if the FPMathOperator operand does not specifiy "contract" flag.
bool SpirvLowerAlgebraTransform::IsOperandNoContract(
    Value *pOperand)  // [in] Operand to check
{
    if (isa<BinaryOperator>(pOperand))
    {
        auto pInst = dyn_cast<BinaryOperator>(pOperand);

        if (isa<FPMathOperator>(pOperand))
        {
            auto fastMathFlags = pInst->getFastMathFlags();
            bool allowContract = fastMathFlags.allowContract();
            if (fastMathFlags.any() && (allowContract == false))
            {
                return true;
            }
        }

        for (auto opIt = pInst->op_begin(), pEnd = pInst->op_end();
            opIt != pEnd; ++opIt)
        {
            return IsOperandNoContract(*opIt);
        }
    }
    return false;
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering opertions for algebraic transformation.
INITIALIZE_PASS(SpirvLowerAlgebraTransform, DEBUG_TYPE,
                "Lower SPIR-V algebraic transforms", false, false)

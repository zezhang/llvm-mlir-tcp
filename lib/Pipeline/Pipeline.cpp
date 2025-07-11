//===------------------------------------------------------------*- C++ -*-===//
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Also available under a BSD-style license. See LICENSE.
//
//===----------------------------------------------------------------------===//

#include "mlir-tcp/Pipeline/Pipeline.h"

#include "mlir-tcp/Conversion/TcpToArith/TcpToArith.h"
#include "mlir-tcp/Conversion/TcpToLinalg/TcpToLinalg.h"
#include "mlir-tcp/Conversion/TcpToTensor/TcpToTensor.h"
#include "mlir-tcp/Conversion/TorchToTcp/TorchToTcp.h"
#include "mlir-tcp/Conversion/TorchToTcp/TorchToTcpCustomOp.h"
#include "mlir-tcp/Dialect/Transforms/DropSymbolicShapeOpsPass.h"
#include "mlir-tcp/Dialect/Transforms/EliminateUnusedTorchOpsPass.h"
#include "mlir-tcp/Dialect/Transforms/TransformTensorOps.h"
#include "mlir-tcp/Dialect/Transforms/VerifyTcpBackendContractPass.h"

#include "mlir/Conversion/AffineToStandard/AffineToStandard.h"
#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/BufferizationToMemRef/BufferizationToMemRef.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h"
#include "mlir/Conversion/MathToLLVM/MathToLLVM.h"
#include "mlir/Conversion/MathToLibm/MathToLibm.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"
#include "mlir/Dialect/Arith/Transforms/Passes.h"
#include "mlir/Dialect/Bufferization/Transforms/OneShotAnalysis.h"
#include "mlir/Dialect/Bufferization/Transforms/Passes.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Func/Transforms/Passes.h"
#include "mlir/Dialect/Linalg/Passes.h"
#include "mlir/Dialect/MemRef/Transforms/Passes.h"
#include "mlir/Dialect/Tensor/Transforms/Passes.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/Passes.h"

#include "torch-mlir/Dialect/TorchConversion/Transforms/Passes.h"

using namespace mlir;

static void createTorchBackendToTcpBackendPipeline(OpPassManager &pm) {
  // Remove unused / unnecessary torch ops first
  pm.addPass(tcp::createEliminateUnusedTorchOpsPass());

  // Torch -> TCP conversions.
  pm.addNestedPass<func::FuncOp>(tcp::createConvertTorchToTcpPass());
  pm.addNestedPass<func::FuncOp>(tcp::createConvertTorchToTcpCustomOpPass());

  // Clean up any non-canonical code introduced above.
  pm.addNestedPass<func::FuncOp>(createCanonicalizerPass());
  // The resolution of `dim` ops tends to create identical ops. CSE them.
  pm.addNestedPass<func::FuncOp>(createCSEPass());

  // Finish the type conversion from `torch` types to the types of the
  // TCP backend contract.
  pm.addPass(torch::TorchConversion::createFuncBackendTypeConversionPass());
  pm.addNestedPass<func::FuncOp>(createCanonicalizerPass());
  pm.addNestedPass<func::FuncOp>(
      torch::TorchConversion::createFinalizingBackendTypeConversionPass());

  // Verify that we have lowered to the form that TCP backend expects.
  // This fails compilation (signalPassFailure) if the IR is not in the
  // correct form.
  pm.addPass(tcp::createVerifyTcpBackendContractPass());
}

static void createTcpToLlvmPipeline(OpPassManager &pm) {
  // Drop TCP symbolic shape ops for dynamic dims
  pm.addNestedPass<func::FuncOp>(tcp::createDropSymbolicShapeOpsPass());

  // TCP transformations.
  pm.addNestedPass<func::FuncOp>(tcp::createDecomposeTensorOpsPass());

  // TCP -> Linalg/Arith conversions.
  pm.addNestedPass<func::FuncOp>(tcp::createConvertTcpToLinalgPass());
  pm.addNestedPass<func::FuncOp>(tcp::createConvertTcpToTensorPass());
  pm.addNestedPass<func::FuncOp>(tcp::createConvertTcpToArithPass());

  // One-shot bufferize tensor -> memref, from
  // https://mlir.llvm.org/docs/Bufferization/.
  bufferization::OneShotBufferizePassOptions bufferizationOptions;
  bufferizationOptions.bufferizeFunctionBoundaries = true;
  bufferizationOptions.functionBoundaryTypeConversion =
      bufferization::LayoutMapOption::IdentityLayoutMap;
  pm.addPass(bufferization::createOneShotBufferizePass(bufferizationOptions));
  // Buffer deallocation pipeline for automatically inserting
  // buffer deallocation ops after one-shot bufferization.
  // https://sourcegraph.com/github.com/llvm/llvm-project@09bc1e825068f314db71ee7eb32d9f93c5ac87a0/-/blob/mlir/lib/Dialect/Bufferization/Pipelines/BufferizationPipelines.cpp?L21
  pm.addPass(createCanonicalizerPass());
  pm.addPass(bufferization::createOwnershipBasedBufferDeallocationPass());
  pm.addPass(createCanonicalizerPass());
  pm.addPass(bufferization::createBufferDeallocationSimplificationPass());
  pm.addPass(bufferization::createLowerDeallocationsPass());
  pm.addPass(createCSEPass());
  pm.addPass(createCanonicalizerPass());
  pm.addPass(createConvertBufferizationToMemRefPass());

  // Blanket-convert any remaining linalg ops to loops if any remain.
  pm.addNestedPass<func::FuncOp>(createConvertLinalgToLoopsPass());
  // Blanket-convert any remaining affine ops if any remain.
  pm.addPass(createLowerAffinePass());
  // Convert SCF to CF (always needed).
  pm.addPass(createSCFToControlFlowPass());

  // Sprinkle some cleanups.
  pm.addPass(createCanonicalizerPass());
  pm.addPass(createCSEPass());

  // Convert Math to LLVM (always needed).
  pm.addPass(createConvertMathToLLVMPass());
  pm.addPass(createConvertMathToLibmPass());
  // Expand complicated MemRef operations before lowering them.
  pm.addPass(memref::createExpandStridedMetadataPass());
  // The expansion may create affine expressions. Get rid of them.
  pm.addPass(createLowerAffinePass());
  // Convert Arith (from affine lowering) to LLVM.
  pm.addNestedPass<func::FuncOp>(createArithToLLVMConversionPass());
  // Convert MemRef to LLVM (always needed).
  pm.addPass(createFinalizeMemRefToLLVMConversionPass());
  // Convert Func to LLVM (always needed).
  pm.addPass(createConvertFuncToLLVMPass());
  // Convert CF to LLVM (always needed).
  pm.addPass(createConvertControlFlowToLLVMPass());

  // Sprinkle some cleanups.
  pm.addPass(createCanonicalizerPass());
  pm.addPass(createCSEPass());

  // Convert remaining unrealized_casts (always needed).
  pm.addPass(createReconcileUnrealizedCastsPass());
}

void tcp::registerTcpPipelines() {
  PassPipelineRegistration<>(
      "torch-backend-to-tcp-backend-pipeline",
      "Pipeline lowering torch backend contract to TCP backend contract.",
      createTorchBackendToTcpBackendPipeline);

  PassPipelineRegistration<>("tcp-to-llvm-pipeline", "Lowers TCP to LLVM",
                             createTcpToLlvmPipeline);
}

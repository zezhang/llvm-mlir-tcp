//===-------------------------------------------------------*- tablegen -*-===//
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Also available under a BSD-style license. See LICENSE.
//
//===----------------------------------------------------------------------===//

#ifndef TCP_CONVERSION_PASSES
#define TCP_CONVERSION_PASSES

include "mlir/Pass/PassBase.td"

//===----------------------------------------------------------------------===//
// TorchToTcp
//===----------------------------------------------------------------------===//

def ConvertTorchToTcp : Pass<"convert-torch-to-tcp", "func::FuncOp"> {
  let summary = "Convert Torch ops to Tcp ops";
  let description = [{
    Convert Torch ops to Tcp ops.
  }];
  let constructor = "mlir::tcp::createConvertTorchToTcpPass()";
  let options = [
    ListOption<"convertTorchOps", "convert-torch-ops", "std::string",
               "List of Torch operation names that should be converted to Tcp",
               "llvm::cl::ZeroOrMore">,
  ];
}

//===----------------------------------------------------------------------===//
// TorchToTcpCustomOp
//===----------------------------------------------------------------------===//

def ConvertTorchToTcpCustomOp : Pass<"convert-torch-to-tcp-custom-op", "func::FuncOp"> {
  let summary = "Convert Torch ops to Tcp custom ops";
  let description = [{
    Convert Torch ops to Tcp custom ops.
  }];
  let constructor = "mlir::tcp::createConvertTorchToTcpCustomOpPass()";
  let options = [
    ListOption<"convertTorchOps", "convert-torch-ops", "std::string",
               "List of Torch operation names that should be converted to Tcp custom op",
               "llvm::cl::ZeroOrMore">,
  ];
}

//===----------------------------------------------------------------------===//
// TcpToLinalg
//===----------------------------------------------------------------------===//

def ConvertTcpToLinalg
      : Pass<"convert-tcp-to-linalg", "func::FuncOp"> {
  let summary = "Lower TCP to Linalg";
  let description = [{
    Pass that converts TCP operations to equivalent operations in Linalg.
  }];

  let constructor = "mlir::tcp::createConvertTcpToLinalgPass()";
  let dependentDialects = [
    "mlir::linalg::LinalgDialect",
  ];
}

//===----------------------------------------------------------------------===//
// TcpToArith
//===----------------------------------------------------------------------===//

def ConvertTcpToArith
      : Pass<"convert-tcp-to-arith", "func::FuncOp"> {
  let summary = "Lower TCP to Arith";
  let description = [{
    Pass that converts TCP operations to equivalent operations in Arith.
  }];

  let constructor = "mlir::tcp::createConvertTcpToArithPass()";
  let dependentDialects = [
    "mlir::arith::ArithDialect",
  ];
}

//===----------------------------------------------------------------------===//
// TcpToTensor
//===----------------------------------------------------------------------===//

def ConvertTcpToTensor
      : Pass<"convert-tcp-to-tensor", "func::FuncOp"> {
  let summary = "Lower TCP to Tensor";
  let description = [{
    Pass that converts TCP operations to equivalent operations in Tensor.
  }];

  let constructor = "mlir::tcp::createConvertTcpToTensorPass()";
  let dependentDialects = [
    "mlir::tensor::TensorDialect",
  ];
}

#endif // TCP_CONVERSION_PASSES

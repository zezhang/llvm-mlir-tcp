//===------------------------------------------------------------*- C++ -*-===//
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Also available under a BSD-style license. See LICENSE.
//
//===----------------------------------------------------------------------===//

#include "mlir-tcp/Conversion/TorchToTcp/TorchToTcp.h"

#include "mlir-tcp/Dialect/IR/TcpDialect.h"
#include "mlir-tcp/Dialect/IR/TcpOps.h"

#include "PopulatePatterns.h"
#include "Utils.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "torch-mlir/Conversion/Utils/Utils.h"
#include "torch-mlir/Dialect/Torch/IR/TorchOps.h"
#include "torch-mlir/Dialect/Torch/Utils/Utils.h"
#include "torch-mlir/Dialect/TorchConversion/IR/TorchConversionOps.h"

#include "llvm/ADT/StringSet.h"

using namespace mlir;
using namespace mlir::tcp;
using namespace mlir::torch;
using namespace mlir::torch::Torch;

namespace {

class ConvertAtenGatherOp : public OpConversionPattern<AtenGatherOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenGatherOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    torch_to_tcp::TorchToTcpCustomOpConversionHelper helper{op, rewriter,
                                                            getTypeConverter()};

    helper.addOperand("self", adaptor.getSelf());
    helper.addOperand("index", adaptor.getIndex());
    helper.addIntAttr("axis", op.getDim());

    return helper.replace();
  }
};
class ConvertAten_IndexPutImplOp
    : public OpConversionPattern<Aten_IndexPutImplOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(Aten_IndexPutImplOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {

    torch_to_tcp::TorchToTcpCustomOpConversionHelper helper{op, rewriter,
                                                            getTypeConverter()};
    helper.addOperand("self", adaptor.getSelf());
    helper.addAsMultipleTensorOperands("index_", adaptor.getIndices());
    helper.addOperand("values", adaptor.getValues());
    helper.addBoolAttr("accumulate", op.getAccumulate());
    helper.addBoolAttr("unsafe", op.getUnsafe());

    return helper.replace();
  }
};

class ConvertAtenConvolutionOp : public OpConversionPattern<AtenConvolutionOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenConvolutionOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    torch_to_tcp::TorchToTcpCustomOpConversionHelper helper{op, rewriter,
                                                            getTypeConverter()};
    helper.addOperand("input", adaptor.getInput());
    helper.addOperand("weight", adaptor.getWeight());
    if (!isa<Torch::NoneType>(adaptor.getBias().getType())) {
      helper.addOperand("bias", adaptor.getBias());
    }

    helper.addListOfIntsAttr("stride", adaptor.getStride());
    helper.addListOfIntsAttr("padding", adaptor.getPadding());
    helper.addListOfIntsAttr("dilation", adaptor.getDilation());
    helper.addListOfIntsAttr("output_padding", adaptor.getOutputPadding());
    helper.addBoolAttr("transposed", op.getTransposed());
    helper.addIntAttr("groups", op.getGroups());

    return helper.replace();
  }
};

class ConvertAtenFakeQuantizePerTensorAffineOp
    : public OpConversionPattern<AtenFakeQuantizePerTensorAffineOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenFakeQuantizePerTensorAffineOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    torch_to_tcp::TorchToTcpCustomOpConversionHelper helper{op, rewriter,
                                                            getTypeConverter()};
    helper.addOperand("self", adaptor.getSelf());
    helper.addFloatAttr("scale", op.getScale());
    helper.addIntAttr("zero_point", op.getZeroPoint());
    helper.addIntAttr("quant_min", op.getQuantMin());
    helper.addIntAttr("quant_max", op.getQuantMax());

    return helper.replace();
  }
};

class ConvertAtenFakeQuantizePerTensorAffineTensorQparamsOp
    : public OpConversionPattern<
          AtenFakeQuantizePerTensorAffineTensorQparamsOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenFakeQuantizePerTensorAffineTensorQparamsOp op,
                  OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    torch_to_tcp::TorchToTcpCustomOpConversionHelper helper{op, rewriter,
                                                            getTypeConverter()};
    helper.addOperand("self", adaptor.getSelf());
    helper.addIntAttr("quant_min", op.getQuantMin());
    helper.addIntAttr("quant_max", op.getQuantMax());

    // scale
    auto scaleTy = dyn_cast<RankedTensorType>(adaptor.getScale().getType());
    if (!scaleTy || scaleTy.getShape().size() != 1 ||
        scaleTy.getNumElements() != 1)
      // scale should be a [1] tensor.
      return rewriter.notifyMatchFailure(op, "Unsupported scale type or size");
    helper.addOperand("scale", adaptor.getScale());

    // zero_point
    auto zeroPointTy =
        dyn_cast<RankedTensorType>(adaptor.getZeroPoint().getType());
    if (!zeroPointTy || zeroPointTy.getShape().size() != 1 ||
        zeroPointTy.getNumElements() != scaleTy.getNumElements())
      // zero_point should be a [1] tensor.
      return rewriter.notifyMatchFailure(op,
                                         "Unsupported zero point type or size");
    helper.addOperand("zero_point", adaptor.getZeroPoint());

    return helper.replace();
  }
};

class ConvertAtenFakeQuantizePerChannelAffineOp
    : public OpConversionPattern<AtenFakeQuantizePerChannelAffineOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenFakeQuantizePerChannelAffineOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    torch_to_tcp::TorchToTcpCustomOpConversionHelper helper{op, rewriter,
                                                            getTypeConverter()};
    helper.addOperand("self", adaptor.getSelf());
    helper.addIntAttr("axis", op.getAxis());
    helper.addIntAttr("quant_min", op.getQuantMin());
    helper.addIntAttr("quant_max", op.getQuantMax());

    // scale
    auto scaleTy = dyn_cast<RankedTensorType>(adaptor.getScale().getType());
    if (!scaleTy || scaleTy.getShape().size() != 1)
      // scale should be a [C] tensor.
      return rewriter.notifyMatchFailure(op, "Unsupported scale type or size");
    helper.addOperand("scale", adaptor.getScale());

    // zero_point
    auto zeroPointTy =
        dyn_cast<RankedTensorType>(adaptor.getZeroPoint().getType());
    if (!zeroPointTy || zeroPointTy.getShape().size() != 1 ||
        zeroPointTy.getNumElements() != scaleTy.getNumElements())
      // zero_point should be a [C] tensor.
      return rewriter.notifyMatchFailure(op,
                                         "Unsupported zero point type or size");
    helper.addOperand("zero_point", adaptor.getZeroPoint());

    return helper.replace();
  }
};

class ConvertAtenTopkOp : public OpConversionPattern<AtenTopkOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenTopkOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    torch_to_tcp::TorchToTcpCustomOpConversionHelper helper{op, rewriter,
                                                            getTypeConverter()};
    helper.addOperand("self", adaptor.getSelf());

    helper.addIntAttr("k", op.getK());
    helper.addIntAttr("dim", op.getDim());
    helper.addBoolAttr("largest", op.getLargest());
    helper.addBoolAttr("sorted", op.getSorted());

    return helper.replace();
  }
};

class ConvertAtenSortOp : public OpConversionPattern<AtenSortOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenSortOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    torch_to_tcp::TorchToTcpCustomOpConversionHelper helper{op, rewriter,
                                                            getTypeConverter()};
    helper.addOperand("self", adaptor.getSelf());

    helper.addIntAttr("dim", op.getDim());
    helper.addBoolAttr("descending", op.getDescending());

    return helper.replace();
  }
};

class ConvertAtenCumsumOp : public OpConversionPattern<AtenCumsumOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenCumsumOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    torch_to_tcp::TorchToTcpCustomOpConversionHelper helper{op, rewriter,
                                                            getTypeConverter()};
    helper.addOperand("self", adaptor.getSelf());

    helper.addIntAttr("dim", op.getDim());
    if (!isa<Torch::ConstantNoneOp>(op.getDtype().getDefiningOp()))
      return rewriter.notifyMatchFailure(op, "Unsupported dtype argument");

    return helper.replace();
  }
};

class ConvertAtenMinDimOp : public OpConversionPattern<AtenMinDimOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenMinDimOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    torch_to_tcp::TorchToTcpCustomOpConversionHelper helper{op, rewriter,
                                                            getTypeConverter()};
    helper.addOperand("self", adaptor.getSelf());

    helper.addIntAttr("dim", op.getDim());
    helper.addBoolAttr("keepdim", op.getKeepdim());

    return helper.replace();
  }
};

class ConvertAtenViewOp : public OpConversionPattern<AtenViewOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenViewOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    torch_to_tcp::TorchToTcpCustomOpConversionHelper helper{op, rewriter,
                                                            getTypeConverter()};
    Value self = adaptor.getSelf();
    SmallVector<int64_t> size;
    // static size array will be handled through TOSA dialect
    if (matchPattern(op.getSize(), m_TorchListOfConstantInts(size)))
      return rewriter.notifyMatchFailure(op,
                                         "only non-constant size is supported");

    helper.addOperand("self", self);
    Operation *primListOp = op.getSize().getDefiningOp();
    auto listConstruct = dyn_cast<Torch::PrimListConstructOp>(primListOp);
    if (!listConstruct) {
      return rewriter.notifyMatchFailure(
          op, "Size must come from PrimListConstructOp");
    }
    int idx = 0;
    for (Value value : listConstruct.getElements()) {
      int64_t dimSize;
      if (matchPattern(value, m_TorchConstantInt(&dimSize))) {
        size.push_back(dimSize);
      } else {
        size.push_back(ShapedType::kDynamic);
        // dynamic shape should follow pattern:
        // %dim_32 = tensor.dim %arg1, %c0 : tensor<?x2736x16xf32>
        // %1 = arith.index_cast %dim_32 : index to i64
        // %2 = torch_c.from_i64 %1
        // %3 = torch.prim.ListConstruct %2 ...
        if (!isa<TorchConversion::FromI64Op>(value.getDefiningOp()))
          return rewriter.notifyMatchFailure(
              op, "dynamic dim size should come from FromI64Op");
        auto conversionOp =
            dyn_cast<TorchConversion::FromI64Op>(value.getDefiningOp());
        if (!isa<arith::IndexCastOp>(conversionOp.getOperand().getDefiningOp()))
          return rewriter.notifyMatchFailure(
              op, "dynamic dim size should come from IndexCastOp");
        auto indexCastOp = dyn_cast<arith::IndexCastOp>(
            conversionOp.getOperand().getDefiningOp());
        if (!isa<tensor::DimOp>(indexCastOp.getIn().getDefiningOp()))
          return rewriter.notifyMatchFailure(
              op, "dynamic dim size should come from DimOp");
        auto dimOp =
            dyn_cast<tensor::DimOp>(indexCastOp.getIn().getDefiningOp());
        helper.addOperand("idx_" + std::to_string(idx), dimOp);
      }
      idx++;
    }
    helper.addDenseIntArrayAttr("size", size);

    return helper.replace();
  }
};

class ConvertAtenSliceScatterOp
    : public OpConversionPattern<AtenSliceScatterOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenSliceScatterOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // this should really have some tcp op to reduce to.  So going to CustomOp
    // is more of a placeholder than a serious implementation
    torch_to_tcp::TorchToTcpCustomOpConversionHelper helper{op, rewriter,
                                                            getTypeConverter()};
    helper.addOperand("self", adaptor.getSelf());
    helper.addOperand("src", adaptor.getSrc());
    helper.addIntAttr("dim", op.getDim());
    helper.addIntAttr("start", op.getStart());
    helper.addIntAttr("end", op.getEnd());
    helper.addIntAttr("step", op.getStep());

    return helper.replace();
  }
};

class ConvertAtenArangeStartStepOp
    : public OpConversionPattern<AtenArangeStartStepOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenArangeStartStepOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {

    // At this point all tensors should have value semantics, and hence the
    // `layout` check can be ignored.

    // The pin_memory should be either `False` or `none`.
    bool pinMemory;
    if (!isa<Torch::NoneType>(op.getPinMemory().getType()) &&
        (!matchPattern(op.getPinMemory(), m_TorchConstantBool(&pinMemory)) ||
         pinMemory)) {
      return rewriter.notifyMatchFailure(
          op, "unimplemented: pin_memory must be either None or false");
    }

    torch_to_tcp::TorchToTcpCustomOpConversionHelper helper{op, rewriter,
                                                            getTypeConverter()};
    bool allStatic = true;
    // trt-mlir takes F64Attr, so we need to convert const int to fp attr
    if (!helper.tryConvertConstToFloatAttr("start", op.getStart())) {
      allStatic = false;
      helper.addOperand("start", adaptor.getStart());
    }
    if (!helper.tryConvertConstToFloatAttr("end", op.getEnd())) {
      allStatic = false;
      helper.addOperand("end", adaptor.getEnd());
    }
    if (!helper.tryConvertConstToFloatAttr("step", op.getStep())) {
      allStatic = false;
      helper.addOperand("step", adaptor.getStep());
    }
    // static start, end, and step case will be handled through TOSA dialect
    if (allStatic)
      return rewriter.notifyMatchFailure(op,
                                         "only non-constant values supported");

    return helper.replace();
  }
};

} // namespace

void torch_to_tcp::populateTcpCustomOpPatternsAndLegality(
    TypeConverter &typeConverter, RewritePatternSet &patterns,
    ConversionTarget &target, const llvm::StringSet<> &convertTorchOpsSet) {

#define INSERT_ATEN_TO_TCP_CUSTOM_OP_PATTERN(AtenOp)                           \
  torch_to_tcp::addPatternIfOpInConvertTorchOpsSet<Convert##AtenOp, AtenOp>(   \
      typeConverter, patterns, target, convertTorchOpsSet)
  INSERT_ATEN_TO_TCP_CUSTOM_OP_PATTERN(AtenGatherOp);
  INSERT_ATEN_TO_TCP_CUSTOM_OP_PATTERN(Aten_IndexPutImplOp);
  INSERT_ATEN_TO_TCP_CUSTOM_OP_PATTERN(AtenFakeQuantizePerTensorAffineOp);
  INSERT_ATEN_TO_TCP_CUSTOM_OP_PATTERN(
      AtenFakeQuantizePerTensorAffineTensorQparamsOp);
  INSERT_ATEN_TO_TCP_CUSTOM_OP_PATTERN(AtenFakeQuantizePerChannelAffineOp);
  INSERT_ATEN_TO_TCP_CUSTOM_OP_PATTERN(AtenTopkOp);
  INSERT_ATEN_TO_TCP_CUSTOM_OP_PATTERN(AtenSortOp);
  INSERT_ATEN_TO_TCP_CUSTOM_OP_PATTERN(AtenCumsumOp);
  INSERT_ATEN_TO_TCP_CUSTOM_OP_PATTERN(AtenMinDimOp);
  INSERT_ATEN_TO_TCP_CUSTOM_OP_PATTERN(AtenSliceScatterOp);
  // Following ops can still live after torch-to-tcp conversion
  patterns.add<ConvertAtenViewOp>(typeConverter, patterns.getContext());
  patterns.add<ConvertAtenArangeStartStepOp>(typeConverter,
                                             patterns.getContext());
#undef INSERT_ATEN_TO_TCP_CUSTOM_OP_PATTERN

  // Torch -> TOSA doesn't handle transposed convolutions; map them to
  // TCP custom_op instead.
  auto isTransposedConvOp = [](AtenConvolutionOp op) {
    bool transposed;
    if (!matchPattern(op.getTransposed(), m_TorchConstantBool(&transposed)))
      return false;
    return transposed;
  };

  // Torch -> TOSA supports only 2D convolutions; map the rest to
  // TCP custom_op instead.
  auto is2dConvOp = [](AtenConvolutionOp op) {
    auto inputTy = cast<torch::Torch::ValueTensorType>(op.getInput().getType());
    return inputTy.getSizes().size() == 4;
  };

  // Mark only regular (non-transposed) 2D convolutions as legal (in Torch
  // dialect). i.e. don't convert them to TCP custom_op and leave them in Torch,
  // to be handled by Torch -> TOSA later.
  torch_to_tcp::addPatternIfOpInConvertTorchOpsSet<ConvertAtenConvolutionOp,
                                                   AtenConvolutionOp>(
      typeConverter, patterns, target, convertTorchOpsSet,
      [&](AtenConvolutionOp op) {
        return !isTransposedConvOp(op) && is2dConvOp(op);
      });
}

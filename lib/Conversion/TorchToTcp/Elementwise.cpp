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
#include "torch-mlir/Dialect/Torch/IR/TorchDialect.h"
#include "torch-mlir/Dialect/Torch/IR/TorchOps.h"
#include "torch-mlir/Dialect/Torch/Utils/Utils.h"

using namespace mlir;
using namespace mlir::tcp;
using namespace mlir::torch;
using namespace mlir::torch::Torch;

namespace {

bool IsMultiplyAlphaOne(Value alphaValue) {
  double doubleValue;
  auto isFloat = matchPattern(alphaValue, m_TorchConstantFloat(&doubleValue));

  int64_t intValue;
  auto isInt = matchPattern(alphaValue, m_TorchConstantInt(&intValue));

  return ((isFloat && doubleValue == 1.0) || (isInt && intValue == 1.0));
}

Value convertScalarOperandToTensor(ConversionPatternRewriter &rewriter,
                                   Operation *op, Value scalarValue,
                                   Value convertedScalarValue, Type outputType,
                                   Type convertedOutputType) {
  RankedTensorType scalarToTensorType =
      RankedTensorType::get({}, convertedScalarValue.getType());
  Value resultValue = torch_to_tcp::scalarToTcpTensor(
      rewriter, op, scalarToTensorType, scalarValue);
  if (isa<mlir::FloatType>(convertedScalarValue.getType()))
    // FP scalarValue is treated as fp64
    resultValue = torch_to_tcp::castTensorToDtype(
        rewriter, rewriter.getF64Type(), outputType, resultValue,
        convertedOutputType);
  else if (isa<mlir::IntegerType>(convertedScalarValue.getType()))
    // INT scalarValue is treated as si64
    resultValue = torch_to_tcp::castTensorToDtype(
        rewriter, rewriter.getIntegerType(64, true), outputType, resultValue,
        convertedOutputType);
  return resultValue;
}

template <typename AtenOpT, typename TcpOpT>
class ConvertAtenAddSubOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;

  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value lhs = adaptor.getSelf();
    RankedTensorType lhsType = dyn_cast<RankedTensorType>(lhs.getType());

    Value rhs = adaptor.getOther();

    RankedTensorType resultType = cast<RankedTensorType>(
        OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
            op.getType()));

    if (!lhsType || !resultType)
      return rewriter.notifyMatchFailure(
          op, "Only Ranked Tensor types are supported in TCP");

    auto inputAType =
        dyn_cast<torch::Torch::ValueTensorType>(op.getSelf().getType())
            .getDtype();
    auto outputType =
        dyn_cast<torch::Torch::ValueTensorType>(op.getType()).getDtype();

    if (isa<AtenAddScalarOp>(op) || isa<AtenSubScalarOp>(op)) {
      rhs = convertScalarOperandToTensor(rewriter, op, op.getOther(),
                                         adaptor.getOther(), outputType,
                                         resultType.getElementType());
      if (!rhs)
        return rewriter.notifyMatchFailure(op, "Unsupported rhs data type");
    } else {
      auto inputBType =
          dyn_cast<torch::Torch::ValueTensorType>(op.getOther().getType())
              .getDtype();
      rhs = torch_to_tcp::castTensorToDtype(rewriter, inputBType, outputType,
                                            rhs, resultType.getElementType());
    }
    lhs = torch_to_tcp::castTensorToDtype(rewriter, inputAType, outputType, lhs,
                                          resultType.getElementType());
    std::tie(lhs, rhs) =
        torch_to_tcp::broadcastToMatchShape(rewriter, lhs, rhs);

    if (!IsMultiplyAlphaOne(op.getAlpha())) {
      Value alpha = convertScalarOperandToTensor(rewriter, op, op.getAlpha(),
                                                 adaptor.getAlpha(), outputType,
                                                 resultType.getElementType());
      if (!alpha)
        return rewriter.notifyMatchFailure(op, "Unsupported alpha data type");
      std::tie(alpha, rhs) =
          torch_to_tcp::broadcastToMatchShape(rewriter, alpha, rhs);
      rhs = rewriter.create<tcp::MulOp>(op->getLoc(), resultType, alpha, rhs);
    }

    rewriter.replaceOpWithNewOp<TcpOpT>(op, resultType, lhs, rhs);
    return success();
  }
};

template <typename AtenOpT>
class ConvertAtenMulOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;

  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value lhs = adaptor.getSelf();
    RankedTensorType lhsType = dyn_cast<RankedTensorType>(lhs.getType());

    Value rhs = adaptor.getOther();

    RankedTensorType resultType = cast<RankedTensorType>(
        OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
            op.getType()));

    if (!lhsType || !resultType)
      return rewriter.notifyMatchFailure(
          op, "Only Ranked Tensor types are supported in TCP");

    auto inputAType =
        dyn_cast<torch::Torch::ValueTensorType>(op.getSelf().getType())
            .getDtype();
    auto outputType =
        dyn_cast<torch::Torch::ValueTensorType>(op.getType()).getDtype();

    if (isa<AtenMulScalarOp>(op)) {
      rhs = convertScalarOperandToTensor(rewriter, op, op.getOther(),
                                         adaptor.getOther(), outputType,
                                         resultType.getElementType());
      if (!rhs)
        return rewriter.notifyMatchFailure(op, "Unsupported rhs data type");
    } else {
      auto inputBType =
          dyn_cast<torch::Torch::ValueTensorType>(op.getOther().getType())
              .getDtype();
      rhs = torch_to_tcp::castTensorToDtype(rewriter, inputBType, outputType,
                                            rhs, resultType.getElementType());
    }
    lhs = torch_to_tcp::castTensorToDtype(rewriter, inputAType, outputType, lhs,
                                          resultType.getElementType());
    std::tie(lhs, rhs) =
        torch_to_tcp::broadcastToMatchShape(rewriter, lhs, rhs);

    rewriter.replaceOpWithNewOp<tcp::MulOp>(op, resultType, lhs, rhs);
    return success();
  }
};

class ConvertAtenBatchNormOp : public OpConversionPattern<AtenBatchNormOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenBatchNormOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value input = adaptor.getInput();
    RankedTensorType inputType = dyn_cast<RankedTensorType>(input.getType());

    Value weight = adaptor.getWeight();
    RankedTensorType weightType = dyn_cast<RankedTensorType>(weight.getType());

    Value bias = adaptor.getBias();
    RankedTensorType biasType = dyn_cast<RankedTensorType>(bias.getType());

    Value runningMean = adaptor.getRunningMean();
    RankedTensorType runningMeanType =
        dyn_cast<RankedTensorType>(runningMean.getType());

    Value runningVar = adaptor.getRunningVar();
    RankedTensorType runningVarType =
        dyn_cast<RankedTensorType>(runningVar.getType());

    RankedTensorType resultType =
        cast<RankedTensorType>(getTypeConverter()->convertType(op.getType()));

    if (!inputType || !weightType || !biasType || !runningMeanType ||
        !runningVarType || !resultType)
      return rewriter.notifyMatchFailure(
          op, "only Ranked Tensor types are supported in TCP");

    if (runningMeanType.getNumElements() == 0 ||
        runningVarType.getNumElements() == 0)
      return rewriter.notifyMatchFailure(
          op, "zero element running_mean and running_var not supported");

    double eps = 0.0;
    if (!matchPattern(op.getEps(), m_TorchConstantFloat(&eps)))
      return rewriter.notifyMatchFailure(op,
                                         "non-float(double) eps unsupported");

    Value epsVal;
    if (auto result = torch_to_tcp::getConstTensor<float>(
            rewriter, op, llvm::ArrayRef(static_cast<float>(eps)), {}))
      epsVal = *result;
    else
      return rewriter.notifyMatchFailure(op,
                                         "failed to get constTensor for eps");

    // momentum is ignored
    Value momentum = adaptor.getMomentum();
    (void)momentum;

    // cudnnEnabled is ignored
    Value cudnnEnabled = adaptor.getCudnnEnabled();
    (void)cudnnEnabled;

    bool training = false;
    if (!matchPattern(op.getTraining(), m_TorchConstantBool(&training)))
      return rewriter.notifyMatchFailure(op, "non-bool training unsupported");
    if (training)
      return rewriter.notifyMatchFailure(
          op, "only inference mode batch_norm lowering supported");

    // PyTorch inputs are [NCHW], and BatchNorm parameters are [C] length
    // vectors. `axisInOutput = 1` allows [C] -> [1, C, 1, 1] expansion
    // followed by a broadcast.
    runningMean = torch_to_tcp::broadcast0DOr1DToNDAndMatchShape(
        rewriter, runningMean, input, inputType.getElementType(),
        /*axisInOutput=*/1);
    runningVar = torch_to_tcp::broadcast0DOr1DToNDAndMatchShape(
        rewriter, runningVar, input, inputType.getElementType(),
        /*axisInOutput=*/1);
    weight = torch_to_tcp::broadcast0DOr1DToNDAndMatchShape(
        rewriter, weight, input, inputType.getElementType(),
        /*axisInOutput=*/1);
    bias = torch_to_tcp::broadcast0DOr1DToNDAndMatchShape(
        rewriter, bias, input, inputType.getElementType(), /*axisInOutput=*/1);
    epsVal = torch_to_tcp::broadcast0DOr1DToNDAndMatchShape(
        rewriter, epsVal, input, inputType.getElementType());

    Value op1SubInputMean = rewriter.create<tcp::SubOp>(op.getLoc(), resultType,
                                                        input, runningMean);
    Value op2AddVarEpsilon = rewriter.create<tcp::AddOp>(
        op.getLoc(), resultType, runningVar, epsVal);
    Value op3SqrtOp2 =
        rewriter.create<tcp::SqrtOp>(op.getLoc(), resultType, op2AddVarEpsilon);
    Value op4DivOp1Op3 = rewriter.create<tcp::DivFOp>(
        op.getLoc(), resultType, op1SubInputMean, op3SqrtOp2);
    Value op5MulWeightOp4 = rewriter.create<tcp::MulOp>(op.getLoc(), resultType,
                                                        weight, op4DivOp1Op3);
    Value op6AddOp5Bias = rewriter.create<tcp::AddOp>(op.getLoc(), resultType,
                                                      op5MulWeightOp4, bias);

    rewriter.replaceOp(op, {op6AddOp5Bias});
    return success();
  }
};

template <typename AtenOpT>
class ConvertAtenDivOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;

  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value lhs = adaptor.getSelf();
    RankedTensorType lhsType = dyn_cast<RankedTensorType>(lhs.getType());

    Value rhs = adaptor.getOther();

    RankedTensorType resultType = cast<RankedTensorType>(
        OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
            op.getType()));

    if (!lhsType || !resultType)
      return rewriter.notifyMatchFailure(
          op, "Only Ranked Tensor types are supported in TCP");

    auto inputAType =
        dyn_cast<torch::Torch::ValueTensorType>(op.getSelf().getType())
            .getDtype();
    auto outputType =
        dyn_cast<torch::Torch::ValueTensorType>(op.getType()).getDtype();

    Type inputBType = nullptr;
    if (isa<AtenDivScalarOp>(op)) {
      inputBType = adaptor.getOther().getType();

      rhs = convertScalarOperandToTensor(rewriter, op, op.getOther(),
                                         adaptor.getOther(), outputType,
                                         resultType.getElementType());
      if (!rhs)
        return rewriter.notifyMatchFailure(op, "Unsupported rhs data type");
    } else {
      inputBType =
          dyn_cast<torch::Torch::ValueTensorType>(op.getOther().getType())
              .getDtype();
      rhs = torch_to_tcp::castTensorToDtype(rewriter, inputBType, outputType,
                                            rhs, resultType.getElementType());
    }
    lhs = torch_to_tcp::castTensorToDtype(rewriter, inputAType, outputType, lhs,
                                          resultType.getElementType());
    std::tie(lhs, rhs) =
        torch_to_tcp::broadcastToMatchShape(rewriter, lhs, rhs);

    if (isa<mlir::FloatType>(outputType)) {
      rewriter.replaceOpWithNewOp<tcp::DivFOp>(op, resultType, lhs, rhs);
    } else {
      auto in1IntType = cast<mlir::IntegerType>(inputAType);
      auto in2IntType = cast<mlir::IntegerType>(inputBType);
      auto outIntType = cast<mlir::IntegerType>(outputType);
      if ((in1IntType.getSignedness() != in2IntType.getSignedness()) ||
          (in1IntType.getSignedness() != outIntType.getSignedness()))
        return rewriter.notifyMatchFailure(op,
                                           "Mixed signedness not supported");
      if (in1IntType.getSignedness() ==
          mlir::IntegerType::SignednessSemantics::Signless)
        return rewriter.notifyMatchFailure(
            op, "Signless division not supported in TCP");

      if (outIntType.getSignedness() ==
          mlir::IntegerType::SignednessSemantics::Unsigned)
        rewriter.replaceOpWithNewOp<tcp::DivUIOp>(op, resultType, lhs, rhs,
                                                  tcp::RoundingMode::Trunc);
      else
        rewriter.replaceOpWithNewOp<tcp::DivSIOp>(op, resultType, lhs, rhs,
                                                  tcp::RoundingMode::Trunc);
    }
    return success();
  }
};

class ConvertAtenClampOp : public OpConversionPattern<AtenClampOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenClampOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value input = adaptor.getSelf();
    RankedTensorType inputType = dyn_cast<RankedTensorType>(input.getType());
    if (!inputType)
      return rewriter.notifyMatchFailure(
          op, "Only Ranked Tensor types are supported in TCP");
    auto elementType = inputType.getElementType();
    if (!elementType.isIntOrFloat())
      return rewriter.notifyMatchFailure(
          op, "Input tensor must have integer or floating-point datatype");

    Value minValue = op.getMin();
    Value maxValue = op.getMax();
    if (checkNotNone(rewriter, op, minValue).failed() &&
        checkNotNone(rewriter, op, maxValue).failed()) {
      return rewriter.notifyMatchFailure(
          op, "clamp op requires at least one of min or max");
    }

    auto setMinMaxAttrs = [&](Value value, FloatAttr &floatAttr,
                              IntegerAttr &intAttr) {
      double floatValue;
      int64_t intValue;
      if (matchPattern(value, m_TorchConstantFloat(&floatValue))) {
        if (isa<mlir::FloatType>(elementType))
          floatAttr = rewriter.getF32FloatAttr(floatValue);
        else if (isa<mlir::IntegerType>(elementType))
          intAttr =
              rewriter.getI64IntegerAttr(static_cast<int64_t>(floatValue));
      } else if (matchPattern(value, m_TorchConstantInt(&intValue))) {
        if (isa<mlir::FloatType>(elementType))
          floatAttr = rewriter.getF32FloatAttr(static_cast<float>(intValue));
        else if (isa<mlir::IntegerType>(elementType))
          intAttr = rewriter.getI64IntegerAttr(intValue);
      } else {
        llvm_unreachable("only float or integer constants are supported as min "
                         "/ max values");
      }
    };

    FloatAttr minFloatAttr, maxFloatAttr;
    IntegerAttr minIntAttr, maxIntAttr;
    if (checkNotNone(rewriter, op, minValue).succeeded()) {
      setMinMaxAttrs(minValue, minFloatAttr, minIntAttr);
    }
    if (checkNotNone(rewriter, op, maxValue).succeeded()) {
      setMinMaxAttrs(maxValue, maxFloatAttr, maxIntAttr);
    }

    rewriter.replaceOpWithNewOp<tcp::ClampOp>(op, inputType, input,
                                              minFloatAttr, maxFloatAttr,
                                              minIntAttr, maxIntAttr);
    return success();
  }
};

class ConvertAtenReluOp : public OpConversionPattern<AtenReluOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenReluOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value input = adaptor.getSelf();
    RankedTensorType inputType = dyn_cast<RankedTensorType>(input.getType());

    if (!inputType)
      return rewriter.notifyMatchFailure(
          op, "Only Ranked Tensor types are supported in TCP");

    auto elementType = inputType.getElementType();
    if (!elementType.isIntOrFloat())
      return rewriter.notifyMatchFailure(
          op, "Input tensor must have integer or floating-point datatype");

    FloatAttr minFloatAttr, maxFloatAttr;
    IntegerAttr minIntAttr, maxIntAttr;
    if (isa<mlir::FloatType>(elementType))
      minFloatAttr = rewriter.getF32FloatAttr(0.0f);
    else
      minIntAttr = rewriter.getI64IntegerAttr(0);

    rewriter.replaceOpWithNewOp<tcp::ClampOp>(op, inputType, input,
                                              minFloatAttr, maxFloatAttr,
                                              minIntAttr, maxIntAttr);
    return success();
  }
};

class ConvertAtenSqrtOp : public OpConversionPattern<AtenSqrtOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenSqrtOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value input = adaptor.getSelf();
    RankedTensorType inputType = dyn_cast<RankedTensorType>(input.getType());

    RankedTensorType resultType =
        cast<RankedTensorType>(getTypeConverter()->convertType(op.getType()));

    if (!inputType || !resultType)
      return rewriter.notifyMatchFailure(
          op, "Only Ranked Tensor types are supported in TCP");

    auto elementType = inputType.getElementType();
    if (!elementType.isIntOrFloat())
      return rewriter.notifyMatchFailure(
          op, "Input tensor must have integer or floating-point datatype");

    Value newInput = input;
    if (isa<mlir::IntegerType>(elementType)) {
      auto inputDType =
          dyn_cast<torch::Torch::ValueTensorType>(op.getSelf().getType())
              .getDtype();
      auto outputDType =
          dyn_cast<torch::Torch::ValueTensorType>(op.getType()).getDtype();
      newInput =
          torch_to_tcp::castTensorToDtype(rewriter, inputDType, outputDType,
                                          input, resultType.getElementType());
    }

    rewriter.replaceOpWithNewOp<tcp::SqrtOp>(op, resultType, newInput);

    return success();
  }
};

class ConvertAtenLog1pOp : public OpConversionPattern<AtenLog1pOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenLog1pOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value input = adaptor.getSelf();
    RankedTensorType inputType = dyn_cast<RankedTensorType>(input.getType());

    if (!inputType)
      return rewriter.notifyMatchFailure(
          op, "Only Ranked Tensor types are supported in TCP");

    auto elementType = inputType.getElementType();
    if (!isa<mlir::FloatType>(elementType))
      return rewriter.notifyMatchFailure(
          op, "Only floating-point datatype is supported");

    auto constOp = torch_to_tcp::getConstTensor<float>(
                       rewriter, op, llvm::ArrayRef((float)1.0), {})
                       .value();
    constOp = torch_to_tcp::broadcast0DOr1DToNDAndMatchShape(
        rewriter, constOp, input, elementType);
    auto addOp =
        rewriter.create<tcp::AddOp>(op.getLoc(), inputType, input, constOp);
    rewriter.replaceOpWithNewOp<tcp::LogOp>(op, inputType, addOp);
    return success();
  }
};

template <typename AtenOpT, typename TcpOpT>
class ConvertAtenUnaryIntOrFpOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;

  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value input = adaptor.getSelf();
    RankedTensorType inputType = dyn_cast<RankedTensorType>(input.getType());

    if (!inputType)
      return rewriter.notifyMatchFailure(
          op, "Only Ranked Tensor types are supported in TCP");

    auto elementType = inputType.getElementType();
    if (!elementType.isIntOrFloat())
      return rewriter.notifyMatchFailure(
          op, "Input tensor must have integer or floating-point datatype");

    RankedTensorType resultType = cast<RankedTensorType>(
        OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
            op.getType()));

    rewriter.replaceOpWithNewOp<TcpOpT>(op, resultType, input);
    return success();
  }
};

template <typename AtenOpT, typename TcpOpT>
class ConvertAtenUnaryFpOnlyOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;

  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value input = adaptor.getSelf();
    RankedTensorType inputType = dyn_cast<RankedTensorType>(input.getType());

    if (!inputType)
      return rewriter.notifyMatchFailure(
          op, "Only Ranked Tensor types are supported in TCP");

    if (!isa<mlir::FloatType>(inputType.getElementType()))
      return rewriter.notifyMatchFailure(
          op, "Input tensor must have floating-point datatype");

    rewriter.replaceOpWithNewOp<TcpOpT>(op, inputType, input);
    return success();
  }
};

class ConvertAtenAtan2Op : public OpConversionPattern<AtenAtan2Op> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenAtan2Op op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value lhs = adaptor.getSelf();
    RankedTensorType lhsType = dyn_cast<RankedTensorType>(lhs.getType());

    Value rhs = adaptor.getOther();
    RankedTensorType rhsType = dyn_cast<RankedTensorType>(rhs.getType());

    RankedTensorType resultType =
        cast<RankedTensorType>(getTypeConverter()->convertType(op.getType()));

    if (!lhsType || !rhsType || !resultType)
      return rewriter.notifyMatchFailure(
          op, "Only Ranked Tensor types are supported in TCP");

    if (!isa<mlir::FloatType>(lhsType.getElementType()) ||
        !isa<mlir::FloatType>(rhsType.getElementType()))
      return rewriter.notifyMatchFailure(
          op, "Input tensors must have floating-point datatype");

    auto inputAType =
        dyn_cast<torch::Torch::ValueTensorType>(op.getSelf().getType())
            .getDtype();
    auto inputBType =
        dyn_cast<torch::Torch::ValueTensorType>(op.getOther().getType())
            .getDtype();
    auto outputType =
        dyn_cast<torch::Torch::ValueTensorType>(op.getType()).getDtype();

    rhs = torch_to_tcp::castTensorToDtype(rewriter, inputBType, outputType, rhs,
                                          resultType.getElementType());
    lhs = torch_to_tcp::castTensorToDtype(rewriter, inputAType, outputType, lhs,
                                          resultType.getElementType());
    std::tie(lhs, rhs) =
        torch_to_tcp::broadcastToMatchShape(rewriter, lhs, rhs);

    rewriter.replaceOpWithNewOp<tcp::Atan2Op>(op, resultType, lhs, rhs);
    return success();
  }
};

class ConvertAtenToDtypeOp : public OpConversionPattern<AtenToDtypeOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenToDtypeOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    MLIRContext *context = op.getContext();
    Value input = op.getSelf();
    auto inputType = dyn_cast<torch::Torch::ValueTensorType>(input.getType());
    auto outputType = dyn_cast<torch::Torch::ValueTensorType>(op.getType());
    RankedTensorType resultType =
        cast<RankedTensorType>(getTypeConverter()->convertType(op.getType()));

    if (!inputType || !outputType)
      return rewriter.notifyMatchFailure(
          op, "Expected Input/Output to be ValueTensorType");

    auto elementType = inputType.getDtype();
    if (!elementType.isIntOrFloat())
      return rewriter.notifyMatchFailure(
          op, "Input tensor must have integer or floating-point datatype");

    // The non_blocking arg should be a constant `False`.
    bool nonBlocking;
    if (!matchPattern(op.getNonBlocking(), m_TorchConstantBool(&nonBlocking)) ||
        nonBlocking) {
      return rewriter.notifyMatchFailure(op,
                                         "unimplemented: non_blocking arg must "
                                         "be a constant with False value");
    }

    // The copy arg should be a constant `False`.
    bool copy;
    if (!matchPattern(op.getCopy(), m_TorchConstantBool(&copy)) || copy) {
      return rewriter.notifyMatchFailure(
          op, "unimplemented: copy arg must be a constant with False value");
    }

    // Only `none`, `contiguous` and `preserve` memory_format is supported.
    if (!isa<Torch::NoneType>(op.getMemoryFormat().getType())) {
      int64_t memoryFormat;
      if (!matchPattern(op.getMemoryFormat(),
                        m_TorchConstantInt(&memoryFormat)) ||
          (memoryFormat != torch_upstream::MemoryFormat::Contiguous &&
           memoryFormat != torch_upstream::MemoryFormat::Preserve))
        return rewriter.notifyMatchFailure(
            op, "unimplemented: the memory format should be specified in "
                "an integer constant with none, contiguous or preserve value");
    }

    if (isa<mlir::FloatType>(inputType.getDtype()) &&
        isa<mlir::FloatType>(outputType.getDtype()))
      // FP -> FP
      rewriter.replaceOpWithNewOp<tcp::CastOp>(
          op, resultType, adaptor.getSelf(), SignednessAttr{},
          SignednessAttr{});
    else if (isa<mlir::FloatType>(inputType.getDtype())) {
      // FP -> INT
      if (auto intType = dyn_cast<mlir::IntegerType>(outputType.getDtype()))
        rewriter.replaceOpWithNewOp<tcp::CastOp>(
            op, resultType, adaptor.getSelf(), SignednessAttr{},
            torch_to_tcp::getTcpSignednessAttr(context,
                                               intType.getSignedness()));
      else
        return rewriter.notifyMatchFailure(
            op, "expect output type to be signless/signed/unsigned integer");
    } else if (isa<mlir::FloatType>(outputType.getDtype())) {
      // INT -> FP
      if (auto intType = dyn_cast<mlir::IntegerType>(inputType.getDtype()))
        rewriter.replaceOpWithNewOp<tcp::CastOp>(
            op, resultType, adaptor.getSelf(),
            torch_to_tcp::getTcpSignednessAttr(context,
                                               intType.getSignedness()),
            SignednessAttr{});
      else
        return rewriter.notifyMatchFailure(
            op, "expect input type to be signless/signed/unsigned integer");
    } else {
      // INT -> INT
      auto inIntType = dyn_cast<mlir::IntegerType>(inputType.getDtype());
      auto outIntType = dyn_cast<mlir::IntegerType>(outputType.getDtype());
      if (inIntType && outIntType)
        rewriter.replaceOpWithNewOp<tcp::CastOp>(
            op, resultType, adaptor.getSelf(),
            torch_to_tcp::getTcpSignednessAttr(context,
                                               inIntType.getSignedness()),
            torch_to_tcp::getTcpSignednessAttr(context,
                                               outIntType.getSignedness()));
      else
        return rewriter.notifyMatchFailure(op,
                                           "invalid input/output data type");
    }
    return success();
  }
};

} // namespace

void torch_to_tcp::populateElementwisePatternsAndLegality(
    TypeConverter &typeConverter, RewritePatternSet &patterns,
    ConversionTarget &target, const llvm::StringSet<> &convertTorchOpsSet) {

#define INSERT_ATEN_ELEMENTWISE_OP_PATTERN(AtenOp)                             \
  torch_to_tcp::addPatternIfOpInConvertTorchOpsSet<Convert##AtenOp, AtenOp>(   \
      typeConverter, patterns, target, convertTorchOpsSet)
  INSERT_ATEN_ELEMENTWISE_OP_PATTERN(AtenToDtypeOp);
  INSERT_ATEN_ELEMENTWISE_OP_PATTERN(AtenClampOp);
  INSERT_ATEN_ELEMENTWISE_OP_PATTERN(AtenReluOp);
  INSERT_ATEN_ELEMENTWISE_OP_PATTERN(AtenBatchNormOp);
  INSERT_ATEN_ELEMENTWISE_OP_PATTERN(AtenAtan2Op);
  INSERT_ATEN_ELEMENTWISE_OP_PATTERN(AtenSqrtOp);
  INSERT_ATEN_ELEMENTWISE_OP_PATTERN(AtenLog1pOp);
#undef INSERT_ATEN_ELEMENTWISE_OP_PATTERN

#define INSERT_ATEN_ELEMENTWISE_ADD_SUB_PATTERN(AtenOp, TcpOp)                 \
  torch_to_tcp::addPatternIfOpInConvertTorchOpsSet<                            \
      ConvertAtenAddSubOp<AtenOp, TcpOp>, AtenOp>(typeConverter, patterns,     \
                                                  target, convertTorchOpsSet)
  INSERT_ATEN_ELEMENTWISE_ADD_SUB_PATTERN(AtenAddTensorOp, tcp::AddOp);
  INSERT_ATEN_ELEMENTWISE_ADD_SUB_PATTERN(AtenSubTensorOp, tcp::SubOp);
  INSERT_ATEN_ELEMENTWISE_ADD_SUB_PATTERN(AtenAddScalarOp, tcp::AddOp);
  INSERT_ATEN_ELEMENTWISE_ADD_SUB_PATTERN(AtenSubScalarOp, tcp::SubOp);
#undef INSERT_ATEN_ELEMENTWISE_ADD_SUB_PATTERN

#define INSERT_ATEN_ELEMENTWISE_MUL_DIV_PATTERN(ConvertAtenOpPattern, AtenOp)  \
  torch_to_tcp::addPatternIfOpInConvertTorchOpsSet<                            \
      ConvertAtenOpPattern<AtenOp>, AtenOp>(typeConverter, patterns, target,   \
                                            convertTorchOpsSet)
  INSERT_ATEN_ELEMENTWISE_MUL_DIV_PATTERN(ConvertAtenMulOp, AtenMulTensorOp);
  INSERT_ATEN_ELEMENTWISE_MUL_DIV_PATTERN(ConvertAtenMulOp, AtenMulScalarOp);
  INSERT_ATEN_ELEMENTWISE_MUL_DIV_PATTERN(ConvertAtenDivOp, AtenDivTensorOp);
  INSERT_ATEN_ELEMENTWISE_MUL_DIV_PATTERN(ConvertAtenDivOp, AtenDivScalarOp);
#undef INSERT_ATEN_ELEMENTWISE_MUL_DIV_PATTERN

// We only convert torch ops with fp inputs here. Hence marking torch ops
// with non-fp inputs as dynamically legal (in Torch dialect) i.e. leave
// them in Torch, to be handled by Torch -> TOSA later. This helps avoid
// legalization errors after Torch -> TCP.
#define INSERT_ATEN_UNARY_FP_ONLY_PATTERN(AtenOp, TcpOp)                       \
  {                                                                            \
    auto isFpOnlyOp = [](AtenOp op) {                                          \
      auto inputTy =                                                           \
          cast<torch::Torch::ValueTensorType>(op.getSelf().getType());         \
      auto inputDTy = inputTy.toBuiltinTensor().getElementType();              \
      return isa<mlir::FloatType>(inputDTy);                                   \
    };                                                                         \
    torch_to_tcp::addPatternIfOpInConvertTorchOpsSet<                          \
        ConvertAtenUnaryFpOnlyOp<AtenOp, TcpOp>, AtenOp>(                      \
        typeConverter, patterns, target, convertTorchOpsSet,                   \
        [&](AtenOp op) { return !isFpOnlyOp(op); });                           \
  }

  INSERT_ATEN_UNARY_FP_ONLY_PATTERN(AtenCeilOp, tcp::CeilOp);
  INSERT_ATEN_UNARY_FP_ONLY_PATTERN(AtenFloorOp, tcp::FloorOp);
  INSERT_ATEN_UNARY_FP_ONLY_PATTERN(AtenRoundOp, tcp::RoundEvenOp);
  INSERT_ATEN_UNARY_FP_ONLY_PATTERN(AtenSigmoidOp, tcp::SigmoidOp);
  INSERT_ATEN_UNARY_FP_ONLY_PATTERN(AtenTanhOp, tcp::TanhOp);
  INSERT_ATEN_UNARY_FP_ONLY_PATTERN(AtenSinOp, tcp::SinOp);
  INSERT_ATEN_UNARY_FP_ONLY_PATTERN(AtenCosOp, tcp::CosOp);
  INSERT_ATEN_UNARY_FP_ONLY_PATTERN(AtenLogOp, tcp::LogOp);
  INSERT_ATEN_UNARY_FP_ONLY_PATTERN(AtenNegOp, tcp::NegOp);
  INSERT_ATEN_UNARY_FP_ONLY_PATTERN(AtenAtanOp, tcp::AtanOp);
#undef INSERT_ATEN_UNARY_FP_ONLY_PATTERN

#define INSERT_ATEN_UNARY_INT_OR_FP_PATTERN(AtenOp, TcpOp)                     \
  torch_to_tcp::addPatternIfOpInConvertTorchOpsSet<                            \
      ConvertAtenUnaryIntOrFpOp<AtenOp, TcpOp>, AtenOp>(                       \
      typeConverter, patterns, target, convertTorchOpsSet)
  INSERT_ATEN_UNARY_INT_OR_FP_PATTERN(AtenAbsOp, tcp::AbsOp);
#undef INSERT_ATEN_UNARY_INT_OR_FP_PATTERN
}

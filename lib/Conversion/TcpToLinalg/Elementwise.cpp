//===------------------------------------------------------------*- C++ -*-===//
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Also available under a BSD-style license. See LICENSE.
//
//===----------------------------------------------------------------------===//

#include "mlir-tcp/Conversion/TcpToLinalg/TcpToLinalg.h"

#include "mlir-tcp/Dialect/IR/TcpDialect.h"
#include "mlir-tcp/Dialect/IR/TcpOps.h"

#include "../PassDetail.h"
#include "PopulatePatterns.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Tensor/Utils/Utils.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Transforms/DialectConversion.h"

using namespace mlir;
using namespace mlir::tcp;

namespace {

Value createElementwiseLinalgGeneric(
    OpBuilder &b, Location loc, ValueRange tensorOperands,
    RankedTensorType resultTensorType,
    function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuilder) {
  auto resultRank = resultTensorType.getRank();

  // In order to populate the resultDimSizes, we only need to look at one of
  // the tensorOperands, since all the operands are expected to have the same
  // shape.
  SmallVector<OpFoldResult> resultDimSizes =
      mlir::tensor::getMixedSizes(b, loc, tensorOperands[0]);

  // Add indexing maps for all the tensor operands and for the result.
  SmallVector<AffineMap> indexingMaps{tensorOperands.size() + 1,
                                      b.getMultiDimIdentityMap(resultRank)};

  SmallVector<utils::IteratorType> iteratorTypes(resultRank,
                                                 utils::IteratorType::parallel);

  Value emptyTensor = b.create<tensor::EmptyOp>(
      loc, resultDimSizes, resultTensorType.getElementType());
  return b
      .create<linalg::GenericOp>(loc, emptyTensor.getType(), tensorOperands,
                                 emptyTensor, indexingMaps, iteratorTypes,
                                 bodyBuilder)
      .getResult(0);
}

FailureOr<Value>
createLinalgPayloadForElementwiseOp(Operation *op,
                                    RankedTensorType resultTensorType,
                                    OpBuilder &b, ValueRange payloadArgs) {
  Location loc = op->getLoc();
  auto elemType = resultTensorType.getElementType();

  if (isa<TanhOp>(op))
    return {b.create<math::TanhOp>(loc, payloadArgs[0])};

  if (auto clampOp = dyn_cast<ClampOp>(op)) {
    // This implementation always performs the max followed by min.
    // TODO: Is this going to work for degenerative floating point numbers?
    Value result = payloadArgs[0];
    if (isa<mlir::FloatType>(elemType)) {
      auto minFloat = clampOp.getMinFloat();
      auto maxFloat = clampOp.getMaxFloat();
      if (minFloat)
        result = b.create<arith::MaximumFOp>(
            loc, result,
            b.create<arith::ConstantFloatOp>(loc, *minFloat, b.getF32Type()));
      if (maxFloat)
        result = b.create<arith::MinimumFOp>(
            loc, result,
            b.create<arith::ConstantFloatOp>(loc, *maxFloat, b.getF32Type()));
    } else if (isa<mlir::IntegerType>(elemType)) {
      auto minInt = clampOp.getMinInt();
      auto maxInt = clampOp.getMaxInt();
      if (minInt)
        result = b.create<arith::MaxSIOp>(
            loc, result,
            b.create<arith::ConstantIntOp>(loc, *minInt, b.getIntegerType(64)));
      if (maxInt)
        result = b.create<arith::MinSIOp>(
            loc, result,
            b.create<arith::ConstantIntOp>(loc, *maxInt, b.getIntegerType(64)));
    } else {
      llvm_unreachable("unsupported element type in "
                       "createLinalgPayloadForElementwiseOp for tcp.clamp");
    }
    return result;
  }

  if (isa<SigmoidOp>(op)) {
    auto elemType = resultTensorType.getElementType();
    auto one = b.create<arith::ConstantOp>(loc, FloatAttr::get(elemType, 1));
    auto negate = b.create<arith::NegFOp>(loc, payloadArgs[0]);
    auto exp = b.create<math::ExpOp>(loc, negate);
    auto sum = b.create<arith::AddFOp>(loc, exp, one);
    return {b.create<arith::DivFOp>(loc, one, sum)};
  }

  if (isa<SqrtOp>(op)) {
    return {b.create<math::SqrtOp>(loc, payloadArgs[0])};
  }

  if (isa<CeilOp>(op)) {
    return {b.create<math::CeilOp>(loc, payloadArgs[0])};
  }

  if (isa<FloorOp>(op)) {
    return {b.create<math::FloorOp>(loc, payloadArgs[0])};
  }

  if (isa<RoundOp>(op)) {
    return {b.create<math::RoundOp>(loc, payloadArgs[0])};
  }

  if (isa<RoundEvenOp>(op)) {
    return {b.create<math::RoundEvenOp>(loc, payloadArgs[0])};
  }

  if (isa<SinOp>(op)) {
    return {b.create<math::SinOp>(loc, payloadArgs[0])};
  }

  if (isa<CosOp>(op)) {
    return {b.create<math::CosOp>(loc, payloadArgs[0])};
  }

  if (isa<AbsOp>(op)) {
    if (isa<mlir::FloatType>(elemType))
      return {b.create<math::AbsFOp>(loc, payloadArgs[0])};
    else if (isa<mlir::IntegerType>(elemType))
      return {b.create<math::AbsIOp>(loc, payloadArgs[0])};
    else
      llvm_unreachable("unsupported element type in "
                       "createLinalgPayloadForElementwiseOp for tcp.abs");
  }

  if (isa<LogOp>(op)) {
    return {b.create<math::LogOp>(loc, payloadArgs[0])};
  }

  if (isa<NegOp>(op)) {
    return {b.create<arith::NegFOp>(loc, payloadArgs[0])};
  }

  if (isa<AtanOp>(op)) {
    return {b.create<math::AtanOp>(loc, payloadArgs[0])};
  }

  if (isa<AddOp>(op)) {
    if (isa<mlir::FloatType>(elemType))
      return {b.create<arith::AddFOp>(loc, payloadArgs[0], payloadArgs[1])};
    else if (isa<mlir::IntegerType>(elemType))
      return {b.create<arith::AddIOp>(loc, payloadArgs[0], payloadArgs[1])};
    else
      llvm_unreachable("unsupported element type in "
                       "createLinalgPayloadForElementwiseOp for tcp.add");
  }

  if (isa<SubOp>(op)) {
    if (isa<mlir::FloatType>(elemType))
      return {b.create<arith::SubFOp>(loc, payloadArgs[0], payloadArgs[1])};
    else if (isa<mlir::IntegerType>(elemType))
      return {b.create<arith::SubIOp>(loc, payloadArgs[0], payloadArgs[1])};
    else
      llvm_unreachable("unsupported element type in "
                       "createLinalgPayloadForElementwiseOp fot tcp.sub");
  }

  if (isa<MulOp>(op)) {
    if (isa<mlir::FloatType>(elemType))
      return {b.create<arith::MulFOp>(loc, payloadArgs[0], payloadArgs[1])};
    else if (isa<mlir::IntegerType>(elemType))
      return {b.create<arith::MulIOp>(loc, payloadArgs[0], payloadArgs[1])};
    else
      llvm_unreachable("unsupported element type in "
                       "createLinalgPayloadForElementwiseOp for tcp.mul");
  }

  if (isa<DivFOp>(op)) {
    if (isa<mlir::FloatType>(elemType))
      return {b.create<arith::DivFOp>(loc, payloadArgs[0], payloadArgs[1])};
    else
      llvm_unreachable("unsupported element type in "
                       "createLinalgPayloadForElementwiseOp for tcp.divf");
  }

  if (auto divOp = dyn_cast<DivSIOp>(op)) {
    if (!isa<mlir::IntegerType>(elemType))
      llvm_unreachable("unsupported element type in "
                       "createLinalgPayloadForElementwiseOp for tcp.divsi");
    if (divOp.getRoundingMode() == RoundingMode::Trunc)
      return {b.create<arith::DivSIOp>(loc, payloadArgs[0], payloadArgs[1])};
    else if (divOp.getRoundingMode() == RoundingMode::Ceil)
      return {
          b.create<arith::CeilDivSIOp>(loc, payloadArgs[0], payloadArgs[1])};
    else
      return {
          b.create<arith::FloorDivSIOp>(loc, payloadArgs[0], payloadArgs[1])};
  }

  if (auto divOp = dyn_cast<DivUIOp>(op)) {
    if (!isa<mlir::IntegerType>(elemType))
      llvm_unreachable("unsupported element type in "
                       "createLinalgPayloadForElementwiseOp for tcp.divui");
    if (divOp.getRoundingMode() == RoundingMode::Trunc ||
        divOp.getRoundingMode() == RoundingMode::Floor)
      return {b.create<arith::DivUIOp>(loc, payloadArgs[0], payloadArgs[1])};
    else
      return {
          b.create<arith::CeilDivUIOp>(loc, payloadArgs[0], payloadArgs[1])};
  }

  if (isa<Atan2Op>(op)) {
    if (isa<mlir::FloatType>(elemType))
      return {b.create<math::Atan2Op>(loc, payloadArgs[0], payloadArgs[1])};
    else
      llvm_unreachable("unsupported element type in "
                       "createLinalgPayloadForElementwiseOp for tcp.atan2");
  }

  if (auto castOp = dyn_cast<CastOp>(op)) {
    auto inputType =
        dyn_cast<RankedTensorType>(castOp.getIn().getType()).getElementType();
    auto outputType = resultTensorType.getElementType();

    if (inputType.getIntOrFloatBitWidth() ==
            outputType.getIntOrFloatBitWidth() &&
        ((!castOp.getInIntSignedness() && !castOp.getOutIntSignedness()) ||
         (castOp.getInIntSignedness() && castOp.getOutIntSignedness() &&
          castOp.getInIntSignedness().value() ==
              castOp.getOutIntSignedness().value())))
      // check for same type
      return {payloadArgs[0]};
    else if (outputType.isInteger(1)) {
      // To I1 (Bool) type
      Value cstZero =
          b.create<arith::ConstantOp>(loc, b.getZeroAttr(inputType));
      if (isa<mlir::FloatType>(inputType)) {
        return {b.create<arith::CmpFOp>(loc, arith::CmpFPredicate::UNE,
                                        payloadArgs[0], cstZero)};
      } else if (isa<mlir::IntegerType>(inputType)) {
        return {b.create<arith::CmpIOp>(loc, arith::CmpIPredicate::ne,
                                        payloadArgs[0], cstZero)};
      }
    } else if (isa<mlir::FloatType>(outputType)) {
      // TO FP type
      // FP -> FP
      if (dyn_cast<mlir::FloatType>(inputType)) {
        if (inputType.getIntOrFloatBitWidth() >
            outputType.getIntOrFloatBitWidth())
          return {b.create<arith::TruncFOp>(loc, outputType, payloadArgs[0])};
        return {b.create<arith::ExtFOp>(loc, outputType, payloadArgs[0])};
      }
      // INT -> FP
      else if (dyn_cast<mlir::IntegerType>(inputType)) {
        // Signless or Unsigned INT to FP
        // Curently, signless is only for i1 (bool) case,
        // which has been handeled above
        if (castOp.getInIntSignedness().value() == Signedness::Signless ||
            castOp.getInIntSignedness().value() == Signedness::Unsigned)
          return {b.create<arith::UIToFPOp>(loc, outputType, payloadArgs[0])};
        // Signed INT to FP
        else if (castOp.getInIntSignedness().value() == Signedness::Signed)
          return {b.create<arith::SIToFPOp>(loc, outputType, payloadArgs[0])};
      }
    } else if (isa<mlir::IntegerType>(outputType)) {
      // TO INT type
      // FP -> INT
      if (dyn_cast<mlir::FloatType>(inputType)) {
        // FP to Signless or Unsigned INT
        if (castOp.getOutIntSignedness().value() == Signedness::Signless ||
            castOp.getOutIntSignedness().value() == Signedness::Unsigned)
          return {b.create<arith::FPToUIOp>(loc, outputType, payloadArgs[0])};
        // FP to Signed INT
        else if (castOp.getOutIntSignedness().value() == Signedness::Signed)
          return {b.create<arith::FPToSIOp>(loc, outputType, payloadArgs[0])};
      }
      // INT -> INT
      if (dyn_cast<mlir::IntegerType>(inputType)) {
        if (inputType.getIntOrFloatBitWidth() >
            outputType.getIntOrFloatBitWidth())
          return {b.create<arith::TruncIOp>(loc, outputType, payloadArgs[0])};
        // Signless or Unsigned INT extension
        if (castOp.getInIntSignedness().value() == Signedness::Signless ||
            castOp.getInIntSignedness().value() == Signedness::Unsigned)
          return {b.create<arith::ExtUIOp>(loc, outputType, payloadArgs[0])};
        // Signed INT extension
        else if (castOp.getInIntSignedness().value() == Signedness::Signed)
          return {b.create<arith::ExtSIOp>(loc, outputType, payloadArgs[0])};
      }
    } else
      llvm_unreachable("unsupported element type in "
                       "createLinalgPayloadForElementwiseOp for tcp.cast");
  }

  return op->emitError(
      "unimplemented lowering in createLinalgPayloadForElementwiseOp");
}

template <typename TcpOpT>
class ConvertElementwiseOp : public OpConversionPattern<TcpOpT> {
public:
  using OpConversionPattern<TcpOpT>::OpConversionPattern;
  using OpAdaptor = typename TcpOpT::Adaptor;

  LogicalResult
  matchAndRewrite(TcpOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    auto resultTensorType = cast<RankedTensorType>(
        OpConversionPattern<TcpOpT>::getTypeConverter()->convertType(
            op->getResult(0).getType()));
    auto tensorOperands = llvm::to_vector<6>(
        llvm::make_filter_range(adaptor.getOperands(), [](Value v) {
          return isa<RankedTensorType>(v.getType());
        }));

    // Create Linalg payload
    auto bodyBuilder = [&](OpBuilder &b, Location loc, ValueRange payloadArgs) {
      FailureOr<Value> result = createLinalgPayloadForElementwiseOp(
          op, resultTensorType, b, payloadArgs);
      // TODO: Check for failure once GenericOp::build supports a body builder
      // that can return a LogicalResult.
      b.create<linalg::YieldOp>(loc, *result);
    };

    Value generic = createElementwiseLinalgGeneric(
        rewriter, loc, tensorOperands, resultTensorType, bodyBuilder);
    rewriter.replaceOp(op, generic);
    return success();
  }
};

} // namespace

void mlir::TcpToLinalg::populateElementwisePatternsAndLegality(
    TypeConverter &typeConverter, RewritePatternSet &patterns,
    ConversionTarget &target) {
  MLIRContext *context = patterns.getContext();

#define INSERT_TCP_TO_LINALG_PATTERN(TcpOp)                                    \
  target.addIllegalOp<TcpOp>();                                                \
  patterns.add<ConvertElementwiseOp<TcpOp>>(typeConverter, context);
  INSERT_TCP_TO_LINALG_PATTERN(AddOp);
  INSERT_TCP_TO_LINALG_PATTERN(ClampOp);
  INSERT_TCP_TO_LINALG_PATTERN(MulOp);
  INSERT_TCP_TO_LINALG_PATTERN(DivFOp);
  INSERT_TCP_TO_LINALG_PATTERN(DivSIOp);
  INSERT_TCP_TO_LINALG_PATTERN(DivUIOp);
  INSERT_TCP_TO_LINALG_PATTERN(SubOp);
  INSERT_TCP_TO_LINALG_PATTERN(TanhOp);
  INSERT_TCP_TO_LINALG_PATTERN(SigmoidOp);
  INSERT_TCP_TO_LINALG_PATTERN(SqrtOp);
  INSERT_TCP_TO_LINALG_PATTERN(CeilOp);
  INSERT_TCP_TO_LINALG_PATTERN(FloorOp);
  INSERT_TCP_TO_LINALG_PATTERN(RoundOp);
  INSERT_TCP_TO_LINALG_PATTERN(RoundEvenOp);
  INSERT_TCP_TO_LINALG_PATTERN(SinOp);
  INSERT_TCP_TO_LINALG_PATTERN(CosOp);
  INSERT_TCP_TO_LINALG_PATTERN(AbsOp);
  INSERT_TCP_TO_LINALG_PATTERN(LogOp);
  INSERT_TCP_TO_LINALG_PATTERN(NegOp);
  INSERT_TCP_TO_LINALG_PATTERN(AtanOp);
  INSERT_TCP_TO_LINALG_PATTERN(Atan2Op);
  INSERT_TCP_TO_LINALG_PATTERN(CastOp);
#undef INSERT_TCP_TO_LINALG_PATTERN
}

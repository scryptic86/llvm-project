//===- TosaToLinalg.cpp - Lowering Tosa to Linalg Dialect -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// These rewriters lower from the Tosa to the Linalg dialect.
//
//===----------------------------------------------------------------------===//

#include "mlir/Conversion/TosaToLinalg/TosaToLinalg.h"
#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Tosa/IR/TosaOps.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#include <numeric>

using namespace mlir;

static SmallVector<StringRef> getNParallelLoopsAttrs(unsigned nParallelLoops) {
  return SmallVector<StringRef>(nParallelLoops, getParallelIteratorTypeName());
}

template <typename T>
static mlir::ConstantOp
createConstFromIntAttribute(Operation *op, std::string attrName,
                            Type requiredAttrType, OpBuilder &rewriter) {
  auto castedN = static_cast<T>(
      op->getAttr(attrName).cast<IntegerAttr>().getValue().getSExtValue());
  return rewriter.create<mlir::ConstantOp>(
      op->getLoc(), IntegerAttr::get(requiredAttrType, castedN));
}

template <typename T>
static void getValuesFromIntArrayAttribute(ArrayAttr attr,
                                           SmallVector<T> &arrayValues) {
  for (Attribute val : attr.getValue()) {
    arrayValues.push_back(val.cast<IntegerAttr>().getValue().getSExtValue());
  }
}

template <typename T, typename P>
static mlir::SelectOp clampHelper(Location loc, Value arg, mlir::ConstantOp min,
                                  mlir::ConstantOp max, P pred,
                                  OpBuilder &rewriter) {
  auto smallerThanMin = rewriter.create<T>(loc, pred, arg, min);
  auto minOrArg =
      rewriter.create<mlir::SelectOp>(loc, smallerThanMin, min, arg);
  auto largerThanMax = rewriter.create<T>(loc, pred, max, arg);
  return rewriter.create<mlir::SelectOp>(loc, largerThanMax, max, minOrArg);
}

static mlir::Value applyPad(Location loc, Value input, ArrayRef<int64_t> pad,
                            Attribute padAttr, OpBuilder &rewriter) {
  // Input should be padded if necessary.
  if (llvm::all_of(pad, [](int64_t p) { return p == 0; }))
    return input;

  ShapedType inputTy = input.getType().cast<ShapedType>();
  Type inputETy = inputTy.getElementType();
  auto inputShape = inputTy.getShape();

  assert((inputShape.size() * 2) == pad.size());

  SmallVector<int64_t, 4> paddedShape;
  SmallVector<OpFoldResult, 8> lowIndices;
  SmallVector<OpFoldResult, 8> highIndices;
  for (int i = 0, s = inputShape.size(); i < s; i++) {
    auto lowPad = pad[i * 2];
    auto highPad = pad[i * 2 + 1];
    paddedShape.push_back(inputShape[i] + highPad + lowPad);
    lowIndices.push_back(rewriter.getIndexAttr(lowPad));
    highIndices.push_back(rewriter.getIndexAttr(highPad));
  }

  Value padValue = rewriter.create<ConstantOp>(loc, padAttr);

  return linalg::PadTensorOp::createPadScalarOp(
             RankedTensorType::get(paddedShape, inputETy), input, padValue,
             lowIndices, highIndices, loc, rewriter)
      .result();
}

static Value
createLinalgBodyCalculationForElementwiseOp(Operation *op, ValueRange args,
                                            ArrayRef<Type> resultTypes,
                                            PatternRewriter &rewriter) {
  Location loc = op->getLoc();
  auto elementTy =
      op->getOperand(0).getType().cast<ShapedType>().getElementType();

  // tosa::AbsOp
  if (isa<tosa::AbsOp>(op) && elementTy.isa<FloatType>())
    return rewriter.create<mlir::AbsFOp>(loc, resultTypes, args);

  // tosa::AddOp
  if (isa<tosa::AddOp>(op) && elementTy.isa<FloatType>())
    return rewriter.create<mlir::AddFOp>(loc, resultTypes, args);

  if (isa<tosa::AddOp>(op) && elementTy.isa<IntegerType>())
    return rewriter.create<mlir::AddIOp>(loc, resultTypes, args);

  // tosa::SubOp
  if (isa<tosa::SubOp>(op) && elementTy.isa<FloatType>())
    return rewriter.create<mlir::SubFOp>(loc, resultTypes, args);

  if (isa<tosa::SubOp>(op) && elementTy.isa<IntegerType>())
    return rewriter.create<mlir::SubIOp>(loc, resultTypes, args);

  // tosa::MulOp
  if (isa<tosa::MulOp>(op) && elementTy.isa<FloatType>()) {
    if (dyn_cast<tosa::MulOp>(op).shift() != 0) {
      (void)rewriter.notifyMatchFailure(op,
                                        "Cannot have shift value for float");
      return nullptr;
    }
    return rewriter.create<mlir::MulFOp>(loc, resultTypes, args);
  }

  // tosa::ReciprocalOp
  if (isa<tosa::ReciprocalOp>(op) && elementTy.isa<FloatType>()) {
    auto one =
        rewriter.create<mlir::ConstantOp>(loc, FloatAttr::get(elementTy, 1));
    return rewriter.create<mlir::DivFOp>(loc, resultTypes, one, args[0]);
  }

  if (isa<tosa::MulOp>(op) && elementTy.isa<IntegerType>()) {
    Value a = args[0];
    Value b = args[1];
    auto shift =
        op->getAttr("shift").cast<IntegerAttr>().getValue().getSExtValue();
    if (shift > 0) {
      auto shiftConst =
          rewriter.create<ConstantIntOp>(loc, shift, /*bitwidth=*/8);
      if (!a.getType().isInteger(32))
        a = rewriter.create<SignExtendIOp>(loc, rewriter.getI32Type(), a);

      if (!b.getType().isInteger(32))
        b = rewriter.create<SignExtendIOp>(loc, rewriter.getI32Type(), b);

      auto result = rewriter.create<tosa::ApplyScaleOp>(
          loc, rewriter.getI32Type(), a, b, shiftConst,
          rewriter.getBoolAttr(false));

      if (elementTy.isInteger(32))
        return result;

      return rewriter.create<TruncateIOp>(loc, elementTy, result);
    }

    int aWidth = a.getType().getIntOrFloatBitWidth();
    int bWidth = b.getType().getIntOrFloatBitWidth();
    int cWidth = resultTypes[0].getIntOrFloatBitWidth();

    if (aWidth < cWidth)
      a = rewriter.create<SignExtendIOp>(loc, resultTypes[0], a);
    if (bWidth < cWidth)
      b = rewriter.create<SignExtendIOp>(loc, resultTypes[0], b);

    return rewriter.create<mlir::MulIOp>(loc, resultTypes, a, b);
  }

  // tosa::NegateOp
  if (isa<tosa::NegateOp>(op) && elementTy.isa<FloatType>())
    return rewriter.create<mlir::NegFOp>(loc, resultTypes, args);

  if (isa<tosa::NegateOp>(op) && elementTy.isa<IntegerType>() &&
      !cast<tosa::NegateOp>(op).quantization_info()) {
    auto constant =
        rewriter.create<ConstantOp>(loc, IntegerAttr::get(elementTy, 0));
    return rewriter.create<SubIOp>(loc, resultTypes, constant, args[0]);
  }

  if (isa<tosa::NegateOp>(op) && elementTy.isa<IntegerType>() &&
      cast<tosa::NegateOp>(op).quantization_info()) {
    auto quantizationInfo = cast<tosa::NegateOp>(op).quantization_info();
    int32_t inputBitWidth = elementTy.getIntOrFloatBitWidth();
    int64_t inZp =
        quantizationInfo.getValue().input_zp().getValue().getSExtValue();
    int64_t outZp =
        quantizationInfo.getValue().output_zp().getValue().getSExtValue();

    // Compute the maximum value that can occur in the intermediate buffer.
    int64_t zpAdd = inZp + outZp;
    int64_t maxValue = APInt::getSignedMaxValue(inputBitWidth).getSExtValue() +
                       std::abs(zpAdd) + 1;

    // Convert that maximum value into the maximum bitwidth needed to represent
    // it. We assume 48-bit numbers may be supported further in the pipeline.
    int intermediateBitWidth = 64;
    if (maxValue <= APInt::getSignedMaxValue(16).getSExtValue()) {
      intermediateBitWidth = 16;
    } else if (maxValue <= APInt::getSignedMaxValue(32).getSExtValue()) {
      intermediateBitWidth = 32;
    } else if (maxValue <= APInt::getSignedMaxValue(48).getSExtValue()) {
      intermediateBitWidth = 48;
    }

    Type intermediateType = rewriter.getIntegerType(intermediateBitWidth);
    Value zpAddValue = rewriter.create<ConstantOp>(
        loc, rewriter.getIntegerAttr(intermediateType, zpAdd));

    // The negation can be applied by doing:
    //  outputValue = inZp + outZp - inputValue
    auto ext = rewriter.create<SignExtendIOp>(loc, intermediateType, args[0]);
    auto sub = rewriter.create<SubIOp>(loc, zpAddValue, ext);

    // Clamp to the negation range.
    auto min = rewriter.create<ConstantOp>(
        loc, rewriter.getIntegerAttr(
                 intermediateType,
                 APInt::getSignedMinValue(inputBitWidth).getSExtValue()));
    auto max = rewriter.create<ConstantOp>(
        loc, rewriter.getIntegerAttr(
                 intermediateType,
                 APInt::getSignedMaxValue(inputBitWidth).getSExtValue()));
    auto clamp = clampHelper<mlir::CmpIOp>(loc, sub, min, max,
                                           CmpIPredicate::slt, rewriter);

    // Truncate to the final value.
    return rewriter.create<TruncateIOp>(loc, elementTy, clamp);
  }

  // tosa::BitwiseAndOp
  if (isa<tosa::BitwiseAndOp>(op) && elementTy.isa<IntegerType>())
    return rewriter.create<mlir::AndOp>(loc, resultTypes, args);

  // tosa::BitwiseOrOp
  if (isa<tosa::BitwiseOrOp>(op) && elementTy.isa<IntegerType>())
    return rewriter.create<mlir::OrOp>(loc, resultTypes, args);

  // tosa::BitwiseNotOp
  if (isa<tosa::BitwiseNotOp>(op) && elementTy.isa<IntegerType>()) {
    auto allOnesAttr = rewriter.getIntegerAttr(
        elementTy, APInt::getAllOnesValue(elementTy.getIntOrFloatBitWidth()));
    auto allOnes = rewriter.create<ConstantOp>(loc, allOnesAttr);
    return rewriter.create<mlir::XOrOp>(loc, resultTypes, args[0], allOnes);
  }

  // tosa::BitwiseXOrOp
  if (isa<tosa::BitwiseXorOp>(op) && elementTy.isa<IntegerType>())
    return rewriter.create<mlir::XOrOp>(loc, resultTypes, args);

  // tosa::LogicalLeftShiftOp
  if (isa<tosa::LogicalLeftShiftOp>(op) && elementTy.isa<IntegerType>())
    return rewriter.create<mlir::ShiftLeftOp>(loc, resultTypes, args);

  // tosa::LogicalRightShiftOp
  if (isa<tosa::LogicalRightShiftOp>(op) && elementTy.isa<IntegerType>())
    return rewriter.create<mlir::UnsignedShiftRightOp>(loc, resultTypes, args);

  // tosa::ArithmeticRightShiftOp
  if (isa<tosa::ArithmeticRightShiftOp>(op) && elementTy.isa<IntegerType>()) {
    auto result =
        rewriter.create<mlir::SignedShiftRightOp>(loc, resultTypes, args);
    auto round = op->getAttr("round").cast<BoolAttr>().getValue();
    if (!round) {
      return result;
    }

    Type i1Ty = IntegerType::get(rewriter.getContext(), /*width=*/1);
    auto one =
        rewriter.create<mlir::ConstantOp>(loc, IntegerAttr::get(elementTy, 1));
    auto zero =
        rewriter.create<mlir::ConstantOp>(loc, IntegerAttr::get(elementTy, 0));
    auto i1one =
        rewriter.create<mlir::ConstantOp>(loc, IntegerAttr::get(i1Ty, 1));

    // Checking that input2 != 0
    auto shiftValueGreaterThanZero =
        rewriter.create<mlir::CmpIOp>(loc, CmpIPredicate::sgt, args[1], zero);

    // Checking for the last bit of input1 to be 1
    auto subtract =
        rewriter.create<mlir::SubIOp>(loc, resultTypes, args[1], one);
    auto shifted = rewriter
                       .create<mlir::SignedShiftRightOp>(loc, resultTypes,
                                                         args[0], subtract)
                       ->getResults();
    auto truncated =
        rewriter.create<mlir::TruncateIOp>(loc, i1Ty, shifted, mlir::None);
    auto isInputOdd = rewriter.create<mlir::AndOp>(loc, i1Ty, truncated, i1one);

    auto shouldRound = rewriter.create<mlir::AndOp>(
        loc, i1Ty, shiftValueGreaterThanZero, isInputOdd);
    auto extended =
        rewriter.create<ZeroExtendIOp>(loc, resultTypes, shouldRound);
    return rewriter.create<mlir::AddIOp>(loc, resultTypes, result, extended);
  }

  // tosa::LogicalAnd
  if (isa<tosa::LogicalAndOp>(op) && elementTy.isInteger(1))
    return rewriter.create<mlir::AndOp>(loc, resultTypes, args);

  // tosa::LogicalNot
  if (isa<tosa::LogicalNotOp>(op) && elementTy.isInteger(1)) {
    auto one = rewriter.create<mlir::ConstantOp>(
        loc, rewriter.getIntegerAttr(elementTy, 1));
    return rewriter.create<mlir::XOrOp>(loc, resultTypes, args[0], one);
  }

  // tosa::LogicalOr
  if (isa<tosa::LogicalOrOp>(op) && elementTy.isInteger(1))
    return rewriter.create<mlir::OrOp>(loc, resultTypes, args);

  // tosa::LogicalXor
  if (isa<tosa::LogicalXorOp>(op) && elementTy.isInteger(1))
    return rewriter.create<mlir::XOrOp>(loc, resultTypes, args);

  // tosa::PowOp
  if (isa<tosa::PowOp>(op) && elementTy.isa<FloatType>())
    return rewriter.create<mlir::math::PowFOp>(loc, resultTypes, args);

  // tosa::RsqrtOp
  if (isa<tosa::RsqrtOp>(op) && elementTy.isa<FloatType>())
    return rewriter.create<mlir::math::RsqrtOp>(loc, resultTypes, args);

  // tosa::LogOp
  if (isa<tosa::LogOp>(op) && elementTy.isa<FloatType>())
    return rewriter.create<mlir::math::LogOp>(loc, resultTypes, args);

  // tosa::ExpOp
  if (isa<tosa::ExpOp>(op) && elementTy.isa<FloatType>())
    return rewriter.create<mlir::math::ExpOp>(loc, resultTypes, args);

  // tosa::TanhOp
  if (isa<tosa::TanhOp>(op) && elementTy.isa<FloatType>())
    return rewriter.create<mlir::math::TanhOp>(loc, resultTypes, args);

  // tosa::GreaterOp
  if (isa<tosa::GreaterOp>(op) && elementTy.isa<FloatType>())
    return rewriter.create<mlir::CmpFOp>(loc, CmpFPredicate::OGT, args[0],
                                         args[1]);

  if (isa<tosa::GreaterOp>(op) && elementTy.isSignlessInteger())
    return rewriter.create<mlir::CmpIOp>(loc, CmpIPredicate::sgt, args[0],
                                         args[1]);

  // tosa::GreaterEqualOp
  if (isa<tosa::GreaterEqualOp>(op) && elementTy.isa<FloatType>())
    return rewriter.create<mlir::CmpFOp>(loc, CmpFPredicate::OGE, args[0],
                                         args[1]);

  if (isa<tosa::GreaterEqualOp>(op) && elementTy.isSignlessInteger())
    return rewriter.create<mlir::CmpIOp>(loc, CmpIPredicate::sge, args[0],
                                         args[1]);

  // tosa::EqualOp
  if (isa<tosa::EqualOp>(op) && elementTy.isa<FloatType>())
    return rewriter.create<mlir::CmpFOp>(loc, CmpFPredicate::OEQ, args[0],
                                         args[1]);

  if (isa<tosa::EqualOp>(op) && elementTy.isSignlessInteger())
    return rewriter.create<mlir::CmpIOp>(loc, CmpIPredicate::eq, args[0],
                                         args[1]);

  // tosa::SelectOp
  if (isa<tosa::SelectOp>(op)) {
    elementTy = op->getOperand(1).getType().cast<ShapedType>().getElementType();
    if (elementTy.isa<FloatType>() || elementTy.isa<IntegerType>())
      return rewriter.create<mlir::SelectOp>(loc, args[0], args[1], args[2]);
  }

  // tosa::MaximumOp
  if (isa<tosa::MaximumOp>(op) && elementTy.isa<FloatType>()) {
    auto predicate = rewriter.create<mlir::CmpFOp>(loc, CmpFPredicate::OGT,
                                                   args[0], args[1]);
    return rewriter.create<mlir::SelectOp>(loc, predicate, args[0], args[1]);
  }

  if (isa<tosa::MaximumOp>(op) && elementTy.isSignlessInteger()) {
    auto predicate = rewriter.create<mlir::CmpIOp>(loc, CmpIPredicate::sgt,
                                                   args[0], args[1]);
    return rewriter.create<mlir::SelectOp>(loc, predicate, args[0], args[1]);
  }

  // tosa::MinimumOp
  if (isa<tosa::MinimumOp>(op) && elementTy.isa<FloatType>()) {
    auto predicate = rewriter.create<mlir::CmpFOp>(loc, CmpFPredicate::OLT,
                                                   args[0], args[1]);
    return rewriter.create<mlir::SelectOp>(loc, predicate, args[0], args[1]);
  }

  if (isa<tosa::MinimumOp>(op) && elementTy.isSignlessInteger()) {
    auto predicate = rewriter.create<mlir::CmpIOp>(loc, CmpIPredicate::slt,
                                                   args[0], args[1]);
    return rewriter.create<mlir::SelectOp>(loc, predicate, args[0], args[1]);
  }

  // tosa::CeilOp
  if (isa<tosa::CeilOp>(op) && elementTy.isa<FloatType>())
    return rewriter.create<mlir::CeilFOp>(loc, resultTypes, args);

  // tosa::FloorOp
  if (isa<tosa::FloorOp>(op) && elementTy.isa<FloatType>())
    return rewriter.create<mlir::FloorFOp>(loc, resultTypes, args);

  // tosa::ClampOp
  if (isa<tosa::ClampOp>(op) && elementTy.isa<FloatType>()) {
    auto min = rewriter.create<mlir::ConstantOp>(loc, elementTy,
                                                 op->getAttr("min_fp"));
    auto max = rewriter.create<mlir::ConstantOp>(loc, elementTy,
                                                 op->getAttr("max_fp"));
    return clampHelper<mlir::CmpFOp>(loc, args[0], min, max, CmpFPredicate::OLT,
                                     rewriter);
  }

  if (isa<tosa::ClampOp>(op) && elementTy.isa<IntegerType>()) {
    auto min = createConstFromIntAttribute<int32_t>(op, "min_int", elementTy,
                                                    rewriter);
    auto max = createConstFromIntAttribute<int32_t>(op, "max_int", elementTy,
                                                    rewriter);
    return clampHelper<mlir::CmpIOp>(loc, args[0], min, max, CmpIPredicate::slt,
                                     rewriter);
  }

  // tosa::ReluNOp
  if (isa<tosa::ReluNOp>(op) && elementTy.isa<FloatType>()) {
    auto zero =
        rewriter.create<mlir::ConstantOp>(loc, FloatAttr::get(elementTy, 0));
    auto n = rewriter.create<mlir::ConstantOp>(loc, elementTy,
                                               op->getAttr("max_fp"));
    return clampHelper<mlir::CmpFOp>(loc, args[0], zero, n, CmpFPredicate::OLT,
                                     rewriter);
  }

  if (isa<tosa::ReluNOp>(op) && elementTy.isa<IntegerType>()) {
    auto zero =
        rewriter.create<mlir::ConstantOp>(loc, IntegerAttr::get(elementTy, 0));
    auto n = createConstFromIntAttribute<int32_t>(op, "max_int", elementTy,
                                                  rewriter);
    return clampHelper<mlir::CmpIOp>(loc, args[0], zero, n, CmpIPredicate::slt,
                                     rewriter);
  }

  // tosa::SigmoidOp
  if (isa<tosa::SigmoidOp>(op) && elementTy.isa<FloatType>()) {
    auto one =
        rewriter.create<mlir::ConstantOp>(loc, FloatAttr::get(elementTy, 1));
    auto negate = rewriter.create<mlir::NegFOp>(loc, resultTypes, args[0]);
    auto exp = rewriter.create<mlir::math::ExpOp>(loc, resultTypes, negate);
    auto added = rewriter.create<mlir::AddFOp>(loc, resultTypes, exp, one);
    return rewriter.create<mlir::DivFOp>(loc, resultTypes, one, added);
  }

  // tosa::CastOp
  if (isa<tosa::CastOp>(op)) {
    Type srcTy = elementTy;
    Type dstTy = resultTypes.front();
    bool bitExtend =
        srcTy.getIntOrFloatBitWidth() < dstTy.getIntOrFloatBitWidth();

    if (srcTy == dstTy)
      return args.front();

    if (srcTy.isa<FloatType>() && dstTy.isa<FloatType>() && bitExtend)
      return rewriter.create<mlir::FPExtOp>(loc, resultTypes, args, mlir::None);

    if (srcTy.isa<FloatType>() && dstTy.isa<FloatType>() && !bitExtend)
      return rewriter.create<mlir::FPTruncOp>(loc, resultTypes, args,
                                              mlir::None);

    // 1-bit integers need to be treated as signless.
    if (srcTy.isInteger(1) && mlir::UIToFPOp::areCastCompatible(srcTy, dstTy))
      return rewriter.create<mlir::UIToFPOp>(loc, resultTypes, args,
                                             mlir::None);

    if (srcTy.isInteger(1) && dstTy.isa<IntegerType>() && bitExtend)
      return rewriter.create<mlir::ZeroExtendIOp>(loc, resultTypes, args,
                                                  mlir::None);

    // All other si-to-fp conversions should be handled by SIToFP.
    if (mlir::SIToFPOp::areCastCompatible(srcTy, dstTy))
      return rewriter.create<mlir::SIToFPOp>(loc, resultTypes, args,
                                             mlir::None);

    // Casting to boolean, floats need to only be checked as not-equal to zero.
    if (srcTy.isa<FloatType>() && dstTy.isInteger(1)) {
      Value zero =
          rewriter.create<ConstantOp>(loc, rewriter.getFloatAttr(srcTy, 0.0));
      return rewriter.create<mlir::CmpFOp>(loc, CmpFPredicate::UNE,
                                           args.front(), zero);
    }

    if (mlir::FPToSIOp::areCastCompatible(srcTy, dstTy))
      return rewriter.create<mlir::FPToSIOp>(loc, resultTypes, args,
                                             mlir::None);

    // Casting to boolean, integers need to only be checked as not-equal to
    // zero.
    if (srcTy.isa<IntegerType>() && dstTy.isInteger(1)) {
      Value zero =
          rewriter.create<ConstantIntOp>(loc, 0, srcTy.getIntOrFloatBitWidth());
      return rewriter.create<mlir::CmpIOp>(loc, CmpIPredicate::ne, args.front(),
                                           zero);
    }

    if (srcTy.isa<IntegerType>() && dstTy.isa<IntegerType>() && bitExtend)
      return rewriter.create<mlir::SignExtendIOp>(loc, resultTypes, args,
                                                  mlir::None);

    if (srcTy.isa<IntegerType>() && dstTy.isa<IntegerType>() && !bitExtend)
      return rewriter.create<mlir::TruncateIOp>(loc, resultTypes, args,
                                                mlir::None);
  }

  (void)rewriter.notifyMatchFailure(
      op, "unhandled op for linalg body calculation for elementwise op");
  return nullptr;
}

static LogicalResult
elementwiseMatchAndRewriteHelper(Operation *operation,
                                 PatternRewriter &rewriter) {
  auto loc = operation->getLoc();
  auto results = operation->getResults();
  auto resultTy = operation->getOperand(0).getType().dyn_cast<ShapedType>();

  if (!resultTy)
    return rewriter.notifyMatchFailure(operation,
                                       "All results must be a shaped type");

  unsigned rank = resultTy.getRank();

  assert(operation->getNumResults() == 1 &&
         "All TOSA elementwise ops should only return a single result.");

  // Construct the indexing maps needed for linalg.generic ops.
  SmallVector<Type> bodyArgTypes;

  for (Value in : operation->getOperands())
    bodyArgTypes.emplace_back(getElementTypeOrSelf(in.getType()));

  SmallVector<Type> opResultTypes;
  SmallVector<Value> initTensors;
  for (auto result : results) {
    auto resultTy = result.getType().template cast<ShapedType>();
    if (!resultTy.hasStaticShape())
      return rewriter.notifyMatchFailure(
          operation,
          "tosa to linalg conversion expects statically shaped tensors");

    initTensors.push_back(rewriter.create<linalg::InitTensorOp>(
        loc, ArrayRef<Value>({}), resultTy.getShape(),
        resultTy.getElementType()));
    opResultTypes.push_back(result.getType());
  }

  auto bodyResultTypes = llvm::to_vector<4>(llvm::map_range(
      initTensors, [](Value v) { return getElementTypeOrSelf(v); }));

  SmallVector<Value, 2> operands;
  SmallVector<AffineMap, 2> indexingMaps;
  indexingMaps.reserve(operation->getNumOperands() + bodyResultTypes.size());

  // Input indexing maps may be broadcasted.
  for (Value operand : operation->getOperands()) {
    ShapedType type = operand.getType().cast<ShapedType>();
    SmallVector<int64_t, 5> newShape;
    SmallVector<AffineExpr, 4> affineExprs;
    newShape.reserve(type.getRank());
    for (auto it : llvm::enumerate(type.getShape())) {
      if (it.value() != 1) {
        newShape.push_back(it.value());
        affineExprs.push_back(
            mlir::getAffineDimExpr(it.index(), rewriter.getContext()));
      }
    }

    if (newShape.size() != rank) {
      operand = rewriter.create<tosa::ReshapeOp>(
          loc, RankedTensorType::get(newShape, type.getElementType()), operand);
    }

    operands.push_back(operand);
    indexingMaps.push_back(AffineMap::get(
        /*dimCount=*/type.getRank(), /*symbolCount=*/0, affineExprs,
        rewriter.getContext()));
  }

  indexingMaps.append(operation->getNumResults(),
                      rewriter.getMultiDimIdentityMap(rank));

  bool didEncounterError = false;
  auto linalgOp = rewriter.create<linalg::GenericOp>(
      loc, opResultTypes, operands, initTensors, indexingMaps,
      getNParallelLoopsAttrs(rank),
      [&](OpBuilder &nestedBuilder, Location nestedLoc, ValueRange blockArgs) {
        Value opResult = createLinalgBodyCalculationForElementwiseOp(
            operation, blockArgs.take_front(operation->getNumOperands()),
            bodyResultTypes, rewriter);
        if (!opResult) {
          didEncounterError = true;
          return;
        }
        nestedBuilder.create<linalg::YieldOp>(loc, opResult);
      });

  if (didEncounterError)
    return failure();

  rewriter.replaceOp(operation, linalgOp->getResults());
  return success();
}

// Returns the constant initial value for a given reduction operation. The
// attribute type varies depending on the element type required.
static Attribute createInitialValueForReduceOp(Operation *op, Type elementTy,
                                               PatternRewriter &rewriter) {
  if (isa<tosa::ReduceSumOp>(op) && elementTy.isa<FloatType>())
    return rewriter.getFloatAttr(elementTy, 0.0);

  if (isa<tosa::ReduceSumOp>(op) && elementTy.isa<IntegerType>())
    return rewriter.getIntegerAttr(elementTy, 0);

  if (isa<tosa::ReduceProdOp>(op) && elementTy.isa<FloatType>())
    return rewriter.getFloatAttr(elementTy, 1.0);

  if (isa<tosa::ReduceProdOp>(op) && elementTy.isa<IntegerType>())
    return rewriter.getIntegerAttr(elementTy, 1);

  if (isa<tosa::ReduceMinOp>(op) && elementTy.isa<FloatType>())
    return rewriter.getFloatAttr(
        elementTy, APFloat::getLargest(
                       elementTy.cast<FloatType>().getFloatSemantics(), false));

  if (isa<tosa::ReduceMinOp>(op) && elementTy.isa<IntegerType>())
    return rewriter.getIntegerAttr(
        elementTy, APInt::getSignedMaxValue(elementTy.getIntOrFloatBitWidth()));

  if (isa<tosa::ReduceMaxOp>(op) && elementTy.isa<FloatType>())
    return rewriter.getFloatAttr(
        elementTy, APFloat::getLargest(
                       elementTy.cast<FloatType>().getFloatSemantics(), true));

  if (isa<tosa::ReduceMaxOp>(op) && elementTy.isa<IntegerType>())
    return rewriter.getIntegerAttr(
        elementTy, APInt::getSignedMinValue(elementTy.getIntOrFloatBitWidth()));

  if (isa<tosa::ReduceAllOp>(op) && elementTy.isInteger(1))
    return rewriter.getIntegerAttr(elementTy, APInt::getAllOnesValue(1));

  if (isa<tosa::ReduceAnyOp>(op) && elementTy.isInteger(1))
    return rewriter.getIntegerAttr(elementTy, APInt::getNullValue(1));

  if (isa<tosa::ArgMaxOp>(op) && elementTy.isa<FloatType>())
    return rewriter.getFloatAttr(
        elementTy, APFloat::getLargest(
                       elementTy.cast<FloatType>().getFloatSemantics(), true));

  if (isa<tosa::ArgMaxOp>(op) && elementTy.isa<IntegerType>())
    return rewriter.getIntegerAttr(
        elementTy, APInt::getSignedMinValue(elementTy.getIntOrFloatBitWidth()));

  return {};
}

// Creates the body calculation for a reduction. The operations vary depending
// on the input type.
static Value createLinalgBodyCalculationForReduceOp(Operation *op,
                                                    ValueRange args,
                                                    Type elementTy,
                                                    PatternRewriter &rewriter) {
  Location loc = op->getLoc();
  if (isa<tosa::ReduceSumOp>(op) && elementTy.isa<FloatType>()) {
    return rewriter.create<AddFOp>(loc, args);
  }

  if (isa<tosa::ReduceSumOp>(op) && elementTy.isa<IntegerType>()) {
    return rewriter.create<AddIOp>(loc, args);
  }

  if (isa<tosa::ReduceProdOp>(op) && elementTy.isa<FloatType>()) {
    return rewriter.create<MulFOp>(loc, args);
  }

  if (isa<tosa::ReduceProdOp>(op) && elementTy.isa<IntegerType>()) {
    return rewriter.create<MulIOp>(loc, args);
  }

  if (isa<tosa::ReduceMinOp>(op) && elementTy.isa<FloatType>()) {
    auto predicate = rewriter.create<mlir::CmpFOp>(loc, CmpFPredicate::OLT,
                                                   args[0], args[1]);
    return rewriter.create<mlir::SelectOp>(loc, predicate, args[0], args[1]);
  }

  if (isa<tosa::ReduceMinOp>(op) && elementTy.isa<IntegerType>()) {
    auto predicate = rewriter.create<mlir::CmpIOp>(loc, CmpIPredicate::slt,
                                                   args[0], args[1]);
    return rewriter.create<mlir::SelectOp>(loc, predicate, args[0], args[1]);
  }

  if (isa<tosa::ReduceMaxOp>(op) && elementTy.isa<FloatType>()) {
    auto predicate = rewriter.create<mlir::CmpFOp>(loc, CmpFPredicate::OGT,
                                                   args[0], args[1]);
    return rewriter.create<mlir::SelectOp>(loc, predicate, args[0], args[1]);
  }

  if (isa<tosa::ReduceMaxOp>(op) && elementTy.isa<IntegerType>()) {
    auto predicate = rewriter.create<mlir::CmpIOp>(loc, CmpIPredicate::sgt,
                                                   args[0], args[1]);
    return rewriter.create<mlir::SelectOp>(loc, predicate, args[0], args[1]);
  }

  if (isa<tosa::ReduceAllOp>(op) && elementTy.isInteger(1))
    return rewriter.create<mlir::AndOp>(loc, args);

  if (isa<tosa::ReduceAnyOp>(op) && elementTy.isInteger(1))
    return rewriter.create<mlir::OrOp>(loc, args);

  return {};
}

// Performs the match and rewrite for reduction operations. This includes
// declaring a correctly sized initial value, and the linalg.generic operation
// that reduces across the specified axis.
static LogicalResult reduceMatchAndRewriteHelper(Operation *op, uint64_t axis,
                                                 PatternRewriter &rewriter) {
  auto loc = op->getLoc();
  auto inputTy = op->getOperand(0).getType().template cast<ShapedType>();
  auto resultTy = op->getResult(0).getType().template cast<ShapedType>();
  auto elementTy = resultTy.getElementType();
  Value input = op->getOperand(0);

  llvm::SmallVector<int64_t> reduceShape;
  for (unsigned i = 0; i < inputTy.getRank(); i++) {
    if (axis != i)
      reduceShape.push_back(inputTy.getDimSize(i));
  }

  Type reduceTy = RankedTensorType::get(reduceShape, resultTy.getElementType());

  // First fill the output buffer with the init value.
  auto initTensor =
      rewriter
          .create<linalg::InitTensorOp>(loc, ArrayRef<Value>({}), reduceShape,
                                        resultTy.getElementType())
          .result();

  auto fillValueAttr = createInitialValueForReduceOp(op, elementTy, rewriter);
  if (!fillValueAttr)
    return rewriter.notifyMatchFailure(
        op, "No initial value found for reduction operation");

  auto fillValue = rewriter.create<ConstantOp>(loc, fillValueAttr);
  auto filledTensor =
      rewriter.create<linalg::FillOp>(loc, initTensor, fillValue).result();

  SmallVector<AffineExpr, 2> srcExprs;
  SmallVector<AffineExpr, 2> dstExprs;
  SmallVector<StringRef, 4> iteratorTypes;
  for (unsigned int i = 0, rank = inputTy.getRank(); i != rank; ++i) {
    srcExprs.push_back(mlir::getAffineDimExpr(i, rewriter.getContext()));

    iteratorTypes.push_back(axis == i ? getReductionIteratorTypeName()
                                      : getParallelIteratorTypeName());
    if (axis != i)
      dstExprs.push_back(mlir::getAffineDimExpr(i, rewriter.getContext()));
  }

  bool didEncounterError = false;
  auto maps = AffineMap::inferFromExprList({srcExprs, dstExprs});
  auto linalgOp = rewriter.create<linalg::GenericOp>(
      loc, reduceTy, input, filledTensor, maps, iteratorTypes,
      [&](OpBuilder &nestedBuilder, Location nestedLoc, ValueRange blockArgs) {
        auto result = createLinalgBodyCalculationForReduceOp(
            op, blockArgs, elementTy, rewriter);
        if (result)
          didEncounterError = true;

        nestedBuilder.create<linalg::YieldOp>(loc, result);
      });

  if (!didEncounterError)
    return failure();

  rewriter.replaceOpWithNewOp<tosa::ReshapeOp>(op, resultTy,
                                               linalgOp.getResults());
  return success();
}

static LogicalResult
convolutionMatchAndRewriterHelper(Operation *op,
                                  ConversionPatternRewriter &rewriter) {
  Location loc = op->getLoc();
  Value input = op->getOperand(0);
  Value weight = op->getOperand(1);
  Value bias = op->getOperand(2);

  ShapedType inputTy = input.getType().cast<ShapedType>();
  ShapedType weightTy = weight.getType().cast<ShapedType>();
  ShapedType biasTy = bias.getType().cast<ShapedType>();
  ShapedType resultTy = op->getResult(0).getType().cast<ShapedType>();

  Type inputETy = inputTy.getElementType();
  Type weightETy = weightTy.getElementType();
  Type biasETy = biasTy.getElementType();
  Type resultETy = resultTy.getElementType();

  auto padAttr = op->getAttr("pad").cast<ArrayAttr>();
  auto strideTosaAttr = op->getAttr("stride").cast<ArrayAttr>();
  auto dilationTosaAttr = op->getAttr("dilation").cast<ArrayAttr>();

  if (!inputTy.hasStaticShape() || !weightTy.hasStaticShape() ||
      !biasTy.hasStaticShape() || !resultTy.hasStaticShape())
    return rewriter.notifyMatchFailure(op,
                                       "tosa.conv ops require static shapes");

  auto weightShape = weightTy.getShape();
  auto resultShape = resultTy.getShape();

  // TODO(suderman): Support other types.
  if (!inputETy.isF32() || !weightETy.isF32() || !biasETy.isF32() ||
      !resultETy.isF32())
    return failure();

  // Apply padding as necessary.
  Attribute zeroAttr = rewriter.getZeroAttr(inputETy);
  llvm::SmallVector<int64_t> pad;
  pad.resize(2, 0);
  getValuesFromIntArrayAttribute(padAttr, pad);
  pad.resize(pad.size() + 2, 0);

  input = applyPad(loc, input, pad, zeroAttr, rewriter);

  // We need to transpose the Conv2DOp kernel to line up the last input/output
  // kernels.
  // TODO(suderman): Eventually we will support specifying the filter channel
  // ordering then we can avoid transposing the kernel.
  if (isa<tosa::Conv2DOp>(op)) {
    int32_t weightRank = weightTy.getRank();
    SmallVector<int64_t> permutation, transposeWeightShape;
    permutation.resize(weightRank, 0);
    transposeWeightShape.resize(weightRank, 0);
    for (int i = 0; i < weightRank; i++) {
      permutation[i] = (i + 1) % weightRank;
      transposeWeightShape[i] = weightShape[permutation[i]];
    }

    Value permutationValue = rewriter.create<ConstantOp>(
        loc, DenseIntElementsAttr::get(
                 RankedTensorType::get({weightRank}, rewriter.getI64Type()),
                 permutation));
    Type newWeightTy = RankedTensorType::get(transposeWeightShape, biasETy);

    weight = rewriter.create<tosa::TransposeOp>(loc, newWeightTy, weight,
                                                permutationValue);
  }

  // Broadcast the initial value to the output tensor before convolving.
  SmallVector<AffineMap, 4> indexingMaps;
  indexingMaps.push_back(AffineMap::get(
      /*dimCount=*/resultTy.getRank(), /*symbolCount=*/0,
      {rewriter.getAffineDimExpr(3)}, rewriter.getContext()));
  indexingMaps.push_back(rewriter.getMultiDimIdentityMap(resultTy.getRank()));

  Value initTensor = rewriter.create<linalg::InitTensorOp>(
      loc, resultTy.getShape(), resultTy.getElementType());

  Value biasBroadcast =
      rewriter
          .create<linalg::GenericOp>(
              loc, resultTy, bias, initTensor, indexingMaps,
              getNParallelLoopsAttrs(resultTy.getRank()),
              [&](OpBuilder &nestedBuilder, Location nestedLoc,
                  ValueRange args) {
                nestedBuilder.create<linalg::YieldOp>(nestedLoc, args[0]);
              })
          .getResult(0);

  // Extract the attributes for convolution.
  llvm::SmallVector<int64_t> stride, dilation;
  getValuesFromIntArrayAttribute(strideTosaAttr, stride);
  getValuesFromIntArrayAttribute(dilationTosaAttr, dilation);

  // Create the convolution op.
  auto strideAttr = DenseIntElementsAttr::get(
      RankedTensorType::get({2}, rewriter.getI64Type()), stride);
  auto dilationAttr = DenseIntElementsAttr::get(
      RankedTensorType::get({2}, rewriter.getI64Type()), dilation);

  if (isa<tosa::Conv2DOp>(op)) {
    rewriter.replaceOpWithNewOp<linalg::ConvInputNHWCFilterHWCFOp>(
        op, resultTy, ValueRange{input, weight}, ValueRange{biasBroadcast},
        dilationAttr, strideAttr);
    return success();
  }

  if (isa<tosa::DepthwiseConv2DOp>(op)) {
    if (llvm::any_of(dilation, [](int64_t d) { return d > 1; }))
      return failure();

    ShapedType linalgConvTy =
        RankedTensorType::get({resultShape[0], resultShape[1], resultShape[2],
                               weightShape[2], weightShape[3]},
                              resultETy);

    Value biasReshape =
        rewriter.create<tosa::ReshapeOp>(loc, linalgConvTy, biasBroadcast);
    Value conv = rewriter
                     .create<linalg::DepthwiseConvInputNHWCFilterHWCFOp>(
                         loc, linalgConvTy, ValueRange{input, weight},
                         ValueRange{biasReshape}, strideAttr)
                     .getResult(0);

    Value reshape = rewriter.create<tosa::ReshapeOp>(loc, resultTy, conv);
    rewriter.replaceOp(op, reshape);
    return success();
  }

  return failure();
}

namespace {

template <typename SrcOp>
class PointwiseConverter : public OpRewritePattern<SrcOp> {
public:
  using OpRewritePattern<SrcOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(SrcOp op,
                                PatternRewriter &rewriter) const final {
    return elementwiseMatchAndRewriteHelper(op, rewriter);
  }
};

template <typename T>
class ConvConverter : public OpConversionPattern<T> {
public:
  using OpConversionPattern<T>::OpConversionPattern;
  LogicalResult
  matchAndRewrite(T op, ArrayRef<Value> args,
                  ConversionPatternRewriter &rewriter) const final {
    return convolutionMatchAndRewriterHelper(op, rewriter);
  }
};

class MatMulConverter : public OpConversionPattern<tosa::MatMulOp> {
public:
  using OpConversionPattern<tosa::MatMulOp>::OpConversionPattern;
  LogicalResult
  matchAndRewrite(tosa::MatMulOp op, ArrayRef<Value> args,
                  ConversionPatternRewriter &rewriter) const final {
    tosa::MatMulOp::Adaptor adaptor(args);

    Location loc = op.getLoc();

    auto outputTy = op.getType().cast<ShapedType>();
    auto outputElementTy = outputTy.getElementType();
    auto zeroAttr = rewriter.getZeroAttr(outputElementTy);
    Value zero = rewriter.create<ConstantOp>(loc, zeroAttr);
    auto initTensor = rewriter.create<linalg::InitTensorOp>(
        loc, outputTy.getShape(), outputTy.getElementType());
    Value zeroTensor =
        rewriter.create<linalg::FillOp>(loc, initTensor, zero).getResult(0);
    rewriter.replaceOpWithNewOp<linalg::MatmulOp>(
        op, TypeRange{op.getType()}, ValueRange{adaptor.a(), adaptor.b()},
        ValueRange{zeroTensor});
    return success();
  }
};

class FullyConnectedConverter
    : public OpConversionPattern<tosa::FullyConnectedOp> {
public:
  using OpConversionPattern<tosa::FullyConnectedOp>::OpConversionPattern;
  LogicalResult
  matchAndRewrite(tosa::FullyConnectedOp op, ArrayRef<Value> args,
                  ConversionPatternRewriter &rewriter) const final {
    Location loc = op.getLoc();
    auto outputTy = op.getType().cast<ShapedType>();
    auto input = op.input();
    auto weight = op.weight();
    auto bias = op.bias();

    auto weightTy = weight.getType().cast<ShapedType>();
    auto biasTy = bias.getType().cast<ShapedType>();

    auto weightShape = weightTy.getShape();

    if (op.quantization_info())
      return failure();

    // Creating maps for the output of MatMul and the bias
    SmallVector<AffineMap, 4> indexingMaps;

    // Broadcast the bias.
    indexingMaps.push_back(AffineMap::get(/*dimCount=*/2, /*symbolCount=*/0,
                                          {rewriter.getAffineDimExpr(1)},
                                          rewriter.getContext()));

    indexingMaps.push_back(rewriter.getMultiDimIdentityMap(outputTy.getRank()));

    auto initTensor =
        rewriter
            .create<linalg::InitTensorOp>(loc, outputTy.getShape(),
                                          outputTy.getElementType())
            ->getResults();

    auto linalgOp =
        rewriter
            .create<linalg::GenericOp>(
                loc, outputTy, bias, initTensor, indexingMaps,
                getNParallelLoopsAttrs(outputTy.getRank()),
                [&](OpBuilder &nested_builder, Location nested_loc,
                    ValueRange args) {
                  nested_builder.create<linalg::YieldOp>(loc, *args.begin());
                })
            ->getResults();

    SmallVector<int64_t> permutation{1, 0};
    auto permutationAttr = DenseIntElementsAttr::get(
        RankedTensorType::get({2}, rewriter.getI64Type()), permutation);
    Value permutationValue = rewriter.create<ConstantOp>(loc, permutationAttr);

    SmallVector<int64_t> newWeightShape{weightShape[1], weightShape[0]};
    Type newWeightTy =
        RankedTensorType::get(newWeightShape, biasTy.getElementType());

    Value transposedWeight = rewriter.create<tosa::TransposeOp>(
        loc, newWeightTy, weight, permutationValue);

    rewriter.replaceOpWithNewOp<linalg::MatmulOp>(
        op, TypeRange{op.getType()}, ValueRange{input, transposedWeight},
        linalgOp);
    return success();
  }
};


class ReshapeConverter : public OpConversionPattern<tosa::ReshapeOp> {
public:
  using OpConversionPattern<tosa::ReshapeOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(tosa::ReshapeOp reshape, ArrayRef<Value> args,
                  ConversionPatternRewriter &rewriter) const final {
    typename tosa::ReshapeOp::Adaptor operands(args);

    ShapedType operandTy = operands.input1().getType().cast<ShapedType>();
    ShapedType resultTy = reshape.getType().template cast<ShapedType>();

    if (operandTy == resultTy) {
      rewriter.replaceOp(reshape, args[0]);
      return success();
    }

    if (!operandTy.hasStaticShape() || !resultTy.hasStaticShape())
      return failure();

    // Compute the reassociation maps for the linalg operation.
    ArrayRef<int64_t> expandedShape =
        (operandTy.getRank() > resultTy.getRank() ? operandTy.getShape()
                                                  : resultTy.getShape());
    ArrayRef<int64_t> collapsedShape =
        (operandTy.getRank() > resultTy.getRank() ? resultTy.getShape()
                                                  : operandTy.getShape());
    unsigned currSrcDim = 0, currDstDim = 0;
    SmallVector<linalg::ReassociationExprs, 4> reassociationMap(
        collapsedShape.size());

    // First scan all dimensions in the source shapes to see whether we have a
    // perfect case where consecutive dimensions in source are collapsed. For
    // such case we can just generate one single linalg.reshape.
    bool isCollapsingSource = true;
    while (currSrcDim < expandedShape.size() &&
           currDstDim < collapsedShape.size()) {
      int64_t dstSize = collapsedShape[currDstDim];
      int64_t srcSize = expandedShape[currSrcDim];
      while (srcSize < dstSize && currSrcDim < expandedShape.size()) {
        reassociationMap[currDstDim].push_back(
            rewriter.getAffineDimExpr(currSrcDim++));
        srcSize *= expandedShape[currSrcDim];
      }
      if (srcSize == dstSize) {
        reassociationMap[currDstDim].push_back(
            rewriter.getAffineDimExpr(currSrcDim++));
        // If the next dim in collapsedShape is not 1, treat subsequent dims in
        // expandedShape which are 1 to be collapsed.
        if (currDstDim == collapsedShape.size() - 1 ||
            collapsedShape[currDstDim + 1] != 1) {
          while (currSrcDim < expandedShape.size() &&
                 expandedShape[currSrcDim] == 1) {
            reassociationMap[currDstDim].push_back(
                rewriter.getAffineDimExpr(currSrcDim++));
          }
        }
      } else {
        isCollapsingSource = false;
        break;
      }
      currDstDim++;
    }

    // Check if any remaining dimensions exist. If either is rank-0 we only
    // require the directly lowering.
    if (currSrcDim != expandedShape.size() ||
        currDstDim != collapsedShape.size())
      isCollapsingSource = collapsedShape.empty() || expandedShape.empty();

    // Otherwise, we need to first reduce all source dimensions into one and
    // then expand to the destination dimensions.
    if (!isCollapsingSource) {
      auto getIdentityExprs = [&rewriter](int n) {
        SmallVector<AffineExpr, 4> exprs;
        for (int i = 0; i < n; ++i)
          exprs.push_back(rewriter.getAffineDimExpr(i));
        return exprs;
      };
      Location loc = reshape.getLoc();
      int64_t totalElems =
          std::accumulate(expandedShape.begin(), expandedShape.end(), 1,
                          std::multiplies<int64_t>());
      auto elemTy = operandTy.getElementType();
      SmallVector<linalg::ReassociationExprs, 4> collapsingMap = {
          // Use operandTy here because we need to collapse all operands
          // dimensions.
          getIdentityExprs(operandTy.getShape().size())};
      SmallVector<linalg::ReassociationExprs, 4> expandingMap = {
          // Use resultTy here because we need to expand to all result
          // dimensions.
          getIdentityExprs(resultTy.getShape().size())};

      auto collapsedTy = RankedTensorType::get({totalElems}, elemTy);
      Value collapsedOp = rewriter.create<linalg::TensorReshapeOp>(
          loc, collapsedTy, args[0], collapsingMap);
      rewriter.replaceOpWithNewOp<linalg::TensorReshapeOp>(
          reshape, resultTy, collapsedOp, expandingMap);

      return success();
    }

    rewriter.replaceOpWithNewOp<linalg::TensorReshapeOp>(
        reshape, resultTy, args[0], reassociationMap);

    return success();
  }
};

class TransposeConverter : public OpRewritePattern<tosa::TransposeOp> {
public:
  using OpRewritePattern<tosa::TransposeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tosa::TransposeOp op,
                                PatternRewriter &rewriter) const final {
    DenseIntElementsAttr perms;
    if (!matchPattern(op.perms(), m_Constant(&perms))) {
      return failure();
    }

    auto resultTy = op.getType().cast<ShapedType>();
    if (!resultTy.hasStaticShape())
      return failure();

    SmallVector<AffineExpr, 2> inputExprs;
    inputExprs.resize(resultTy.getRank());
    for (auto permutation : llvm::enumerate(perms.getIntValues())) {
      inputExprs[permutation.value().getZExtValue()] =
          rewriter.getAffineDimExpr(permutation.index());
    }

    auto initTensor = rewriter.create<linalg::InitTensorOp>(
        op.getLoc(), ArrayRef<Value>({}), resultTy.getShape(),
        resultTy.getElementType());

    SmallVector<AffineMap, 2> affineMaps = {
        AffineMap::get(resultTy.getRank(), /*symbolCount=*/0, inputExprs,
                       rewriter.getContext()),
        rewriter.getMultiDimIdentityMap(resultTy.getRank())};

    rewriter.replaceOpWithNewOp<linalg::GenericOp>(
        op, resultTy, op.input1(), ValueRange{initTensor}, affineMaps,
        getNParallelLoopsAttrs(resultTy.getRank()),
        [&](OpBuilder &nestedBuilder, Location nestedLoc, ValueRange args) {
          nestedBuilder.create<linalg::YieldOp>(op.getLoc(), *args.begin());
        });
    return success();
  }
};

class RescaleConverter : public OpRewritePattern<tosa::RescaleOp> {
public:
  using OpRewritePattern<tosa::RescaleOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tosa::RescaleOp op,
                                PatternRewriter &rewriter) const final {
    auto loc = op.getLoc();
    auto input = op.input();
    auto inputTy = op.input().getType().cast<ShapedType>();
    auto outputTy = op.output().getType().cast<ShapedType>();
    unsigned rank = inputTy.getRank();

    if (!outputTy.hasStaticShape())
      return rewriter.notifyMatchFailure(
          op, "tosa to linalg conversion expects statically shaped tensors");

    // The shift and multiplier values.
    SmallVector<int32_t> multiplierValues;
    getValuesFromIntArrayAttribute(op.multiplier(), multiplierValues);

    SmallVector<int8_t> shiftValues;
    getValuesFromIntArrayAttribute(op.shift(), shiftValues);

    // Double round only occurs if shift is greater than 31, check that this
    // is ever true.
    bool doubleRound =
        op.double_round() &&
        llvm::any_of(shiftValues, [](int32_t v) { return v > 31; });

    SmallVector<AffineMap> indexingMaps = {
        rewriter.getMultiDimIdentityMap(rank)};
    SmallVector<Value, 4> genericInputs = {input};

    // If we are rescaling per-channel then we need to store the multiplier
    // values in a buffer.
    Value multiplierConstant;
    int64_t multiplierArg = 0;
    if (multiplierValues.size() == 1) {
      multiplierConstant = rewriter.create<ConstantOp>(
          loc, rewriter.getI32IntegerAttr(multiplierValues.front()));
    } else {
      SmallVector<AffineExpr, 2> multiplierExprs{
          rewriter.getAffineDimExpr(rank - 1)};
      auto multiplierType =
          RankedTensorType::get({static_cast<int64_t>(multiplierValues.size())},
                                rewriter.getI32Type());
      genericInputs.push_back(rewriter.create<ConstantOp>(
          loc, DenseIntElementsAttr::get(multiplierType, multiplierValues)));

      indexingMaps.push_back(AffineMap::get(/*dimCount=*/rank,
                                            /*symbolCount=*/0, multiplierExprs,
                                            rewriter.getContext()));

      multiplierArg = indexingMaps.size() - 1;
    }

    // If we are rescaling per-channel then we need to store the shift
    // values in a buffer.
    Value shiftConstant;
    int64_t shiftArg = 0;
    if (shiftValues.size() == 1) {
      shiftConstant = rewriter.create<ConstantOp>(
          loc, rewriter.getI8IntegerAttr(shiftValues.front()));
    } else {
      SmallVector<AffineExpr, 2> shiftExprs = {
          rewriter.getAffineDimExpr(rank - 1)};
      auto shiftType =
          RankedTensorType::get({static_cast<int64_t>(shiftValues.size())},
                                rewriter.getIntegerType(8));
      genericInputs.push_back(rewriter.create<ConstantOp>(
          loc, DenseIntElementsAttr::get(shiftType, shiftValues)));
      indexingMaps.push_back(AffineMap::get(/*dimCount=*/rank,
                                            /*symbolCount=*/0, shiftExprs,
                                            rewriter.getContext()));
      shiftArg = indexingMaps.size() - 1;
    }

    // Indexing maps for output values.
    indexingMaps.push_back(rewriter.getMultiDimIdentityMap(rank));

    // Construct the indexing maps needed for linalg.generic ops.
    Value initTensor = rewriter.create<linalg::InitTensorOp>(
        loc, ArrayRef<Value>({}), outputTy.getShape(),
        outputTy.getElementType());

    auto linalgOp = rewriter.create<linalg::GenericOp>(
        loc, outputTy, genericInputs, ValueRange{initTensor}, indexingMaps,
        getNParallelLoopsAttrs(rank),
        [&](OpBuilder &nestedBuilder, Location nestedLoc,
            ValueRange blockArgs) {
          // For now we do all of our math in 64-bit. This is not optimal but
          // should be correct for now, consider computing correct bit depth
          // later.
          auto inputZp = createConstFromIntAttribute<int32_t>(
              op, "input_zp", nestedBuilder.getI32Type(), nestedBuilder);
          auto outputZp = createConstFromIntAttribute<int32_t>(
              op, "output_zp", nestedBuilder.getI32Type(), nestedBuilder);

          Value value = blockArgs[0];
          Value multiplier = multiplierConstant ? multiplierConstant
                                                : blockArgs[multiplierArg];
          Value shift = shiftConstant ? shiftConstant : blockArgs[shiftArg];

          if (value.getType().getIntOrFloatBitWidth() < 32) {
            value = nestedBuilder.create<SignExtendIOp>(
                nestedLoc, nestedBuilder.getI32Type(), value);
          }

          value = nestedBuilder.create<SubIOp>(nestedLoc, value, inputZp);

          value = nestedBuilder.create<tosa::ApplyScaleOp>(
              loc, nestedBuilder.getI32Type(), value, multiplier, shift,
              nestedBuilder.getBoolAttr(doubleRound));

          // Move to the new zero-point.
          value = nestedBuilder.create<AddIOp>(nestedLoc, value, outputZp);

          // Saturate to the output size.
          IntegerType outIntType =
              blockArgs.back().getType().cast<IntegerType>();
          unsigned outBitWidth = outIntType.getWidth();
          auto intMin = nestedBuilder.create<ConstantOp>(
              loc, nestedBuilder.getIntegerAttr(
                       nestedBuilder.getI32Type(),
                       APInt::getSignedMinValue(outBitWidth).getSExtValue()));
          auto intMax = nestedBuilder.create<ConstantOp>(
              loc, nestedBuilder.getIntegerAttr(
                       nestedBuilder.getI32Type(),
                       APInt::getSignedMaxValue(outBitWidth).getSExtValue()));

          value = clampHelper<mlir::CmpIOp>(nestedLoc, value, intMin, intMax,
                                            CmpIPredicate::slt, nestedBuilder);

          if (outIntType.getWidth() < 32) {
            value =
                nestedBuilder.create<TruncateIOp>(nestedLoc, outIntType, value);
          }

          nestedBuilder.create<linalg::YieldOp>(loc, value);
        });

    rewriter.replaceOp(op, linalgOp->getResults());
    return success();
  }
};

class ResizeConverter : public OpRewritePattern<tosa::ResizeOp> {
public:
  using OpRewritePattern<tosa::ResizeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tosa::ResizeOp op,
                                PatternRewriter &rewriter) const final {
    Location loc = op.getLoc();
    auto input = op.input();
    auto inputTy = input.getType().cast<ShapedType>();
    auto resultTy = op.getType().cast<ShapedType>();
    auto resultElementTy = resultTy.getElementType();

    auto imageH = inputTy.getShape()[1];
    auto imageW = inputTy.getShape()[2];

    if (!resultTy.hasStaticShape())
      return failure();
    if (op.mode() != "NEAREST_NEIGHBOR" && op.mode() != "BILINEAR")
      return failure();

    auto initTensor =
        rewriter
            .create<linalg::InitTensorOp>(loc, ArrayRef<Value>{},
                                          resultTy.getShape(), resultElementTy)
            .result();

    SmallVector<AffineMap, 2> affineMaps = {
        rewriter.getMultiDimIdentityMap(resultTy.getRank())};

    auto genericOp = rewriter.create<linalg::IndexedGenericOp>(
        loc, resultTy, ValueRange({}), ValueRange{initTensor}, affineMaps,
        getNParallelLoopsAttrs(resultTy.getRank()));
    rewriter.replaceOp(op, genericOp.getResult(0));

    {
      OpBuilder::InsertionGuard regionGuard(rewriter);
      Block *block = rewriter.createBlock(
          &genericOp.region(), genericOp.region().end(),
          TypeRange({rewriter.getIndexType(), rewriter.getIndexType(),
                     rewriter.getIndexType(), rewriter.getIndexType(),
                     resultElementTy}));
      Value batch = block->getArgument(0);
      Value channel = block->getArgument(3);

      auto hwMin =
          rewriter.create<ConstantOp>(loc, rewriter.getI32IntegerAttr(0));
      auto hMax = rewriter.create<ConstantOp>(
          loc, rewriter.getI32IntegerAttr(imageH - 1));
      auto wMax = rewriter.create<ConstantOp>(
          loc, rewriter.getI32IntegerAttr(imageW - 1));

      Value inY = rewriter.create<IndexCastOp>(loc, rewriter.getI32Type(),
                                               block->getArgument(1));
      Value inX = rewriter.create<IndexCastOp>(loc, rewriter.getI32Type(),
                                               block->getArgument(2));

      int32_t shift = op.shift();
      bool floatingPointMode = shift == 0;

      Value yStride, xStride, yOffset, xOffset;
      if (floatingPointMode) {
        yStride = rewriter.create<ConstantOp>(loc, op.stride_fp()[0]);
        xStride = rewriter.create<ConstantOp>(loc, op.stride_fp()[1]);
        yOffset = rewriter.create<ConstantOp>(loc, op.offset_fp()[0]);
        xOffset = rewriter.create<ConstantOp>(loc, op.offset_fp()[1]);
      } else {
        SmallVector<int32_t> stride, offset;
        getValuesFromIntArrayAttribute(op.stride(), stride);
        getValuesFromIntArrayAttribute(op.offset(), offset);

        yStride = rewriter.create<ConstantOp>(
            loc, rewriter.getI32IntegerAttr(stride[0]));
        xStride = rewriter.create<ConstantOp>(
            loc, rewriter.getI32IntegerAttr(stride[1]));
        yOffset = rewriter.create<ConstantOp>(
            loc, rewriter.getI32IntegerAttr(offset[0]));
        xOffset = rewriter.create<ConstantOp>(
            loc, rewriter.getI32IntegerAttr(offset[1]));
      }

      // Compute the the integer index and partial offset.
      // x = x * stride + offset;
      // ix = floor(x)
      // dx = x - ix
      Value ix, iy, dx, dy;
      if (floatingPointMode) {
        Value y = rewriter.create<UIToFPOp>(loc, rewriter.getF32Type(), inY);
        Value x = rewriter.create<UIToFPOp>(loc, rewriter.getF32Type(), inX);

        y = rewriter.create<MulFOp>(loc, y, yStride);
        x = rewriter.create<MulFOp>(loc, x, xStride);

        y = rewriter.create<AddFOp>(loc, y, yOffset);
        x = rewriter.create<AddFOp>(loc, x, xOffset);

        iy = rewriter.create<FloorFOp>(loc, y);
        ix = rewriter.create<FloorFOp>(loc, x);

        dy = rewriter.create<SubFOp>(loc, y, iy);
        dx = rewriter.create<SubFOp>(loc, x, ix);

        iy = rewriter.create<FPToSIOp>(loc, rewriter.getI32Type(), iy);
        ix = rewriter.create<FPToSIOp>(loc, rewriter.getI32Type(), ix);
      } else {
        Value shiftVal =
            rewriter.create<ConstantOp>(loc, rewriter.getI32IntegerAttr(shift));

        Value y = rewriter.create<MulIOp>(loc, inY, yStride);
        Value x = rewriter.create<MulIOp>(loc, inX, xStride);

        y = rewriter.create<AddIOp>(loc, y, yOffset);
        x = rewriter.create<AddIOp>(loc, x, xOffset);

        iy = rewriter.create<SignedShiftRightOp>(loc, y, shiftVal);
        ix = rewriter.create<SignedShiftRightOp>(loc, x, shiftVal);

        Value yTrunc = rewriter.create<ShiftLeftOp>(loc, iy, shiftVal);
        Value xTrunc = rewriter.create<ShiftLeftOp>(loc, ix, shiftVal);

        dy = rewriter.create<SubIOp>(loc, y, yTrunc);
        dx = rewriter.create<SubIOp>(loc, x, xTrunc);
      }

      if (op.mode() == "NEAREST_NEIGHBOR") {
        Value yPred, xPred;
        // Round the index position towards the closest pixel location.
        if (floatingPointMode) {
          auto halfVal =
              rewriter.create<ConstantOp>(loc, rewriter.getF32FloatAttr(0.5f));
          yPred = rewriter.create<mlir::CmpFOp>(loc, CmpFPredicate::OGE, dy,
                                                halfVal);
          xPred = rewriter.create<mlir::CmpFOp>(loc, CmpFPredicate::OGE, dx,
                                                halfVal);
        } else {
          auto halfVal = rewriter.create<ConstantOp>(
              loc, rewriter.getI32IntegerAttr(1 << (shift - 1)));
          yPred = rewriter.create<mlir::CmpIOp>(loc, CmpIPredicate::sge, dy,
                                                halfVal);
          xPred = rewriter.create<mlir::CmpIOp>(loc, CmpIPredicate::sge, dx,
                                                halfVal);
        }

        auto zeroVal =
            rewriter.create<ConstantOp>(loc, rewriter.getI32IntegerAttr(0));
        auto oneVal =
            rewriter.create<ConstantOp>(loc, rewriter.getI32IntegerAttr(1));

        auto yOffset =
            rewriter.create<mlir::SelectOp>(loc, yPred, oneVal, zeroVal);
        auto xOffset =
            rewriter.create<mlir::SelectOp>(loc, xPred, oneVal, zeroVal);

        iy = rewriter.create<AddIOp>(loc, iy, yOffset);
        ix = rewriter.create<AddIOp>(loc, ix, xOffset);

        // Clamp the to be within the bounds of the input image.

        iy = clampHelper<mlir::CmpIOp>(loc, iy, hwMin, hMax, CmpIPredicate::slt,
                                       rewriter);
        ix = clampHelper<mlir::CmpIOp>(loc, ix, hwMin, wMax, CmpIPredicate::slt,
                                       rewriter);

        // Read the value from the input array.
        iy = rewriter.create<IndexCastOp>(loc, rewriter.getIndexType(), iy);
        ix = rewriter.create<IndexCastOp>(loc, rewriter.getIndexType(), ix);

        Value result = rewriter.create<tensor::ExtractOp>(
            loc, input, ValueRange{batch, iy, ix, channel});

        rewriter.create<linalg::YieldOp>(loc, result);

        return success();
      }

      if (op.mode() == "BILINEAR") {
        Value y0 = iy;
        Value x0 = ix;

        auto oneVal =
            rewriter.create<ConstantOp>(loc, rewriter.getI32IntegerAttr(1));
        Value y1 = rewriter.create<AddIOp>(loc, y0, oneVal);
        Value x1 = rewriter.create<AddIOp>(loc, x0, oneVal);

        y0 = clampHelper<mlir::CmpIOp>(loc, y0, hwMin, hMax, CmpIPredicate::slt,
                                       rewriter);
        y1 = clampHelper<mlir::CmpIOp>(loc, y1, hwMin, hMax, CmpIPredicate::slt,
                                       rewriter);

        x0 = clampHelper<mlir::CmpIOp>(loc, x0, hwMin, wMax, CmpIPredicate::slt,
                                       rewriter);
        x1 = clampHelper<mlir::CmpIOp>(loc, x1, hwMin, wMax, CmpIPredicate::slt,
                                       rewriter);

        y0 = rewriter.create<IndexCastOp>(loc, rewriter.getIndexType(), y0);
        y1 = rewriter.create<IndexCastOp>(loc, rewriter.getIndexType(), y1);
        x0 = rewriter.create<IndexCastOp>(loc, rewriter.getIndexType(), x0);
        x1 = rewriter.create<IndexCastOp>(loc, rewriter.getIndexType(), x1);

        Value y0x0 = rewriter.create<tensor::ExtractOp>(
            loc, input, ValueRange{batch, y0, x0, channel});
        Value y0x1 = rewriter.create<tensor::ExtractOp>(
            loc, input, ValueRange{batch, y0, x1, channel});
        Value y1x0 = rewriter.create<tensor::ExtractOp>(
            loc, input, ValueRange{batch, y1, x0, channel});
        Value y1x1 = rewriter.create<tensor::ExtractOp>(
            loc, input, ValueRange{batch, y1, x1, channel});

        if (floatingPointMode) {
          auto oneVal =
              rewriter.create<ConstantOp>(loc, rewriter.getF32FloatAttr(1.f));
          Value rightPart = dx;
          Value leftPart = rewriter.create<SubFOp>(loc, oneVal, dx);

          y0x0 = rewriter.create<MulFOp>(loc, y0x0, leftPart);
          y0x1 = rewriter.create<MulFOp>(loc, y0x1, rightPart);
          Value topAcc = rewriter.create<AddFOp>(loc, y0x0, y0x1);

          y1x0 = rewriter.create<MulFOp>(loc, y1x0, leftPart);
          y1x1 = rewriter.create<MulFOp>(loc, y1x1, rightPart);
          Value bottomAcc = rewriter.create<AddFOp>(loc, y1x0, y1x1);

          Value bottomPart = dy;
          Value topPart = rewriter.create<SubFOp>(loc, oneVal, dy);
          topAcc = rewriter.create<MulFOp>(loc, topAcc, topPart);
          bottomAcc = rewriter.create<MulFOp>(loc, bottomAcc, bottomPart);
          Value result = rewriter.create<AddFOp>(loc, topAcc, bottomAcc);

          rewriter.create<linalg::YieldOp>(loc, result);
          return success();
        } else {
          y0x0 = rewriter.create<SignExtendIOp>(loc, resultElementTy, y0x0);
          y0x1 = rewriter.create<SignExtendIOp>(loc, resultElementTy, y0x1);
          y1x0 = rewriter.create<SignExtendIOp>(loc, resultElementTy, y1x0);
          y1x1 = rewriter.create<SignExtendIOp>(loc, resultElementTy, y1x1);

          if (resultElementTy.getIntOrFloatBitWidth() > 32) {
            dx = rewriter.create<SignExtendIOp>(loc, resultElementTy, dx);
            dy = rewriter.create<SignExtendIOp>(loc, resultElementTy, dy);
          }

          auto unitVal = rewriter.create<ConstantOp>(
              loc, rewriter.getIntegerAttr(resultElementTy, 1 << shift));
          Value rightPart = dx;
          Value leftPart = rewriter.create<SubIOp>(loc, unitVal, dx);

          y0x0 = rewriter.create<MulIOp>(loc, y0x0, leftPart);
          y0x1 = rewriter.create<MulIOp>(loc, y0x1, rightPart);
          Value topAcc = rewriter.create<AddIOp>(loc, y0x0, y0x1);

          y1x0 = rewriter.create<MulIOp>(loc, y1x0, leftPart);
          y1x1 = rewriter.create<MulIOp>(loc, y1x1, rightPart);
          Value bottomAcc = rewriter.create<AddIOp>(loc, y1x0, y1x1);

          Value bottomPart = dy;
          Value topPart = rewriter.create<SubIOp>(loc, unitVal, dy);
          topAcc = rewriter.create<MulIOp>(loc, topAcc, topPart);
          bottomAcc = rewriter.create<MulIOp>(loc, bottomAcc, bottomPart);
          Value result = rewriter.create<AddIOp>(loc, topAcc, bottomAcc);

          rewriter.create<linalg::YieldOp>(loc, result);
          return success();
        }
      }

      return failure();
    }

    return success();
  }
};

// At the codegen level any identity operations should be removed. Any cases
// where identity is load-bearing (e.g. cross device computation) should be
// handled before lowering to codegen.
template <typename SrcOp>
class IdentityNConverter : public OpRewritePattern<SrcOp> {
public:
  using OpRewritePattern<SrcOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(SrcOp op,
                                PatternRewriter &rewriter) const final {
    rewriter.replaceOp(op, op.getOperation()->getOperands());
    return success();
  }
};

template <typename SrcOp>
class ReduceConverter : public OpRewritePattern<SrcOp> {
public:
  using OpRewritePattern<SrcOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(SrcOp reduceOp,
                                PatternRewriter &rewriter) const final {
    return reduceMatchAndRewriteHelper(reduceOp, reduceOp.axis(), rewriter);
  }
};

struct ConcatConverter : public OpConversionPattern<tosa::ConcatOp> {
  using OpConversionPattern<tosa::ConcatOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(tosa::ConcatOp op, ArrayRef<Value> args,
                  ConversionPatternRewriter &rewriter) const override {
    auto resultType = op.getType().dyn_cast<RankedTensorType>();
    if (!resultType || !resultType.hasStaticShape()) {
      return rewriter.notifyMatchFailure(op,
                                         "expected static shaped tensor type");
    }

    Location loc = op.getLoc();
    int axis = op.axis();
    Value axisValue =
        rewriter.create<ConstantOp>(loc, rewriter.getIndexAttr(axis));
    int rank = resultType.getRank();
    SmallVector<Value, 3> offsets, sizes, strides;
    sizes.reserve(rank);
    strides.resize(rank, rewriter.create<ConstantIndexOp>(loc, 1));
    offsets.resize(rank, rewriter.create<ConstantIndexOp>(loc, 0));

    for (int i = 0; i < rank; ++i) {
      sizes.push_back(rewriter.create<memref::DimOp>(loc, args[0], i));
    }

    Value resultDimSize = sizes[axis];
    for (auto arg : args.drop_front()) {
      auto size = rewriter.create<memref::DimOp>(loc, arg, axisValue);
      resultDimSize = rewriter.create<AddIOp>(loc, resultDimSize, size);
    }
    sizes[axis] = resultDimSize;

    Value init = rewriter.create<linalg::InitTensorOp>(
        loc, resultType.getShape(), resultType.getElementType());

    Value zeroVal = rewriter.create<ConstantOp>(
        loc, rewriter.getZeroAttr(resultType.getElementType()));
    Value result =
        rewriter.create<linalg::FillOp>(loc, init, zeroVal).getResult(0);

    for (auto arg : args) {
      sizes[axis] = rewriter.create<memref::DimOp>(loc, arg, axisValue);
      result = rewriter.create<SubTensorInsertOp>(loc, arg, result, offsets,
                                                  sizes, strides);
      offsets[axis] = rewriter.create<AddIOp>(loc, offsets[axis], sizes[axis]);
    }
    rewriter.replaceOp(op, result);
    return success();
  }
};

class ReverseConverter : public OpRewritePattern<tosa::ReverseOp> {
public:
  using OpRewritePattern<tosa::ReverseOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tosa::ReverseOp op,
                                PatternRewriter &rewriter) const final {
    auto loc = op.getLoc();
    Value input = op.input();
    auto inputTy = input.getType().template cast<ShapedType>();
    auto resultTy = op.getType().template cast<ShapedType>();
    auto rank = resultTy.getRank();
    auto axis = op.axis();

    if (!inputTy.hasStaticShape())
      return rewriter.notifyMatchFailure(
          op, "No initial value found for reduction operation");

    // First fill the output buffer with the init value.
    auto initTensor = rewriter
                          .create<linalg::InitTensorOp>(
                              loc, ArrayRef<Value>({}), inputTy.getShape(),
                              inputTy.getElementType())
                          .result();

    SmallVector<AffineExpr, 2> inputExprs;
    inputExprs.resize(resultTy.getRank());

    for (int i = 0; i < rank; i++)
      inputExprs[i] = rewriter.getAffineDimExpr(i);

    inputExprs[axis] =
        rewriter.getAffineConstantExpr(inputTy.getDimSize(axis) - 1) -
        inputExprs[axis];

    SmallVector<AffineMap, 2> affineMaps = {
        AffineMap::get(resultTy.getRank(), /*symbolCount=*/0, inputExprs,
                       rewriter.getContext()),
        rewriter.getMultiDimIdentityMap(resultTy.getRank())};

    rewriter.replaceOpWithNewOp<linalg::GenericOp>(
        op, resultTy, op.input(), ValueRange{initTensor}, affineMaps,
        getNParallelLoopsAttrs(resultTy.getRank()),
        [&](OpBuilder &nestedBuilder, Location nestedLoc, ValueRange args) {
          nestedBuilder.create<linalg::YieldOp>(op.getLoc(), *args.begin());
        });
    return success();
  }
};

// This converter translate a tile operation to a reshape, broadcast, reshape.
// The first reshape minimally expands each tiled dimension to include a
// proceding size-1 dim. This dim is then broadcasted to the appropriate
// multiple.
struct TileConverter : public OpConversionPattern<tosa::TileOp> {
  using OpConversionPattern<tosa::TileOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(tosa::TileOp op, ArrayRef<Value> args,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    auto input = op.input1();
    auto inputTy = input.getType().cast<ShapedType>();
    auto inputShape = inputTy.getShape();
    auto resultTy = op.getType().cast<ShapedType>();
    auto elementTy = inputTy.getElementType();
    int64_t rank = inputTy.getRank();

    if (!inputTy.hasStaticShape() || !resultTy.hasStaticShape())
      return failure();

    SmallVector<int64_t> multiples;
    getValuesFromIntArrayAttribute(op.multiples(), multiples);

    // Broadcast the newly added dimensions to their appropriate multiple.
    SmallVector<int64_t, 2> genericShape;
    for (int i = 0; i < rank; i++) {
      genericShape.push_back(multiples[i]);
      genericShape.push_back(inputShape[i]);
    }

    auto initTensor = rewriter.create<linalg::InitTensorOp>(
        op.getLoc(), ArrayRef<Value>({}), genericShape, elementTy);

    // We needs to map the input shape to the non-broadcasted dimensions.
    SmallVector<AffineExpr, 4> dimExprs;
    dimExprs.reserve(rank);
    for (unsigned i = 0; i < rank; ++i)
      dimExprs.push_back(rewriter.getAffineDimExpr(i * 2 + 1));

    auto readAffineMap =
        AffineMap::get(/*dimCount=*/rank * 2, /*symbolCount=*/0, dimExprs,
                       rewriter.getContext());

    SmallVector<AffineMap, 2> affineMaps = {
        readAffineMap, rewriter.getMultiDimIdentityMap(genericShape.size())};

    auto genericOp = rewriter.create<linalg::GenericOp>(
        loc, RankedTensorType::get(genericShape, elementTy), input,
        ValueRange{initTensor}, affineMaps,
        getNParallelLoopsAttrs(genericShape.size()),
        [&](OpBuilder &nestedBuilder, Location nestedLoc, ValueRange args) {
          nestedBuilder.create<linalg::YieldOp>(op.getLoc(), *args.begin());
        });

    rewriter.replaceOpWithNewOp<tosa::ReshapeOp>(
        op, resultTy, genericOp.getResult(0),
        rewriter.getI64ArrayAttr(resultTy.getShape()));
    return success();
  }
};

class PadConverter : public OpRewritePattern<tosa::PadOp> {
public:
  using OpRewritePattern<tosa::PadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tosa::PadOp padOp,
                                PatternRewriter &rewriter) const final {
    auto loc = padOp.getLoc();
    auto input = padOp.input1();
    auto padding = padOp.padding();

    ShapedType inputTy = input.getType().cast<ShapedType>();
    ShapedType paddingTy = padding.getType().cast<ShapedType>();
    Type elementTy = inputTy.getElementType();
    int64_t rank = inputTy.getRank();

    if (!inputTy.hasStaticShape() || !paddingTy.hasStaticShape()) {
      return rewriter.notifyMatchFailure(
          padOp,
          "Pad converter requires static shaped input / padding values.");
    }

    Attribute constantAttr;
    if (elementTy.isa<FloatType>())
      constantAttr = rewriter.getFloatAttr(elementTy, 0.0);
    else if (elementTy.isa<IntegerType>() && !padOp.quantization_info())
      constantAttr = rewriter.getIntegerAttr(elementTy, 0);
    else if (elementTy.isa<IntegerType>() && padOp.quantization_info()) {
      auto value = padOp.quantization_info().getValue().input_zp().getValue();
      constantAttr = rewriter.getIntegerAttr(elementTy, value.getZExtValue());
    }

    if (!constantAttr) {
      return rewriter.notifyMatchFailure(
          padOp,
          "tosa.pad to linalg lowering encountered an unknown element type");
    }

    Value lowIndex = rewriter.create<ConstantOp>(loc, rewriter.getIndexAttr(0));
    Value highIndex =
        rewriter.create<ConstantOp>(loc, rewriter.getIndexAttr(1));

    SmallVector<OpFoldResult, 3> lowValues;
    SmallVector<OpFoldResult, 3> highValues;

    lowValues.reserve(rank);
    highValues.reserve(rank);

    for (int i = 0; i < rank; i++) {
      Value inputIndex = rewriter.createOrFold<ConstantIndexOp>(loc, i);
      Value lowVal = rewriter.createOrFold<tensor::ExtractOp>(
          loc, padding, ValueRange({inputIndex, lowIndex}));
      Value highVal = rewriter.createOrFold<tensor::ExtractOp>(
          loc, padding, ValueRange({inputIndex, highIndex}));

      lowVal = rewriter.createOrFold<IndexCastOp>(loc, rewriter.getIndexType(),
                                                  lowVal);
      highVal = rewriter.createOrFold<IndexCastOp>(loc, rewriter.getIndexType(),
                                                   highVal);

      lowValues.push_back(lowVal);
      highValues.push_back(highVal);
    }

    Value constant = rewriter.create<ConstantOp>(loc, constantAttr);

    auto newPadOp = linalg::PadTensorOp::createPadScalarOp(
        padOp.getType(), input, constant, lowValues, highValues, loc, rewriter);

    rewriter.replaceOp(padOp, newPadOp.getResult());
    return success();
  }
};

// Tosa argmax lowering represents the ArgMax op as an linalg.indexed_generic
// op, producing two output buffers.
//
// The first output buffer contains the index of the found maximum value. It is
// initialized to 0 and is resulting integer type.
//
// The second output buffer contains the maximum value found. It is initialized
// to the minimum representable value of the input element type. After being
// populated by indexed_generic, this buffer is disgarded as only the index is
// requested.
//
// The indexed_generic op updates both the maximum value and index if the
// current value exceeds the running max.
class ArgMaxConverter : public OpRewritePattern<tosa::ArgMaxOp> {
public:
  using OpRewritePattern<tosa::ArgMaxOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tosa::ArgMaxOp argmaxOp,
                                PatternRewriter &rewriter) const final {
    auto loc = argmaxOp.getLoc();
    Value input = argmaxOp.input();
    auto inputTy = input.getType().cast<ShapedType>();
    auto resultTy = argmaxOp.output().getType().cast<ShapedType>();
    auto inElementTy = inputTy.getElementType();
    auto outElementTy = resultTy.getElementType();
    int axis = argmaxOp.axis();
    auto resultMaxTy = RankedTensorType::get(resultTy.getShape(), inElementTy);

    if (!inputTy.hasStaticShape())
      return rewriter.notifyMatchFailure(
          argmaxOp,
          "tosa.arg_max to linalg.* requires statically shaped input");

    if (!outElementTy.isa<IntegerType>())
      return rewriter.notifyMatchFailure(
          argmaxOp,
          "tosa.arg_max to linalg.* requires integer-like result type");

    // First fill the output buffer for the index.
    auto initTensorIdx =
        rewriter
            .create<linalg::InitTensorOp>(loc, ArrayRef<Value>({}),
                                          resultTy.getShape(), outElementTy)
            .result();
    auto fillValueIdx = rewriter.create<ConstantOp>(
        loc, rewriter.getIntegerAttr(outElementTy, 0));
    auto filledTensorIdx =
        rewriter.create<linalg::FillOp>(loc, initTensorIdx, fillValueIdx)
            .result();

    // Second fill the output buffer for the running max.
    auto initTensorMax =
        rewriter
            .create<linalg::InitTensorOp>(loc, ArrayRef<Value>({}),
                                          resultTy.getShape(), inElementTy)
            .result();
    auto fillValueMaxAttr =
        createInitialValueForReduceOp(argmaxOp, inElementTy, rewriter);

    if (!fillValueMaxAttr)
      return rewriter.notifyMatchFailure(
          argmaxOp, "unsupported tosa.argmax element type");

    auto fillValueMax = rewriter.create<ConstantOp>(loc, fillValueMaxAttr);
    auto filledTensorMax =
        rewriter.create<linalg::FillOp>(loc, initTensorMax, fillValueMax)
            .result();

    // We need to reduce along the arg-max axis, with parallel operations along
    // the rest.
    SmallVector<StringRef, 4> iteratorTypes;
    iteratorTypes.resize(inputTy.getRank(), getParallelIteratorTypeName());
    iteratorTypes[axis] = getReductionIteratorTypeName();

    SmallVector<AffineExpr, 2> srcExprs;
    SmallVector<AffineExpr, 2> dstExprs;
    for (int i = 0, rank = inputTy.getRank(); i != rank; ++i) {
      srcExprs.push_back(mlir::getAffineDimExpr(i, rewriter.getContext()));
      if (axis != i)
        dstExprs.push_back(mlir::getAffineDimExpr(i, rewriter.getContext()));
    }

    bool didEncounterError = false;
    auto maps = AffineMap::inferFromExprList({srcExprs, dstExprs, dstExprs});
    auto linalgOp = rewriter.create<linalg::IndexedGenericOp>(
        loc, ArrayRef<Type>({resultTy, resultMaxTy}), input,
        ValueRange({filledTensorIdx, filledTensorMax}), maps, iteratorTypes,
        [&](OpBuilder &nestedBuilder, Location nestedLoc, ValueRange ivs,
            ValueRange blockArgs) {
          auto newValue = blockArgs[0];
          auto oldIndex = blockArgs[1];
          auto oldValue = blockArgs[2];

          Value newIndex = rewriter.create<IndexCastOp>(
              nestedLoc, oldIndex.getType(), ivs[axis]);

          Value predicate;
          if (inElementTy.isa<FloatType>()) {
            predicate = rewriter.create<mlir::CmpFOp>(
                nestedLoc, CmpFPredicate::OGT, newValue, oldValue);
          } else if (inElementTy.isa<IntegerType>()) {
            predicate = rewriter.create<mlir::CmpIOp>(
                nestedLoc, CmpIPredicate::sgt, newValue, oldValue);
          } else {
            didEncounterError = true;
            return;
          }

          auto resultMax = rewriter.create<mlir::SelectOp>(nestedLoc, predicate,
                                                           newValue, oldValue);
          auto resultIndex = rewriter.create<mlir::SelectOp>(
              nestedLoc, predicate, newIndex, oldIndex);
          nestedBuilder.create<linalg::YieldOp>(
              nestedLoc, ValueRange({resultIndex, resultMax}));
        });

    if (didEncounterError)
      return rewriter.notifyMatchFailure(
          argmaxOp, "unsupported tosa.argmax element type");

    rewriter.replaceOp(argmaxOp, linalgOp.getResult(0));
    return success();
  }
};

class GatherConverter : public OpConversionPattern<tosa::GatherOp> {
public:
  using OpConversionPattern<tosa::GatherOp>::OpConversionPattern;
  LogicalResult
  matchAndRewrite(tosa::GatherOp op, ArrayRef<Value> args,
                  ConversionPatternRewriter &rewriter) const final {
    auto input = args[0];
    auto indices = args[1];

    auto inputTy = input.getType().cast<ShapedType>();
    auto indicesTy = indices.getType().cast<ShapedType>();
    auto resultTy = op.getType().cast<ShapedType>();

    if (!inputTy.hasStaticShape() || !indicesTy.hasStaticShape())
      return rewriter.notifyMatchFailure(
          op, "require input type to have static shape");

    auto resultElementTy = resultTy.getElementType();

    auto loc = op.getLoc();

    auto initTensor =
        rewriter
            .create<linalg::InitTensorOp>(loc, ArrayRef<Value>{},
                                          resultTy.getShape(), resultElementTy)
            .result();

    SmallVector<AffineMap, 2> affineMaps = {
        AffineMap::get(
            /*dimCount=*/resultTy.getRank(), /*symbolCount=*/0,
            {rewriter.getAffineDimExpr(0), rewriter.getAffineDimExpr(1)},
            rewriter.getContext()),
        rewriter.getMultiDimIdentityMap(resultTy.getRank())};

    auto genericOp = rewriter.create<linalg::IndexedGenericOp>(
        loc, ArrayRef<Type>({resultTy}), ValueRange{indices},
        ValueRange{initTensor}, affineMaps,
        getNParallelLoopsAttrs(resultTy.getRank()),
        [&](OpBuilder &b, Location loc, ValueRange indices, ValueRange args) {
          auto indexValue = args[0];
          auto index0 = indices[0];
          Value index1 = rewriter.create<IndexCastOp>(
              loc, rewriter.getIndexType(), indexValue);
          auto index2 = indices[2];
          Value extract = rewriter.create<tensor::ExtractOp>(
              loc, input, ValueRange{index0, index1, index2});
          rewriter.create<linalg::YieldOp>(loc, extract);
        });
    rewriter.replaceOp(op, genericOp.getResult(0));
    return success();
  }
};

// Lowerings the TableOp to a series of gathers and numerica operations. This
// includes interpolation between the high/low values. For the I8 varient, this
// simplifies to a single gather operation.
class TableConverter : public OpRewritePattern<tosa::TableOp> {
public:
  using OpRewritePattern<tosa::TableOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tosa::TableOp op,
                                PatternRewriter &rewriter) const final {
    auto loc = op.getLoc();
    Value input = op.input();
    Value table = op.table();
    auto inputTy = input.getType().cast<ShapedType>();
    auto tableTy = table.getType().cast<ShapedType>();
    auto resultTy = op.getType().cast<ShapedType>();

    if (!inputTy.hasStaticShape())
      return rewriter.notifyMatchFailure(
          op, "require input type to have static shape");

    auto inputElementTy = inputTy.getElementType();
    auto tableElementTy = tableTy.getElementType();
    auto resultElementTy = resultTy.getElementType();

    auto initTensor =
        rewriter
            .create<linalg::InitTensorOp>(loc, ArrayRef<Value>{},
                                          resultTy.getShape(), resultElementTy)
            .result();

    SmallVector<AffineMap, 2> affineMaps = {
        rewriter.getMultiDimIdentityMap(resultTy.getRank()),
        rewriter.getMultiDimIdentityMap(resultTy.getRank())};

    auto genericOp = rewriter.create<linalg::GenericOp>(
        loc, resultTy, ValueRange({input}), ValueRange{initTensor}, affineMaps,
        getNParallelLoopsAttrs(resultTy.getRank()));
    rewriter.replaceOp(op, genericOp.getResult(0));

    {
      OpBuilder::InsertionGuard regionGuard(rewriter);
      Block *block =
          rewriter.createBlock(&genericOp.region(), genericOp.region().end(),
                               TypeRange({inputElementTy, resultElementTy}));

      auto inputValue = block->getArgument(0);
      rewriter.setInsertionPointToStart(block);
      if (inputElementTy.isInteger(8) && tableElementTy.isInteger(8) &&
          resultElementTy.isInteger(8)) {
        Value index = rewriter.create<IndexCastOp>(loc, rewriter.getIndexType(),
                                                   inputValue);
        Value extract =
            rewriter.create<tensor::ExtractOp>(loc, table, ValueRange{index});
        rewriter.create<linalg::YieldOp>(loc, extract);
        return success();
      }

      if (inputElementTy.isInteger(16) && tableElementTy.isInteger(16) &&
          resultElementTy.isInteger(32)) {
        Value extend = rewriter.create<SignExtendIOp>(
            loc, rewriter.getI32Type(), inputValue);

        auto offset =
            rewriter.create<ConstantOp>(loc, rewriter.getI32IntegerAttr(32768));
        auto seven =
            rewriter.create<ConstantOp>(loc, rewriter.getI32IntegerAttr(7));
        auto one =
            rewriter.create<ConstantOp>(loc, rewriter.getI32IntegerAttr(1));
        auto b1111111 =
            rewriter.create<ConstantOp>(loc, rewriter.getI32IntegerAttr(127));

        // Compute the index and fractional part from the input value:
        // value = value + 32768
        // index = value >> 7;
        // fraction = 0x01111111 & value
        auto extendAdd = rewriter.create<AddIOp>(loc, extend, offset);
        Value index =
            rewriter.create<UnsignedShiftRightOp>(loc, extendAdd, seven);
        Value fraction = rewriter.create<mlir::AndOp>(loc, extendAdd, b1111111);

        // Extract the base and next values from the table.
        // base = (int32_t) table[index];
        // next = (int32_t) table[index + 1];
        Value indexPlusOne = rewriter.create<AddIOp>(loc, index, one);

        index =
            rewriter.create<IndexCastOp>(loc, rewriter.getIndexType(), index);
        indexPlusOne = rewriter.create<IndexCastOp>(
            loc, rewriter.getIndexType(), indexPlusOne);

        Value base =
            rewriter.create<tensor::ExtractOp>(loc, table, ValueRange{index});
        Value next = rewriter.create<tensor::ExtractOp>(
            loc, table, ValueRange{indexPlusOne});

        base = rewriter.create<SignExtendIOp>(loc, rewriter.getI32Type(), base);
        next = rewriter.create<SignExtendIOp>(loc, rewriter.getI32Type(), next);

        // Use the fractional part to interpolate between the input values:
        // result = (base << 7) + (next - base) * fraction
        Value baseScaled = rewriter.create<ShiftLeftOp>(loc, base, seven);
        Value diff = rewriter.create<SubIOp>(loc, next, base);
        Value diffScaled = rewriter.create<MulIOp>(loc, diff, fraction);
        Value result = rewriter.create<AddIOp>(loc, baseScaled, diffScaled);

        rewriter.create<linalg::YieldOp>(loc, result);

        return success();
      }
    }

    return rewriter.notifyMatchFailure(
        op, "unable to create body for tosa.table op");
  }
};

template <typename SrcOp>
class Pool2dConverter : public OpRewritePattern<SrcOp> {
public:
  using OpRewritePattern<SrcOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(SrcOp op,
                                PatternRewriter &rewriter) const final {
    Location loc = op.getLoc();
    Value input = op.input();
    ShapedType inputTy = input.getType().cast<ShapedType>();
    Type inElementTy = inputTy.getElementType();

    ShapedType resultTy = op.getType().template cast<ShapedType>();
    Type outElementTy = inputTy.getElementType();

    if (!inputTy.hasStaticShape())
      return failure();

    // Determine what the initial value needs to be for the max pool op.
    Attribute initialAttr;
    if (isa<tosa::MaxPool2dOp>(op) && outElementTy.isF32())
      initialAttr = rewriter.getFloatAttr(
          outElementTy,
          APFloat::getLargest(
              outElementTy.cast<FloatType>().getFloatSemantics(), true));

    if (isa<tosa::MaxPool2dOp>(op) && outElementTy.isa<IntegerType>())
      initialAttr = rewriter.getIntegerAttr(
          outElementTy,
          APInt::getSignedMinValue(outElementTy.getIntOrFloatBitWidth()));

    if (isa<tosa::AvgPool2dOp>(op) && outElementTy.isa<FloatType>())
      initialAttr = rewriter.getZeroAttr(outElementTy);

    if (!initialAttr)
      return rewriter.notifyMatchFailure(
          op, "Unsupported initial value for tosa.maxpool_2d op");

    // Apply padding as necessary.
    llvm::SmallVector<int64_t> pad;
    pad.resize(2, 0);
    getValuesFromIntArrayAttribute(op.pad(), pad);
    pad.resize(pad.size() + 2, 0);
    input = applyPad(loc, input, pad, initialAttr, rewriter);

    Value initialValue = rewriter.create<ConstantOp>(loc, initialAttr);

    SmallVector<int64_t> kernel, stride;
    getValuesFromIntArrayAttribute(op.kernel(), kernel);
    getValuesFromIntArrayAttribute(op.stride(), stride);

    Attribute strideAttr = rewriter.getI64VectorAttr(stride);
    Attribute dilationAttr = rewriter.getI64VectorAttr({1, 1});
    int64_t kernelSize = kernel[0] * kernel[1];

    // Create the linalg op that performs pooling.
    Value initTensor = rewriter.create<linalg::InitTensorOp>(
        loc, resultTy.getShape(), resultTy.getElementType());

    Value filledInitTensor =
        rewriter.create<linalg::FillOp>(loc, initTensor, initialValue).result();

    Value fakeWindowDims =
        rewriter.create<linalg::InitTensorOp>(loc, kernel, outElementTy);

    auto createOp = [&](auto *typePtr) -> linalg::LinalgOp {
      return cast<linalg::LinalgOp>(
          rewriter
              .create<std::remove_pointer_t<decltype(typePtr)>>(
                  loc, ArrayRef<Type>{resultTy},
                  ValueRange{input, fakeWindowDims}, filledInitTensor,
                  dilationAttr, strideAttr)
              .getOperation());
    };

    if (isa<tosa::MaxPool2dOp>(op) && inElementTy.isF32()) {
      linalg::LinalgOp poolingOp =
          createOp(static_cast<linalg::PoolingNHWCMaxFOp *>(nullptr));
      rewriter.replaceOp(op, poolingOp->getResult(0));
      return success();
    }

    if (isa<tosa::MaxPool2dOp>(op) && inElementTy.isInteger(8)) {
      linalg::LinalgOp poolingOp =
          createOp(static_cast<linalg::PoolingNHWCMaxI8Op *>(nullptr));
      rewriter.replaceOp(op, poolingOp->getResult(0));
      return success();
    }

    if (isa<tosa::MaxPool2dOp>(op) && inElementTy.isInteger(16)) {
      linalg::LinalgOp poolingOp =
          createOp(static_cast<linalg::PoolingNHWCMaxI16Op *>(nullptr));
      rewriter.replaceOp(op, poolingOp->getResult(0));
      return success();
    }

    if (isa<tosa::MaxPool2dOp>(op) && inElementTy.isInteger(32)) {
      linalg::LinalgOp poolingOp =
          createOp(static_cast<linalg::PoolingNHWCMaxI32Op *>(nullptr));
      rewriter.replaceOp(op, poolingOp->getResult(0));
      return success();
    }

    if (isa<tosa::AvgPool2dOp>(op) && inElementTy.isF32()) {
      linalg::LinalgOp poolingOp =
          createOp(static_cast<linalg::PoolingNHWCSumFOp *>(nullptr));
      auto constAttr = DenseElementsAttr::get(
          resultTy, static_cast<float>(1.0 / kernelSize));
      auto constant = rewriter.create<ConstantOp>(loc, constAttr);
      auto mul = rewriter.create<tosa::MulOp>(
          loc, resultTy, poolingOp->getResult(0), constant, 0);
      rewriter.replaceOp(op, mul.output());
      return success();
    }

    return failure();
  }
};

} // namespace

void mlir::tosa::populateTosaToLinalgOnTensorsConversionPatterns(
    RewritePatternSet *patterns) {
  patterns->add<
      // clang-format off
      PointwiseConverter<tosa::AddOp>,
      PointwiseConverter<tosa::SubOp>,
      PointwiseConverter<tosa::MulOp>,
      PointwiseConverter<tosa::NegateOp>,
      PointwiseConverter<tosa::PowOp>,
      PointwiseConverter<tosa::ReciprocalOp>,
      PointwiseConverter<tosa::RsqrtOp>,
      PointwiseConverter<tosa::LogOp>,
      PointwiseConverter<tosa::ExpOp>,
      PointwiseConverter<tosa::AbsOp>,
      PointwiseConverter<tosa::TanhOp>,
      PointwiseConverter<tosa::BitwiseAndOp>,
      PointwiseConverter<tosa::BitwiseOrOp>,
      PointwiseConverter<tosa::BitwiseNotOp>,
      PointwiseConverter<tosa::BitwiseXorOp>,
      PointwiseConverter<tosa::LogicalAndOp>,
      PointwiseConverter<tosa::LogicalNotOp>,
      PointwiseConverter<tosa::LogicalOrOp>,
      PointwiseConverter<tosa::LogicalXorOp>,
      PointwiseConverter<tosa::CastOp>,
      PointwiseConverter<tosa::LogicalLeftShiftOp>,
      PointwiseConverter<tosa::LogicalRightShiftOp>,
      PointwiseConverter<tosa::ArithmeticRightShiftOp>,
      PointwiseConverter<tosa::SelectOp>,
      PointwiseConverter<tosa::GreaterOp>,
      PointwiseConverter<tosa::GreaterEqualOp>,
      PointwiseConverter<tosa::EqualOp>,
      PointwiseConverter<tosa::MaximumOp>,
      PointwiseConverter<tosa::MinimumOp>,
      PointwiseConverter<tosa::CeilOp>,
      PointwiseConverter<tosa::FloorOp>,
      PointwiseConverter<tosa::ClampOp>,
      PointwiseConverter<tosa::ReluNOp>,
      PointwiseConverter<tosa::SigmoidOp>,
      IdentityNConverter<tosa::IdentityOp>,
      IdentityNConverter<tosa::IdentityNOp>,
      ReduceConverter<tosa::ReduceAllOp>,
      ReduceConverter<tosa::ReduceAnyOp>,
      ReduceConverter<tosa::ReduceMinOp>,
      ReduceConverter<tosa::ReduceMaxOp>,
      ReduceConverter<tosa::ReduceSumOp>,
      ReduceConverter<tosa::ReduceProdOp>,
      ArgMaxConverter,
      ConcatConverter,
      ConvConverter<tosa::Conv2DOp>,
      ConvConverter<tosa::DepthwiseConv2DOp>,
      GatherConverter,
      PadConverter,
      ReshapeConverter,
      RescaleConverter,
      ResizeConverter,
      ReverseConverter,
      TableConverter,
      TileConverter,
      TransposeConverter,
      MatMulConverter,
      Pool2dConverter<tosa::AvgPool2dOp>,
      Pool2dConverter<tosa::MaxPool2dOp>,
      FullyConnectedConverter>(patterns->getContext());
  // clang-format on
}

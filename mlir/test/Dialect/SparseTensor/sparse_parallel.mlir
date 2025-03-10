// RUN: mlir-opt %s -sparsification="parallelization-strategy=0" | \
// RUN:   FileCheck %s --check-prefix=CHECK-PAR0
// RUN: mlir-opt %s -sparsification="parallelization-strategy=1" | \
// RUN:   FileCheck %s --check-prefix=CHECK-PAR1
// RUN: mlir-opt %s -sparsification="parallelization-strategy=2" | \
// RUN:   FileCheck %s --check-prefix=CHECK-PAR2
// RUN: mlir-opt %s -sparsification="parallelization-strategy=3" | \
// RUN:   FileCheck %s --check-prefix=CHECK-PAR3
// RUN: mlir-opt %s -sparsification="parallelization-strategy=4" | \
// RUN:   FileCheck %s --check-prefix=CHECK-PAR4

#trait_dd = {
  indexing_maps = [
    affine_map<(i,j) -> (i,j)>,  // A
    affine_map<(i,j) -> (i,j)>   // X (out)
  ],
  sparse = [
    [ "D", "D" ],  // A
    [ "D", "D" ]   // X
  ],
  iterator_types = ["parallel", "parallel"],
  doc = "X(i,j) = A(i,j) * SCALE"
}

//
// CHECK-PAR0-LABEL: func @scale_dd
// CHECK-PAR0:         scf.for
// CHECK-PAR0:           scf.for
// CHECK-PAR0:         return
//
// CHECK-PAR1-LABEL: func @scale_dd
// CHECK-PAR1:         scf.parallel
// CHECK-PAR1:           scf.for
// CHECK-PAR1:         return
//
// CHECK-PAR2-LABEL: func @scale_dd
// CHECK-PAR2:         scf.parallel
// CHECK-PAR2:           scf.for
// CHECK-PAR2:         return
//
// CHECK-PAR3-LABEL: func @scale_dd
// CHECK-PAR3:         scf.parallel
// CHECK-PAR3:           scf.parallel
// CHECK-PAR3:         return
//
// CHECK-PAR4-LABEL: func @scale_dd
// CHECK-PAR4:         scf.parallel
// CHECK-PAR4:           scf.parallel
// CHECK-PAR4:         return
//
func @scale_dd(%scale: f32, %arga: tensor<?x?xf32>, %argx: tensor<?x?xf32>) -> tensor<?x?xf32> {
  %0 = linalg.generic #trait_dd
     ins(%arga: tensor<?x?xf32>)
    outs(%argx: tensor<?x?xf32>) {
      ^bb(%a: f32, %x: f32):
        %0 = mulf %a, %scale : f32
        linalg.yield %0 : f32
  } -> tensor<?x?xf32>
  return %0 : tensor<?x?xf32>
}

#trait_ss = {
  indexing_maps = [
    affine_map<(i,j) -> (i,j)>,  // A
    affine_map<(i,j) -> (i,j)>   // X (out)
  ],
  sparse = [
    [ "S", "S" ],  // A
    [ "D", "D" ]   // X
  ],
  iterator_types = ["parallel", "parallel"],
  doc = "X(i,j) = A(i,j) * SCALE"
}

//
// CHECK-PAR0-LABEL: func @scale_ss
// CHECK-PAR0:         scf.for
// CHECK-PAR0:           scf.for
// CHECK-PAR0:         return
//
// CHECK-PAR1-LABEL: func @scale_ss
// CHECK-PAR1:         scf.for
// CHECK-PAR1:           scf.for
// CHECK-PAR1:         return
//
// CHECK-PAR2-LABEL: func @scale_ss
// CHECK-PAR2:         scf.parallel
// CHECK-PAR2:           scf.for
// CHECK-PAR2:         return
//
// CHECK-PAR3-LABEL: func @scale_ss
// CHECK-PAR3:         scf.for
// CHECK-PAR3:           scf.for
// CHECK-PAR3:         return
//
// CHECK-PAR4-LABEL: func @scale_ss
// CHECK-PAR4:         scf.parallel
// CHECK-PAR4:           scf.parallel
// CHECK-PAR4:         return
//
func @scale_ss(%scale: f32, %arga: tensor<?x?xf32>, %argx: tensor<?x?xf32>) -> tensor<?x?xf32> {
  %0 = linalg.generic #trait_ss
     ins(%arga: tensor<?x?xf32>)
    outs(%argx: tensor<?x?xf32>) {
      ^bb(%a: f32, %x: f32):
        %0 = mulf %a, %scale : f32
        linalg.yield %0 : f32
  } -> tensor<?x?xf32>
  return %0 : tensor<?x?xf32>
}

#trait_matvec = {
  indexing_maps = [
    affine_map<(i,j) -> (i,j)>,  // A
    affine_map<(i,j) -> (j)>,    // b
    affine_map<(i,j) -> (i)>     // x (out)
  ],
  sparse = [
    [ "D", "S" ],  // A
    [ "D" ],       // b
    [ "D" ]        // x
  ],
  iterator_types = ["parallel", "reduction"],
  doc = "x(i) += A(i,j) * b(j)"
}

//
// CHECK-PAR0-LABEL: func @matvec
// CHECK-PAR0:         scf.for
// CHECK-PAR0:           scf.for
// CHECK-PAR0:         return
//
// CHECK-PAR1-LABEL: func @matvec
// CHECK-PAR1:         scf.parallel
// CHECK-PAR1:           scf.for
// CHECK-PAR1:         return
//
// CHECK-PAR2-LABEL: func @matvec
// CHECK-PAR2:         scf.parallel
// CHECK-PAR2:           scf.for
// CHECK-PAR2:         return
//
// CHECK-PAR3-LABEL: func @matvec
// CHECK-PAR3:         scf.parallel
// CHECK-PAR3:           scf.for
// CHECK-PAR3:         return
//
// CHECK-PAR4-LABEL: func @matvec
// CHECK-PAR4:         scf.parallel
// CHECK-PAR4:           scf.for
// CHECK-PAR4:         return
//
func @matvec(%argA: tensor<16x32xf32>, %argb: tensor<32xf32>, %argx: tensor<16xf32>) -> tensor<16xf32> {
  %0 = linalg.generic #trait_matvec
      ins(%argA, %argb : tensor<16x32xf32>, tensor<32xf32>)
     outs(%argx: tensor<16xf32>) {
    ^bb(%A: f32, %b: f32, %x: f32):
      %0 = mulf %A, %b : f32
      %1 = addf %0, %x : f32
      linalg.yield %1 : f32
  } -> tensor<16xf32>
  return %0 : tensor<16xf32>
}

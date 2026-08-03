; Test that -lto-split-partitions=N partitions the module by call graph for
; regular LTO. A shared function used by two mutually-independent callers is
; split so that each caller lands in a different partition; the shared
; function is defined in one partition and referenced from the other.

; REQUIRES: aarch64-registered-target

; RUN: llvm-as %s -o %t.bc
; RUN: llvm-lto2 run -r %t.bc,shared_fn,plx -r %t.bc,caller_a,plx \
; RUN:   -r %t.bc,caller_b,plx -o %t %t.bc -lto-split-partitions=2
; RUN: llvm-nm %t.0 | FileCheck %s --check-prefix=CHECK0
; RUN: llvm-nm %t.1 | FileCheck %s --check-prefix=CHECK1

; CHECK0-DAG: T caller_b
; CHECK0-DAG: T shared_fn

; CHECK1-DAG: T caller_a
; CHECK1-DAG: U shared_fn

target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-unknown-linux-gnu"

define i32 @shared_fn(i32 %n) noinline nounwind {
entry:
  %r = add i32 %n, 1
  ret i32 %r
}

define i32 @caller_a(i32 %n) noinline {
  %r = call i32 @shared_fn(i32 %n)
  ret i32 %r
}

define i32 @caller_b(i32 %n) noinline {
  %r = call i32 @shared_fn(i32 %n)
  ret i32 %r
}

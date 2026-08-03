; Test that -lto-split-partitions=N partitions the module by call graph for
; ThinLTO. A local symbol shared by two independent callers is promoted to
; external linkage and renamed with the ThinLTO "name.llvm.<hash>"
; convention; it is defined in one split partition and referenced from the
; other, so the two callers can resolve it after partitions are merged.

; REQUIRES: aarch64-registered-target

; RUN: opt -module-summary %s -o %t.bc
; RUN: llvm-lto2 run -r %t.bc,caller_a,plx -r %t.bc,caller_b,plx -o %t %t.bc \
; RUN:   -lto-split-partitions=2
; RUN: llvm-nm %t.0 | FileCheck %s --check-prefix=CHECK0
; RUN: llvm-nm %t.1 | FileCheck %s --check-prefix=CHECK1

; CHECK0-DAG: T caller_b
; CHECK0-DAG: T {{promoted_internal[.]llvm[.][0-9a-f]+}}

; CHECK1-DAG: T caller_a
; CHECK1-DAG: U {{promoted_internal[.]llvm[.][0-9a-f]+}}

target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-unknown-linux-gnu"

define internal i32 @promoted_internal(i32 %n) noinline nounwind {
entry:
  %r = add i32 %n, 1
  ret i32 %r
}

define i32 @caller_a(i32 %n) noinline {
  %r = call i32 @promoted_internal(i32 %n)
  ret i32 %r
}

define i32 @caller_b(i32 %n) noinline {
  %r = call i32 @promoted_internal(i32 %n)
  ret i32 %r
}

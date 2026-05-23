; Test that internal symbols referenced by multiple call graph partitions are
; promoted during module splitting.
;
; RUN: llvm-split -enable-split-module-CG=true -j2 -o %t. %s
; RUN: llvm-dis %t.0 -o - | FileCheck %s --check-prefix=PART0
; RUN: llvm-dis %t.1 -o - | FileCheck %s --check-prefix=PART1

; PART0-DAG: define hidden void @promoted_internal()
; PART0-DAG: declare void @caller_a()
; PART0-DAG: define void @caller_b()

; PART1-DAG: define available_externally hidden void @promoted_internal()
; PART1-DAG: define void @caller_a()
; PART1-DAG: declare void @caller_b()

define internal void @promoted_internal() {
entry:
  ret void
}

define void @caller_a() {
entry:
  call void @promoted_internal()
  ret void
}

define void @caller_b() {
entry:
  call void @promoted_internal()
  ret void
}

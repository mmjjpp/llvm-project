; REQUIRES: aarch64-registered-target

; RUN: opt -thinlto-bc -o %t.o %s
; RUN: llvm-lto2 run -thinlto-distributed-indexes %t.o \
; RUN:   -o %t.index \
; RUN:   -r=%t.o,caller_a,px \
; RUN:   -r=%t.o,caller_b,px

; RUN: %clang_cc1 -triple aarch64-unknown-linux-gnu \
; RUN:   -emit-obj -fthinlto-index=%t.o.thinlto.bc \
; RUN:   -o %t.split.o -x ir %t.o \
; RUN:   -mllvm -thinlto-split=true \
; RUN:   -mllvm -thinlto-split-partitions=2 \
; RUN:   -mllvm -thinlto-split-module-size-threshold=0 \
; RUN:   -mllvm -thinlto-split-module-size-rate-threshold=2.0 \
; RUN:   -thinlto-split-output-list=%t.split.rsp
; RUN: FileCheck %s --check-prefix=SPLIT-RSP --input-file=%t.split.rsp
; RUN: llvm-nm %t.split.o.thinlto-split.0.o | FileCheck %s --check-prefix=NM0
; RUN: llvm-nm %t.split.o.thinlto-split.1.o | FileCheck %s --check-prefix=NM1

; RUN: %clang_cc1 -triple aarch64-unknown-linux-gnu \
; RUN:   -emit-obj -fthinlto-index=%t.o.thinlto.bc \
; RUN:   -o %t.skip.o -x ir %t.o \
; RUN:   -mllvm -thinlto-split=true \
; RUN:   -mllvm -thinlto-split-partitions=2 \
; RUN:   -thinlto-split-output-list=%t.skip.rsp
; RUN: FileCheck %s --check-prefix=SKIP-RSP --input-file=%t.skip.rsp

; SPLIT-RSP: {{.*}}.split.o.thinlto-split.0.o
; SPLIT-RSP-NEXT: {{.*}}.split.o.thinlto-split.1.o
; SPLIT-RSP-NOT: {{.*}}.split.o{{$}}
; SPLIT-RSP-NOT: thinlto-split.2.o
; SPLIT-RSP-NOT: {{.*}}.merged.o
; SPLIT-RSP-NOT: 4294967295

; SKIP-RSP: {{.*}}.skip.o.thinlto-split.0.o
; SKIP-RSP-NOT: {{.*}}.skip.o{{$}}
; SKIP-RSP-NOT: thinlto-split.1.o
; SKIP-RSP-NOT: 4294967295

; Verify that the user-specified -o path is empty (0 bytes) when split codegen
; is active. In the split path, cc1 replaces the original output stream with a
; null stream so no object content lands in the -o file. The driver's ld.lld -r
; merge produces the final -o content separately.
; RUN: wc -c %t.split.o | FileCheck %s --check-prefix=EMPTY-SPLIT
; EMPTY-SPLIT: 0
; RUN: wc -c %t.skip.o | FileCheck %s --check-prefix=EMPTY-SKIP
; EMPTY-SKIP: 0

; NM0: T caller_b
; NM0: T {{.*shared[._][0-9a-f]+.*}}
; NM0-NOT: T shared{{$}}

; NM1: T caller_a
; NM1: U {{.*shared[._][0-9a-f]+.*}}
; NM1-NOT: T shared{{$}}

target triple = "aarch64-unknown-linux-gnu"

define internal void @shared() {
entry:
  ret void
}

define void @caller_a() {
entry:
  call void @shared()
  ret void
}

define void @caller_b() {
entry:
  call void @shared()
  ret void
}

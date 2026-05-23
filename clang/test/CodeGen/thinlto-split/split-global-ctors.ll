; REQUIRES: aarch64-registered-target
;
; Regression coverage for ThinLTO split backend partitioning of global
; constructors. The supported split-codegen invocation path is the clang
; distributed backend compile followed by an explicit ld.lld -r merge.

;--- ctors.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-unknown-linux-gnu"

$g1 = comdat any
$g2 = comdat any
$g3 = comdat any
$g4 = comdat any
$g5 = comdat any
$g6 = comdat any

@g1 = weak_odr hidden global i32 0, comdat, align 4
@g2 = weak_odr hidden global i32 0, comdat, align 4
@g3 = weak_odr hidden global i32 0, comdat, align 4
@g4 = weak_odr hidden global i32 0, comdat, align 4
@g5 = weak_odr hidden global i32 0, comdat, align 4
@g6 = weak_odr hidden global i32 0, comdat, align 4

@llvm.used = appending global [6 x ptr] [
  ptr @g1, ptr @g2, ptr @g3, ptr @g4, ptr @g5, ptr @g6
], section "llvm.metadata"

@llvm.global_ctors = appending global [7 x { i32, ptr, ptr }] [
  { i32, ptr, ptr } { i32 65535, ptr @ctor1, ptr @g1 },
  { i32, ptr, ptr } { i32 65535, ptr @ctor2, ptr @g2 },
  { i32, ptr, ptr } { i32 65535, ptr @ctor3, ptr @g3 },
  { i32, ptr, ptr } { i32 65535, ptr @ctor4, ptr @g4 },
  { i32, ptr, ptr } { i32 65535, ptr @ctor5, ptr @g5 },
  { i32, ptr, ptr } { i32 65535, ptr @ctor6, ptr @g6 },
  { i32, ptr, ptr } { i32 65535, ptr @_GLOBAL__sub_I_mod, ptr null }
]

define internal void @ctor1() section ".text.startup" comdat($g1) {
  store volatile i32 1, ptr @g1, align 4
  ret void
}

define internal void @ctor2() section ".text.startup" comdat($g2) {
  store volatile i32 2, ptr @g2, align 4
  ret void
}

define internal void @ctor3() section ".text.startup" comdat($g3) {
  store volatile i32 3, ptr @g3, align 4
  ret void
}

define internal void @ctor4() section ".text.startup" comdat($g4) {
  store volatile i32 4, ptr @g4, align 4
  ret void
}

define internal void @ctor5() section ".text.startup" comdat($g5) {
  store volatile i32 5, ptr @g5, align 4
  ret void
}

define internal void @ctor6() section ".text.startup" comdat($g6) {
  store volatile i32 6, ptr @g6, align 4
  ret void
}

define internal void @_GLOBAL__sub_I_mod() section ".text.startup" {
  call void asm sideeffect "", ""()
  ret void
}

define void @rootA() {
  call void @ctor1()
  call void @ctor2()
  call void @ctor3()
  ret void
}

define void @rootB() {
  call void @ctor4()
  call void @ctor5()
  call void @ctor6()
  ret void
}

;--- comdat.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-unknown-linux-gnu"

$grp = comdat any
$keygrp = comdat any

@g = weak_odr hidden global i32 0, comdat($grp), align 4
@key = weak_odr hidden global i32 0, comdat($keygrp), align 4
@llvm.used = appending global [2 x ptr] [ptr @g, ptr @key], section "llvm.metadata"

@llvm.global_ctors = appending global [2 x { i32, ptr, ptr }] [
  { i32, ptr, ptr } { i32 65535, ptr @ctor, ptr @g },
  { i32, ptr, ptr } { i32 65535, ptr @ctor_alias, ptr @key }
]

@ctor_alias = internal alias void (), ptr @ctor_with_key

define internal void @helper() noinline optnone comdat($grp) {
  store volatile i32 7, ptr @g, align 4
  ret void
}

define internal void @key_owner() noinline optnone comdat($keygrp) {
  store volatile i32 8, ptr @key, align 4
  ret void
}

define internal void @ctor() section ".text.startup" comdat($grp) {
  store volatile i32 9, ptr @g, align 4
  ret void
}

define internal void @ctor_with_key() section ".text.startup" {
  store volatile i32 10, ptr @key, align 4
  ret void
}

define void @rootA() {
  call void @helper()
  call void @key_owner()
  ret void
}

define void @rootB() {
  call void asm sideeffect "", ""()
  ret void
}

;--- checks
; RUN: rm -rf %t.dir
; RUN: split-file %s %t.dir

; RUN: opt --thinlto-bc --thinlto-split-lto-unit -o %t.ctors.o %t.dir/ctors.ll
; RUN: llvm-lto2 run -thinlto-distributed-indexes %t.ctors.o \
; RUN:   -o %t.ctors.index \
; RUN:   -r=%t.ctors.o,rootA,px \
; RUN:   -r=%t.ctors.o,rootB,px \
; RUN:   -r=%t.ctors.o,g1,px \
; RUN:   -r=%t.ctors.o,g2,px \
; RUN:   -r=%t.ctors.o,g3,px \
; RUN:   -r=%t.ctors.o,g4,px \
; RUN:   -r=%t.ctors.o,g5,px \
; RUN:   -r=%t.ctors.o,g6,px
; RUN: %clang_cc1 -triple aarch64-unknown-linux-gnu \
; RUN:   -emit-obj -fthinlto-index=%t.ctors.o.thinlto.bc \
; RUN:   -thinlto-split-output-list=%t.ctors.rsp \
; RUN:   -save-temps=obj -o %t.ctors.backend.o -x ir %t.ctors.o \
; RUN:   -mllvm -thinlto-split=true \
; RUN:   -mllvm -thinlto-split-partitions=2 \
; RUN:   -mllvm -thinlto-split-module-size-threshold=0 \
; RUN:   -mllvm -thinlto-split-module-size-rate-threshold=2.0
; RUN: llvm-dis %t.ctors.backend.o.ctors.ll.0.5.precodegen.bc -o %t.ctors.0.ll
; RUN: llvm-dis %t.ctors.backend.o.ctors.ll.1.5.precodegen.bc -o %t.ctors.1.ll
; RUN: cat %t.ctors.0.ll %t.ctors.1.ll | FileCheck %s --check-prefix=OWNER
; RUN: ld.lld -r -o %t.ctors.merged.o @%t.ctors.rsp
; RUN: ld.lld %t.ctors.merged.o -shared -o %t.ctors.so
; RUN: llvm-readelf -SW %t.ctors.so | FileCheck %s --check-prefix=FINAL

; OWNER: @llvm.global_ctors = appending global [7 x { i32, ptr, ptr }]
; OWNER-DAG: define hidden void @ctor1.{{.*}}(){{.*}}section ".text.startup"
; OWNER-DAG: define hidden void @ctor2.{{.*}}(){{.*}}section ".text.startup"
; OWNER-DAG: define hidden void @ctor3.{{.*}}(){{.*}}section ".text.startup"
; OWNER-DAG: define hidden void @ctor4.{{.*}}(){{.*}}section ".text.startup"
; OWNER-DAG: define hidden void @ctor5.{{.*}}(){{.*}}section ".text.startup"
; OWNER-DAG: define hidden void @ctor6.{{.*}}(){{.*}}section ".text.startup"
; OWNER-DAG: define hidden void @_GLOBAL__sub_I_mod.{{.*}}(){{.*}}section ".text.startup"
; FINAL: .init_array{{.*}}000038{{.*}}

; RUN: opt --thinlto-bc --thinlto-split-lto-unit -o %t.comdat.o %t.dir/comdat.ll
; RUN: llvm-lto2 run -thinlto-distributed-indexes %t.comdat.o \
; RUN:   -o %t.comdat.index \
; RUN:   -r=%t.comdat.o,rootA,px \
; RUN:   -r=%t.comdat.o,rootB,px \
; RUN:   -r=%t.comdat.o,g,px \
; RUN:   -r=%t.comdat.o,key,px
; RUN: %clang_cc1 -triple aarch64-unknown-linux-gnu \
; RUN:   -emit-obj -fthinlto-index=%t.comdat.o.thinlto.bc \
; RUN:   -thinlto-split-output-list=%t.comdat.rsp \
; RUN:   -save-temps=obj -o %t.comdat.backend.o -x ir %t.comdat.o \
; RUN:   -mllvm -thinlto-split=true \
; RUN:   -mllvm -thinlto-split-partitions=2 \
; RUN:   -mllvm -thinlto-split-module-size-threshold=0 \
; RUN:   -mllvm -thinlto-split-module-size-rate-threshold=2.0
; RUN: llvm-dis %t.comdat.backend.o.comdat.ll.0.5.precodegen.bc -o %t.comdat.0.ll
; RUN: llvm-dis %t.comdat.backend.o.comdat.ll.1.5.precodegen.bc -o %t.comdat.1.ll
; RUN: cat %t.comdat.0.ll %t.comdat.1.ll | FileCheck %s --check-prefix=COMDAT-OWNER
; RUN: ld.lld -r -o %t.comdat.merged.o @%t.comdat.rsp
; RUN: ld.lld %t.comdat.merged.o -shared -o %t.comdat.so
; RUN: llvm-readelf -SW %t.comdat.so | FileCheck %s --check-prefix=COMDAT-FINAL
; RUN: llvm-readelf -Ws %t.comdat.so | FileCheck %s --check-prefix=COMDAT-SYMS

; COMDAT-OWNER: $grp = comdat any
; COMDAT-OWNER-DAG: @g = weak_odr hidden global i32 0, comdat($grp), align 4
; COMDAT-OWNER-DAG: @key = weak_odr hidden global i32 0, comdat($keygrp), align 4
; COMDAT-OWNER: @llvm.global_ctors = appending global [2 x { i32, ptr, ptr }]
; COMDAT-OWNER-DAG: define {{.*}} @helper{{.*}}(){{.*}}comdat($grp)
; COMDAT-OWNER-DAG: define {{.*}} @ctor{{.*}}(){{.*}}comdat($grp)
; COMDAT-OWNER-DAG: define {{.*}} @key_owner{{.*}}(){{.*}}comdat($keygrp)
; COMDAT-OWNER-DAG: define {{.*}} @ctor_with_key{{.*}}(){{.*}}section ".text.startup"
; COMDAT-FINAL: .init_array{{.*}}000010{{.*}}
; COMDAT-SYMS-NOT: UND helper
; COMDAT-SYMS-NOT: UND key_owner

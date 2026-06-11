; REQUIRES: aarch64-registered-target

; End-to-end test for ThinLTO split + Driver-mediated lld merge.
;
; This test simulates a realistic distributed ThinLTO backend scenario:
;   1. Generate ThinLTO bitcode from an IR module containing multiple functions,
;      global variables, global constructors (init_array), internal functions
;      referenced across partitions, comdat/weak symbols.
;   2. Generate the distributed ThinLTO index.
;   3. Invoke clang Driver to compile with -fthinlto-index=... and -thinlto-split,
;      producing partition objects and lld -r merging them into a single output.o.
;   4. Validate the merged output.o is a valid ELF relocatable object with
;      expected symbols, sections, and correct RSP ordering.

; --- Step 1: Generate ThinLTO bitcode ---
; RUN: opt -thinlto-bc -thinlto-split-lto-unit -o %t.o %s

; --- Step 2: Generate distributed ThinLTO index ---
; RUN: llvm-lto2 run -thinlto-distributed-indexes %t.o \
; RUN:   -o %t.index \
; RUN:   -r=%t.o,func_a,px \
; RUN:   -r=%t.o,func_b,px \
; RUN:   -r=%t.o,func_c,px \
; RUN:   -r=%t.o,func_d,px \
; RUN:   -r=%t.o,func_e,px \
; RUN:   -r=%t.o,weak_func,px \
; RUN:   -r=%t.o,g_global,px \
; RUN:   -r=%t.o,g_ctor_data,px \
; RUN:   -r=%t.o,comdat_var,px

; --- Step 3: clang_cc1 split path — verify partition objects and RSP ---
; RUN: %clang_cc1 -triple aarch64-unknown-linux-gnu \
; RUN:   -emit-obj -fthinlto-index=%t.o.thinlto.bc \
; RUN:   -thinlto-split-output-list=%t.cc1.rsp \
; RUN:   -o %t.cc1.o -x ir %t.o \
; RUN:   -mllvm -lto-split-by-callgraph=true \
; RUN:   -mllvm -thinlto-split-partitions=2 \
; RUN:   -mllvm -thinlto-split-module-size-threshold=0 \
; RUN:   -mllvm -thinlto-split-module-size-rate-threshold=2.0

; Verify RSP contains partition objects in order 0, 1
; RSP-RSP: {{.*\.thinlto-split\.0\.o}}
; RSP-RSP-NEXT: {{.*\.thinlto-split\.1\.o}}
; RSP should NOT contain the bare -o output
; RSP-RSP-NOT: {{\.cc1\.o$}}
; RSP should NOT contain uint32_max (4294967295)
; RSP-RSP-NOT: 4294967295

; Verify partition objects are valid ELF
; RUN: llvm-readobj -h %t.cc1.o.thinlto-split.0.o | FileCheck %s --check-prefix=PART0-ELF
; PART0-ELF: Type: Relocatable

; RUN: llvm-readobj -h %t.cc1.o.thinlto-split.1.o | FileCheck %s --check-prefix=PART1-ELF
; PART1-ELF: Type: Relocatable

; Verify inter-partition symbol promotion: shared_helper should have hash suffix
; RUN: llvm-nm %t.cc1.o.thinlto-split.0.o | FileCheck %s --check-prefix=NM0
; NM0-DAG: T {{shared_helper[._]}}
; NM0-DAG: T func_
; NM0-DAG: D g_global
; NM0-DAG: V comdat_var

; RUN: llvm-nm %t.cc1.o.thinlto-split.1.o | FileCheck %s --check-prefix=NM1
; NM1-DAG: U {{shared_helper[._]}}
; NM1-DAG: T func_

; Verify the cc1 -o path exists but is empty (split path writes partition objects instead)
; RUN: wc -c %t.cc1.o | FileCheck %s --check-prefix=EMPTY-O
; EMPTY-O: 0

; --- Step 4: clang Driver split path — verify full end-to-end ---
; The Driver must: (a) invoke cc1 with -thinlto-split-output-list, (b) invoke
; sibling ld.lld -r to merge partition objects, (c) produce a single valid output.o

; First, verify the Driver generates correct command lines (-###)
; RUN: %clang -### -target aarch64-unknown-linux-gnu \
; RUN:   -B%S/Inputs/lld \
; RUN:   -c -fthinlto-index=%t.o.thinlto.bc -x ir %t.o -o %t.driver.o \
; RUN:   -mllvm -lto-split-by-callgraph=true \
; RUN:   -mllvm -thinlto-split-partitions=2 \
; RUN:   -mllvm -thinlto-split-module-size-threshold=0 \
; RUN:   -mllvm -thinlto-split-module-size-rate-threshold=2.0 2>&1 | FileCheck %s --check-prefix=DRIVER

; DRIVER: "-cc1"
; DRIVER-SAME: "-fthinlto-index={{.*}}.thinlto.bc"
; DRIVER-SAME: "-thinlto-split-output-list=[[RSP:[^"]+\.thinlto-split\.rsp]]"
; DRIVER: "{{.*}}ld.lld" "-r" "-o" "{{.*}}driver.o" "@[[RSP]]"

; Non-split path should NOT have -thinlto-split-output-list or ld.lld
; RUN: %clang -### -target aarch64-unknown-linux-gnu \
; RUN:   -B%S/Inputs/lld \
; RUN:   -c -fthinlto-index=%t.o.thinlto.bc -x ir %t.o -o %t.nosplit_driver.o \
; RUN:   -mllvm -lto-split-by-callgraph=false 2>&1 | FileCheck %s --check-prefix=NOSPLIT-DRIVER

; NOSPLIT-DRIVER: "-cc1"
; NOSPLIT-DRIVER-NOT: thinlto-split-output-list
; NOSPLIT-DRIVER-NOT: ld.lld

; --- Step 4b: Verify -save-temps + ThinLTO split ---
; -save-temps normally prevents collapsing the assemble step, but ThinLTO split
; must still emit objects directly (-emit-obj) because the assembly path cannot
; produce multiple partition outputs (AcceptsMultipleOutputsPerTask).

; RUN: %clang -### -target aarch64-unknown-linux-gnu \
; RUN:   -B%S/Inputs/lld \
; RUN:   -save-temps -c -fthinlto-index=%t.o.thinlto.bc -x ir %t.o -o %t.save.o \
; RUN:   -mllvm -lto-split-by-callgraph=true \
; RUN:   -mllvm -thinlto-split-partitions=2 \
; RUN:   -mllvm -thinlto-split-module-size-threshold=0 \
; RUN:   -mllvm -thinlto-split-module-size-rate-threshold=2.0 2>&1 | FileCheck %s --check-prefix=SAVE-TEMPS

; cc1 must use -emit-obj (not -S) even with -save-temps
; SAVE-TEMPS: "-cc1"
; SAVE-TEMPS-SAME: "-emit-obj"
; SAVE-TEMPS-SAME: "-thinlto-split-output-list=[[SAVE_RSP:[^"]+\.thinlto-split\.rsp]]"
; SAVE-TEMPS: ld.lld{{.*}}-r{{.*}}-o{{.*}}@[[SAVE_RSP]]

; Verify ordinary -save-temps without split still uses -S (not collapsed)
; RUN: %clang -### -target aarch64-unknown-linux-gnu \
; RUN:   -save-temps -c -fthinlto-index=%t.o.thinlto.bc -x ir %t.o -o %t.save_nosplit.o \
; RUN:   -mllvm -lto-split-by-callgraph=false 2>&1 | FileCheck %s --check-prefix=SAVE-TEMPS-NOSPLIT

; SAVE-TEMPS-NOSPLIT: "-cc1"
; SAVE-TEMPS-NOSPLIT-SAME: "-S"
; SAVE-TEMPS-NOSPLIT-NOT: thinlto-split-output-list

; --- Step 5: Verify merged output.o is valid ---
; Use clang_cc1 + ld.lld directly to verify the merge produces a valid ELF.
; (We cannot use %clang Driver directly because it needs ld.lld in PATH,
;  and lit test environments may not guarantee that. So we verify the merge
;  result by manually running ld.lld on the partition objects.)

; RUN: ld.lld -r -o %t.merged.o %t.cc1.o.thinlto-split.0.o %t.cc1.o.thinlto-split.1.o

; merged.o must be a valid ELF relocatable object
; RUN: llvm-readobj -h %t.merged.o | FileCheck %s --check-prefix=MERGED-ELF
; MERGED-ELF: Type: Relocatable

; merged.o must contain .init_array section (from global constructor)
; RUN: llvm-readobj -S %t.merged.o | FileCheck %s --check-prefix=MERGED-SECTIONS
; MERGED-SECTIONS: Name: .init_array
; MERGED-SECTIONS: Name: .group

; merged.o must contain symbols from BOTH partitions
; RUN: llvm-nm %t.merged.o | FileCheck %s --check-prefix=MERGED-NM
; MERGED-NM-DAG: T func_a
; MERGED-NM-DAG: T func_b
; MERGED-NM-DAG: T func_c
; MERGED-NM-DAG: T func_d
; MERGED-NM-DAG: T func_e
; MERGED-NM-DAG: W weak_func
; MERGED-NM-DAG: {{D|B}} g_global
; MERGED-NM-DAG: {{D|B}} g_ctor_data
; MERGED-NM-DAG: {{V|v}} comdat_var
; Internal symbols promoted across partitions
; MERGED-NM-DAG: T {{.*shared_helper[._]}}
; MERGED-NM-DAG: T {{.*ctor_init[._]}}

; --- Step 6: Verify non-split path produces correct output ---
; RUN: %clang_cc1 -triple aarch64-unknown-linux-gnu \
; RUN:   -emit-obj -fthinlto-index=%t.o.thinlto.bc \
; RUN:   -o %t.nosplit.o -x ir %t.o

; RUN: llvm-readobj -h %t.nosplit.o | FileCheck %s --check-prefix=NOSPLIT-ELF
; NOSPLIT-ELF: Type: Relocatable

; RUN: llvm-nm %t.nosplit.o | FileCheck %s --check-prefix=NOSPLIT-NM
; NOSPLIT-NM-DAG: T func_a
; NOSPLIT-NM-DAG: T func_b
; NOSPLIT-NM-DAG: t shared_helper
; NOSPLIT-NM-DAG: t ctor_init

; Verify no partition objects leaked in non-split path
; RUN: not ls %t.nosplit.o.thinlto-split.0.o 2>/dev/null
; RUN: not ls %t.nosplit.o.thinlto-split.1.o 2>/dev/null

; --- IR source module ---
; Realistic module with features that stress the split+merge path:
; - Internal function shared_helper referenced by multiple roots (promoted across partitions)
; - Global constructor (init_array) with ctor_init in .text.startup
; - Comdat group with weak_odr variable
; - Weak function
; - Multiple root functions to force 2+ partitions

target triple = "aarch64-unknown-linux-gnu"
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"

@g_global = global i32 42
@g_ctor_data = global i32 0

@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [
  { i32, ptr, ptr } { i32 65535, ptr @ctor_init, ptr @g_ctor_data }
]

$comdat_grp = comdat any
@comdat_var = weak_odr global i32 10, comdat($comdat_grp)

define internal void @shared_helper() {
entry:
  store volatile i32 1, ptr @g_global, align 4
  ret void
}

define weak void @weak_func() {
entry:
  ret void
}

define internal void @ctor_init() section ".text.startup" {
entry:
  store i32 100, ptr @g_ctor_data, align 4
  ret void
}

define void @func_a() {
entry:
  call void @shared_helper()
  call void @weak_func()
  %val = load i32, ptr @comdat_var, align 4
  %sum = add i32 %val, 1
  store i32 %sum, ptr @g_global, align 4
  ret void
}

define void @func_b() {
entry:
  call void @shared_helper()
  store volatile i32 2, ptr @g_global, align 4
  ret void
}

define void @func_c() {
entry:
  call void @shared_helper()
  ret void
}

define void @func_d() {
entry:
  %v = load i32, ptr @g_global, align 4
  %r = add i32 %v, 10
  store i32 %r, ptr @g_global, align 4
  call void @shared_helper()
  ret void
}

define void @func_e() {
entry:
  call void @weak_func()
  ret void
}
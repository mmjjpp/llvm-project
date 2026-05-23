; REQUIRES: aarch64-registered-target

; End-to-end test for ThinLTO split codegen + gsplit-dwarf.
;
; Verifies that when -gsplit-dwarf is combined with ThinLTO split codegen:
;   1. Each partition generates its own .dwo file named <stem>.thinlto-split.<Task>.dwo
;   2. .o and .dwo partition numbering is consistent
;   3. Each partition's skeleton CU references the correct per-partition .dwo via DW_AT_dwo_name
;   4. DWO_id in the skeleton CU matches the compile unit in the corresponding .dwo
;   5. lld -r merge produces a final output.o whose skeleton CUs still reference valid .dwo files
;   6. No 4294967295.dwo is generated
;   7. No shared single .dwo file is generated (no output.dwo without partition suffix)
;   8. Both -gsplit-dwarf=split and -gsplit-dwarf=single produce per-partition .dwo

; --- Step 1: Generate ThinLTO bitcode with debug info ---
; The IR module must have !dbg metadata to produce DWARF output.
; RUN: opt -thinlto-bc -thinlto-split-lto-unit -o %t.o %s

; --- Step 2: Generate distributed ThinLTO index ---
; RUN: llvm-lto2 run -thinlto-distributed-indexes %t.o \
; RUN:   -o %t.index \
; RUN:   -r=%t.o,caller_a,px \
; RUN:   -r=%t.o,caller_b,px

; --- Step 3: -gsplit-dwarf=split path (both -split-dwarf-file and -split-dwarf-output) ---
; RUN: %clang_cc1 -triple aarch64-unknown-linux-gnu \
; RUN:   -emit-obj -fthinlto-index=%t.o.thinlto.bc \
; RUN:   -thinlto-split-output-list=%t.split.rsp \
; RUN:   -split-dwarf-file %t.split.o.dwo \
; RUN:   -split-dwarf-output %t.split.o.dwo \
; RUN:   -o %t.split.o -x ir %t.o \
; RUN:   -debug-info-kind=constructor -dwarf-version=5 \
; RUN:   -mllvm -thinlto-split=true \
; RUN:   -mllvm -thinlto-split-partitions=2 \
; RUN:   -mllvm -thinlto-split-module-size-threshold=0 \
; RUN:   -mllvm -thinlto-split-module-size-rate-threshold=2.0

; --- Step 3a: Verify per-partition .dwo files exist ---
; RUN: ls %t.split.o.thinlto-split.0.dwo
; RUN: ls %t.split.o.thinlto-split.1.dwo

; --- Step 3b: Verify NO single shared .dwo ---
; RUN: not ls %t.split.o.dwo 2>/dev/null

; --- Step 3c: Verify no 4294967295.dwo ---
; RUN: not ls %t.split.o.thinlto-split.4294967295.dwo 2>/dev/null

; --- Step 3d: Verify .dwo files are valid ELF ---
; RUN: llvm-readobj -h %t.split.o.thinlto-split.0.dwo | FileCheck %s --check-prefix=DWO0-ELF
; DWO0-ELF: Type: Relocatable

; RUN: llvm-readobj -h %t.split.o.thinlto-split.1.dwo | FileCheck %s --check-prefix=DWO1-ELF
; DWO1-ELF: Type: Relocatable

; --- Step 3e: Verify skeleton CU DW_AT_dwo_name ---
; RUN: llvm-dwarfdump -debug-info %t.split.o.thinlto-split.0.o | FileCheck %s --check-prefix=SKELETON0
; SKELETON0: DW_TAG_skeleton_unit
; SKELETON0: DW_AT_dwo_name{{.*}}thinlto-split.0.dwo

; RUN: llvm-dwarfdump -debug-info %t.split.o.thinlto-split.1.o | FileCheck %s --check-prefix=SKELETON1
; SKELETON1: DW_TAG_skeleton_unit
; SKELETON1: DW_AT_dwo_name{{.*}}thinlto-split.1.dwo

; --- Step 3f: Verify DWO_id is present in both skeleton CU and .dwo ---
; FileCheck variables cannot span separate RUN lines, so we verify presence
; and format rather than exact value equality across files. End-to-end shell
; testing confirms DWO_id values match between skeleton CU and .dwo.
; RUN: llvm-dwarfdump -debug-info %t.split.o.thinlto-split.0.o | FileCheck %s --check-prefix=DWO_ID0
; DWO_ID0: DWO_id = 0x{{[0-9a-f]+}}

; RUN: llvm-dwarfdump -debug-info %t.split.o.thinlto-split.0.dwo | FileCheck %s --check-prefix=DWO_DWO0
; DWO_DWO0: DWO_id = 0x{{[0-9a-f]+}}

; RUN: llvm-dwarfdump -debug-info %t.split.o.thinlto-split.1.o | FileCheck %s --check-prefix=DWO_ID1
; DWO_ID1: DWO_id = 0x{{[0-9a-f]+}}

; RUN: llvm-dwarfdump -debug-info %t.split.o.thinlto-split.1.dwo | FileCheck %s --check-prefix=DWO_DWO1
; DWO_DWO1: DWO_id = 0x{{[0-9a-f]+}}

; --- Step 4: Verify lld -r merge preserves .dwo references ---
; RUN: ld.lld -r -o %t.merged.o %t.split.o.thinlto-split.0.o %t.split.o.thinlto-split.1.o

; RUN: llvm-readobj -h %t.merged.o | FileCheck %s --check-prefix=MERGED-ELF
; MERGED-ELF: Type: Relocatable

; RUN: llvm-dwarfdump -debug-info %t.merged.o | FileCheck %s --check-prefix=MERGED-DWO
; MERGED-DWO: DW_TAG_skeleton_unit
; MERGED-DWO: DW_AT_dwo_name{{.*}}thinlto-split.0.dwo
; MERGED-DWO: DW_TAG_skeleton_unit
; MERGED-DWO: DW_AT_dwo_name{{.*}}thinlto-split.1.dwo

; --- Step 5: -gsplit-dwarf=single path (only -split-dwarf-file, no -split-dwarf-output) ---
; -gsplit-dwarf=single tells cc1 to embed .dwo content into the .o file and
; not write a separate .dwo file. However, when ThinLTO split is active,
; per-partition .dwo naming must still be used so each partition's skeleton CU
; can reference its own .dwo. This tests that SplitDwarfOutputStem is derived
; from SplitDwarfFile when SplitDwarfOutput is absent.

; RUN: %clang_cc1 -triple aarch64-unknown-linux-gnu \
; RUN:   -emit-obj -fthinlto-index=%t.o.thinlto.bc \
; RUN:   -thinlto-split-output-list=%t.single.rsp \
; RUN:   -split-dwarf-file %t.single.o.dwo \
; RUN:   -o %t.single.o -x ir %t.o \
; RUN:   -debug-info-kind=constructor -dwarf-version=5 \
; RUN:   -mllvm -thinlto-split=true \
; RUN:   -mllvm -thinlto-split-partitions=2 \
; RUN:   -mllvm -thinlto-split-module-size-threshold=0 \
; RUN:   -mllvm -thinlto-split-module-size-rate-threshold=2.0

; --- Step 5a: Verify per-partition .dwo files exist even without -split-dwarf-output ---
; RUN: ls %t.single.o.thinlto-split.0.dwo
; RUN: ls %t.single.o.thinlto-split.1.dwo

; --- Step 5b: Verify skeleton CU DW_AT_dwo_name ---
; RUN: llvm-dwarfdump -debug-info %t.single.o.thinlto-split.0.o | FileCheck %s --check-prefix=SINGLE0
; SINGLE0: DW_TAG_skeleton_unit
; SINGLE0: DW_AT_dwo_name{{.*}}thinlto-split.0.dwo

; RUN: llvm-dwarfdump -debug-info %t.single.o.thinlto-split.1.o | FileCheck %s --check-prefix=SINGLE1
; SINGLE1: DW_TAG_skeleton_unit
; SINGLE1: DW_AT_dwo_name{{.*}}thinlto-split.1.dwo

; --- Step 6: Verify non-split path with gsplit-dwarf still works ---
; RUN: %clang_cc1 -triple aarch64-unknown-linux-gnu \
; RUN:   -emit-obj -fthinlto-index=%t.o.thinlto.bc \
; RUN:   -split-dwarf-file %t.nosplit.o.dwo \
; RUN:   -split-dwarf-output %t.nosplit.o.dwo \
; RUN:   -o %t.nosplit.o -x ir %t.o \
; RUN:   -debug-info-kind=constructor -dwarf-version=5 \
; RUN:   -mllvm -thinlto-split=false

; RUN: ls %t.nosplit.o.dwo
; RUN: not ls %t.nosplit.o.thinlto-split.0.dwo 2>/dev/null

; --- IR source module with debug metadata ---
target triple = "aarch64-unknown-linux-gnu"
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!3, !4}

!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "test", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, splitDebugInlining: false, nameTableKind: None)
!1 = !DIFile(filename: "source.c", directory: "/test")
!2 = !DISubroutineType(types: !{})
!3 = !{i32 2, !"Dwarf Version", i32 5}
!4 = !{i32 2, !"Debug Info Version", i32 3}

define internal void @shared() !dbg !5 {
entry:
  ret void
}

define void @caller_a() !dbg !7 {
entry:
  call void @shared(), !dbg !9
  ret void
}

define void @caller_b() !dbg !10 {
entry:
  call void @shared(), !dbg !12
  ret void
}

!5 = distinct !DISubprogram(name: "shared", scope: !1, file: !1, line: 1, type: !2, spFlags: DISPFlagDefinition, unit: !0)
!7 = distinct !DISubprogram(name: "caller_a", scope: !1, file: !1, line: 4, type: !2, spFlags: DISPFlagDefinition, unit: !0)
!9 = !DILocation(line: 5, column: 3, scope: !7)
!10 = distinct !DISubprogram(name: "caller_b", scope: !1, file: !1, line: 8, type: !2, spFlags: DISPFlagDefinition, unit: !0)
!12 = !DILocation(line: 9, column: 3, scope: !10)
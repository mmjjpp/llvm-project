// RUN: %clang -### -target aarch64-unknown-linux-gnu -B%S/Inputs/lld \
// RUN:   -c -fthinlto-index=foo.thinlto.bc -x ir %s -o foo.o \
// RUN:   -mllvm -lto-split-by-callgraph=true \
// RUN:   -mllvm -thinlto-split-partitions=2 2>&1 | FileCheck %s --check-prefix=MERGE
// RUN: %clang -### -target aarch64-unknown-linux-gnu -B%S/Inputs/lld \
// RUN:   -c -fthinlto-index=foo.thinlto.bc -x ir %s -o foo.o \
// RUN:   -mllvm -lto-split-by-callgraph=true 2>&1 | FileCheck %s --check-prefix=LLD
// RUN: %clang -### -target aarch64-unknown-linux-gnu -B%S/Inputs/lld \
// RUN:   -c -fthinlto-index=foo.thinlto.bc -x ir %s -o foo.o \
// RUN:   -mllvm -lto-split-by-callgraph=false 2>&1 | FileCheck %s --check-prefix=NOSPLIT
// RUN: %clang -### -target aarch64-unknown-linux-gnu -B%S/Inputs/lld \
// RUN:   -save-temps -c -fthinlto-index=foo.thinlto.bc -x ir %s -o foo.o \
// RUN:   -mllvm -lto-split-by-callgraph=true 2>&1 | FileCheck %s --check-prefix=SAVE-TEMPS
// RUN: rm -rf %t.empty
// RUN: mkdir -p %t.empty
// RUN: not env PATH= %clang -### -ccc-install-dir %t.empty \
// RUN:   -target aarch64-unknown-linux-gnu \
// RUN:   -c -fthinlto-index=foo.thinlto.bc -x ir %s -o foo.o \
// RUN:   -mllvm -lto-split-by-callgraph=true 2>&1 | FileCheck %s --check-prefix=MISSING-LLD
// RUN: %clang -### -target x86_64-unknown-freebsd \
// RUN:   -c -fthinlto-index=foo.thinlto.bc -x ir %s -o foo.o \
// RUN:   -mllvm -lto-split-by-callgraph=true 2>&1 | FileCheck %s --check-prefix=FREEBSD
// RUN: %clang -### -target x86_64-unknown-fuchsia \
// RUN:   -c -fthinlto-index=foo.thinlto.bc -x ir %s -o foo.o \
// RUN:   -mllvm -lto-split-by-callgraph=true 2>&1 | FileCheck %s --check-prefix=FUCHSIA
// RUN: %clang -### -target x86_64-none-elf \
// RUN:   -c -fthinlto-index=foo.thinlto.bc -x ir %s -o foo.o \
// RUN:   -mllvm -lto-split-by-callgraph=true 2>&1 | FileCheck %s --check-prefix=BAREMETAL

// MERGE: "-cc1"
// MERGE-SAME: "-fthinlto-index=foo.thinlto.bc"
// MERGE-SAME: "-thinlto-split-output-list=[[RSP:[^"]+\.thinlto-split\.rsp]]"
// MERGE-SAME: "-o" "[[TEMP_O:[^"]+\.o]]"
// MERGE: "{{.*}}/Inputs/lld/ld.lld" "-r" "-o" "foo.o" "@[[RSP]]"

// LLD: "-cc1"
// LLD-SAME: "-thinlto-split-output-list=[[LLD_RSP:[^"]+\.thinlto-split\.rsp]]"
// LLD: "{{.*}}/Inputs/lld/ld.lld" "-r" "-o" "foo.o" "@[[LLD_RSP]]"

// NOSPLIT: "-cc1"
// NOSPLIT-NOT: thinlto-split-output-list
// NOSPLIT-NOT: ld.lld

// SAVE-TEMPS: "-cc1"
// SAVE-TEMPS-SAME: "-emit-obj"
// SAVE-TEMPS-SAME: "-thinlto-split-output-list=[[SAVE_RSP:[^"]+\.thinlto-split\.rsp]]"
// SAVE-TEMPS: "{{.*}}/Inputs/lld/ld.lld" "-r" "-o" "foo.o" "@[[SAVE_RSP]]"

// MISSING-LLD: error: cannot find 'ld.lld' required for ThinLTO split codegen

// FREEBSD: "-cc1"
// FREEBSD-SAME: "-fthinlto-index=foo.thinlto.bc"
// FREEBSD-NOT: thinlto-split-output-list
// FREEBSD-NOT: "-r"

// FUCHSIA: "-cc1"
// FUCHSIA-SAME: "-fthinlto-index=foo.thinlto.bc"
// FUCHSIA-NOT: thinlto-split-output-list
// FUCHSIA-NOT: "-r"

// BAREMETAL: "-cc1"
// BAREMETAL-SAME: "-fthinlto-index=foo.thinlto.bc"
// BAREMETAL-NOT: thinlto-split-output-list
// BAREMETAL-NOT: "-r"

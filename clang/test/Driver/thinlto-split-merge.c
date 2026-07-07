// RUN: %clang -### -target aarch64-unknown-linux-gnu -B%S/Inputs/lld \
// RUN:   -c -fthinlto-index=foo.thinlto.bc -x ir %s -o foo.o \
// RUN:   -mllvm -thinlto-split=true 2>&1 | FileCheck %s --check-prefix=MERGE
// RUN: %clang -### -target aarch64-unknown-linux-gnu -B%S/Inputs/lld \
// RUN:   -c -fthinlto-index=foo.thinlto.bc -x ir %s -o foo.o \
// RUN:   -mllvm -thinlto-split=false 2>&1 | FileCheck %s --check-prefix=NOSPLIT
// RUN: %clang -### -target aarch64-unknown-linux-gnu -B%S/Inputs/lld \
// RUN:   -save-temps -c -fthinlto-index=foo.thinlto.bc -x ir %s -o foo.o \
// RUN:   -mllvm -thinlto-split=true 2>&1 | FileCheck %s --check-prefix=SAVE-TEMPS
// RUN: %if !system-windows %{ rm -rf %t.empty && mkdir -p %t.empty && not env PATH= %clang -### -ccc-install-dir %t.empty -target aarch64-unknown-linux-gnu -c -fthinlto-index=foo.thinlto.bc -x ir %s -o foo.o -mllvm -thinlto-split=true 2>&1 | FileCheck %s --check-prefix=MISSING-LLD %}
// RUN: %clang -### -target x86_64-unknown-freebsd \
// RUN:   -c -fthinlto-index=foo.thinlto.bc -x ir %s -o foo.o \
// RUN:   -mllvm -thinlto-split=true 2>&1 | FileCheck %s --check-prefix=FREEBSD

// MERGE: "-cc1"
// MERGE-SAME: "-fthinlto-index=foo.thinlto.bc"
// MERGE-SAME: "-thinlto-split-output-list=[[RSP:[^"]+\.thinlto-split\.rsp]]"
// MERGE-SAME: "-o" "[[TEMP_O:[^"]+\.o]]"
// MERGE: "{{.*}}Inputs{{[/\\]+}}lld{{[/\\]+}}ld.lld" "-r" "-o" "foo.o" "@[[RSP]]"

// NOSPLIT: "-cc1"
// NOSPLIT-NOT: thinlto-split-output-list
// NOSPLIT-NOT: ld.lld

// SAVE-TEMPS: "-cc1"
// SAVE-TEMPS-SAME: "-emit-obj"
// SAVE-TEMPS-SAME: "-thinlto-split-output-list=[[SAVE_RSP:[^"]+\.thinlto-split\.rsp]]"
// SAVE-TEMPS: "{{.*}}Inputs{{[/\\]+}}lld{{[/\\]+}}ld.lld" "-r" "-o" "foo.o" "@[[SAVE_RSP]]"

// MISSING-LLD: error: cannot find 'ld.lld' required for ThinLTO split codegen

// FREEBSD: "-cc1"
// FREEBSD-SAME: "-fthinlto-index=foo.thinlto.bc"
// FREEBSD-NOT: thinlto-split-output-list
// FREEBSD-NOT: "-r"

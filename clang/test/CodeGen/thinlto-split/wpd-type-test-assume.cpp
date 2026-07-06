// UNSUPPORTED: system-windows
// REQUIRES: aarch64-registered-target

// Test that SplitModuleCG's resolveVCallsFromSummary correctly handles
// TypeTestAssumeVCalls (variable args) and TypeTestAssumeConstVCalls
// (constant args) from the function summary's TypeIdInfo.
//
// -fwhole-program-vtables causes Clang to emit llvm.type.test + llvm.assume
// for virtual calls. Variable args -> TypeTestAssumeVCalls,
// constant args -> TypeTestAssumeConstVCalls.
//
// Two TUs are used so the compiler cannot prove the concrete type at
// compile time: tu-virt.cpp takes a Base* but cannot know which Derived
// the caller passes, so it must emit type.test + assume for WPD.

// RUN: rm -rf %t %t.dir && split-file %s %t.dir
// RUN: %clangxx -O2 -flto=thin -fwhole-program-vtables -fvisibility=hidden \
// RUN:   -c %t.dir/tu-virt.cpp -o %t-virt.o
// RUN: %clangxx -O2 -flto=thin -fwhole-program-vtables -fvisibility=hidden \
// RUN:   -c %t.dir/tu-main.cpp -o %t-main.o

// RUN: %clangxx -O2 -flto=thin -fuse-ld=lld \
// RUN:   -fwhole-program-vtables -fvisibility=hidden \
// RUN:   %t-virt.o %t-main.o -o %t \
// RUN:   -Wl,-mllvm,-thinlto-split=true \
// RUN:   -Wl,-mllvm,-thinlto-split-partitions=2 \
// RUN:   -Wl,-mllvm,-thinlto-split-module-size-threshold=0 \
// RUN:   -Wl,-mllvm,-thinlto-split-module-size-rate-threshold=0 \
// RUN:   -Wl,--save-temps -Wl,-mllvm,-debug-only=split-module-CG

// Verify summary has TypeTestAssumeVCalls and TypeTestAssumeConstVCalls
// RUN: llvm-dis %t.index.bc -o - 2>/dev/null | FileCheck %s --check-prefix=SUMMARY
// SUMMARY-DAG: typeTestAssumeVCalls
// SUMMARY-DAG: typeTestAssumeConstVCalls

// --- Check: WPD devirtualization worked in the optimized IR ---
// main is fully constant-folded (ret i32 142), no call to Derived::fn.
// RUN: llvm-dis %t.tu-main.cpp.5.4.opt.bc -o - | FileCheck %s --check-prefix=DEVIRT
// DEVIRT-LABEL: define {{.*}} @main
// DEVIRT: entry:
// DEVIRT: ret i32 142
// DEVIRT-NOT: call @_ZNK7Derived2fnEi

//--- types.h
struct Base {
  virtual ~Base() = default;
  virtual int fn(int x) const = 0;
};

struct Derived : Base {
  int fn(int x) const override { return x + 50; }
};

//--- tu-virt.cpp
#include "types.h"

// Variable arg -> TypeTestAssumeVCalls
int caller_var(const Base *p, int x) { return p->fn(x); }

// Constant arg -> TypeTestAssumeConstVCalls
int caller_const(const Base *p) { return p->fn(42); }

//--- tu-main.cpp
#include "types.h"

extern int caller_var(const Base *p, int x);
extern int caller_const(const Base *p);

int main() {
  Derived d;
  return caller_var(&d, 0) + caller_const(&d);
}

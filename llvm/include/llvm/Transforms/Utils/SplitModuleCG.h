//===- SplitModuleCG.h - Split a module by its call graph -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the llvm::SplitModuleCG class, which splits a module
// into partitions based on its call graph, so that the partitions can be
// optimized and codegen'd in parallel.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_SPLITMODULECG_H
#define LLVM_TRANSFORMS_UTILS_SPLITMODULECG_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/InstructionCost.h"
#include "llvm/Transforms/Utils/SplitModuleUtils.h"

#include <memory>

namespace llvm {

/// Splits a module into linkable partitions by traversing its call graph,
/// so that each partition carries a self-consistent subset of functions
/// (a root + its callees) and is balanced by IR-instruction cost. The
/// partitions can be optimized and codegen'd in parallel.
///
/// buildGraph() and createWorkList() run in the constructor;
/// splitModule then:
///   1. externalize(): promotes locals to external+hidden; unnamed entities
///      get a stable name.
///   2. sortWorkList(): sorts the worklist by accumulated cost.
///   3. doPartitioning(): greedily assigns each root + dependencies to the
///      least-loaded partition.
///   4. Per partition: CloneModule, then dealWithMpart downgrades duplicate
///      definitions to available_externally, erases unused declarations,
///      and renames promoted locals with a common per-module suffix.
///   5. Emits each partition synchronously through ModuleCreationCallback.
///      Driver-level callers may transfer them to worker contexts.
class SplitModuleCG {
public:
  /// Invoked synchronously once per partition.
  using ModuleCreationCallback =
      function_ref<void(std::unique_ptr<Module> MPart, unsigned PartitionId)>;

  /// Construct a SplitModuleCG over module \p M.
  ///
  /// \param M The module to partition. Must outlive the SplitModuleCG
  ///          instance and any partitions emitted via splitModule().
  /// \param LimitPartition Upper bound on the number of partitions to
  ///          produce. Pass 0 (the default) to derive the partition count
  ///          from the number of call-graph roots discovered in
  ///          createWorkList, capped to the available hardware parallelism.
  ///          The actual partition count is finalized in the constructor.
  LLVM_ABI SplitModuleCG(Module &M, unsigned LimitPartition = 0);

  /// Splits the module and invokes \p ModuleCallback once per partition.
  LLVM_ABI void splitModule(ModuleCreationCallback ModuleCallback);

  unsigned getPartitionNum() const { return NumPartitions; }

private:
  using CostType = InstructionCost::CostType;

  struct WorkListEntry {
    const Function *Root = nullptr;
    BitVector Nodes;
    CostType TotalCost = 0;
  };

  /// Number of partitions to produce; finalized in the constructor.
  unsigned NumPartitions;
  Module &M;
  CallGraph CG;
  std::unique_ptr<splitmodule::Graph> Graph;
  /// Names of GVs already external before externalize; excluded from renaming.
  StringSet<> OriginalExternals;
  /// Tracks functions that may be defined in multiple partitions so that
  /// dealWithMpart can downgrade duplicates to available_externally:
  ///   - Membership: the definition is safely downgradable (see
  ///     canDowngradeToAvailableExternally); absent functions are untouched.
  ///   - The mapped bool: true until the first partition holding the
  ///     definition claims it (sets false) and keeps it; later partitions
  ///     downgrade their copies to available_externally.
  DenseMap<const Function *, bool> ExternalFunction;
  /// Partitioning worklist built by createWorkList, sorted by TotalCost.
  SmallVector<WorkListEntry> WorkList;

  /// Build direct call edges on top of the shared splitting graph.
  void buildGraph();

  /// Walk WorkList in cost-sorted order and greedily assign each root and
  /// its dependencies to the partition with the lowest accumulated cost
  /// (load-balanced bin-packing). Returns one set per partition.
  std::vector<DenseSet<const Function *>> doPartitioning();

  /// Post-process a cloned partition \p MPart (partition index \p I):
  ///   - Downgrade duplicate definitions of originally-external functions to
  ///     available_externally.
  ///   - Erase declarations that are no longer used.
  ///   - Rename promoted local symbols (now external, not in OriginalExternals)
  ///     to "name.llvm.<suffix>" to avoid duplicate symbols across partitions.
  void dealWithMpart(Module &MPart, unsigned I);

  /// Discover graph roots and build WorkList. Functions in rootless cycles are
  /// treated as roots themselves.
  void createWorkList();

  /// Sort work by total cost (descending) and then by function name.
  void sortWorkList();
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_UTILS_SPLITMODULECG_H

//===- SplitModuleCG.cpp - Split a module by its call graph ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the llvm::SplitModuleCG class, which splits a module
// into partitions based on its call graph.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/SplitModuleCG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/Threading.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
using namespace llvm;

#define DEBUG_TYPE "split-module-cg"

static cl::opt<bool> EnablePrintSimplifiedCallGraph(
    "enable-print-simplified-callgraph", cl::Hidden, cl::init(false),
    cl::desc("print the module splitting graph"));

namespace {

using PartitionID = unsigned;

} // namespace

/// Returns whether duplicate definitions of \p F across partitions may be
/// downgraded to available_externally. This is safe for external functions
/// (either originally external or promoted by externalize), and for
/// weak_odr/linkonce_odr functions whose equivalent definitions can be
/// deduplicated to reduce codegen. Interposable linkages (weak/linkonce
/// non-ODR) are excluded since downgrading them would change their
/// optimization semantics.
static bool canDowngradeToAvailableExternally(const Function &F) {
  return !F.isDeclaration() &&
         (F.hasExternalLinkage() || F.hasWeakODRLinkage() ||
          F.hasLinkOnceODRLinkage());
}

/// Fallback for getUniqueModuleId when the module has no exportable symbols
/// to seed a hash (e.g. every definition is in a comdat or has linkonce_odr
/// linkage), in which case getUniqueModuleId returns "". Hashes the module
/// identifier and the names of all named global values so that such modules
/// can still be split with promoted locals renamed uniquely per module.
static std::string computeFallbackSuffix(const Module &M) {
  MD5 Md5;
  Md5.update(M.getModuleIdentifier());
  Md5.update(ArrayRef<uint8_t>{0});
  for (const GlobalValue &GV : M.global_values()) {
    if (!GV.hasName())
      continue;
    Md5.update(GV.getName());
    Md5.update(ArrayRef<uint8_t>{0});
  }
  MD5::MD5Result R;
  Md5.final(R);
  SmallString<32> Str;
  MD5::stringifyResult(R, Str);
  return ("." + Str).str();
}

std::vector<DenseSet<const Function *>> SplitModuleCG::doPartitioning() {
  LLVM_DEBUG(dbgs() << "\n--Partitioning Starts--\n");
  assert(NumPartitions != 0 && "Partition count must be at least 1");
  std::vector<DenseSet<const Function *>> Partitions;
  Partitions.resize(NumPartitions);

  auto ComparePartitions = [](const std::pair<PartitionID, CostType> &LHS,
                              const std::pair<PartitionID, CostType> &RHS) {
    // When two partitions have the same cost, assign to the one with the
    // biggest ID first. This allows us to put things in P0 last, because P0 may
    // have other stuff added later.
    if (LHS.second == RHS.second)
      return LHS.first < RHS.first;
    return LHS.second > RHS.second;
  };

  std::vector<std::pair<PartitionID, CostType>> BalancingQueue;
  for (unsigned I = 0; I < NumPartitions; ++I)
    BalancingQueue.emplace_back(I, 0);

  for (const WorkListEntry &Entry : WorkList) {
    // Normal "load-balancing", assign to partition with least pressure.
    auto [PID, _] = BalancingQueue.back();

    // Insert the root and its dependencies into the partition, tracking only
    // newly inserted nodes when updating the balancing queue.
    auto &FnsInPart = Partitions[PID];
    CostType AddedCost = 0;
    for (unsigned NodeID : Entry.Nodes.set_bits()) {
      const auto &Node = Graph->getNode(NodeID);
      if (FnsInPart.insert(&Node.getFunction()).second)
        AddedCost += Node.getIndividualCost();
    }

    // Update the cost of the selected partition, which is the entry at the
    // back of the sorted queue, before re-sorting.
    BalancingQueue.back().second += AddedCost;

    sort(BalancingQueue, ComparePartitions);
  }

  return Partitions;
}

void SplitModuleCG::buildGraph() {
  splitmodule::FunctionCostMap FunctionCosts;
  CostType ModuleCost = splitmodule::calculateFunctionCosts(
      M,
      [](Function &F) {
        CostType Cost = 0;
        for (const BasicBlock &BB : F)
          Cost += std::distance(BB.begin(), BB.end());
        return Cost;
      },
      FunctionCosts);

  Graph = std::make_unique<splitmodule::Graph>(M, ModuleCost);
  for (const Function &F : M)
    if (!F.isDeclaration())
      Graph->getOrInsertNode(F, FunctionCosts.at(&F));

  for (auto &NodePair : CG) {
    CallGraphNode &CGNode = *NodePair.second;
    Function *F = CGNode.getFunction();
    if (!F || F->isDeclaration())
      continue;

    splitmodule::Graph::Node &Node = Graph->getNode(*F);
    for (const auto &CallRecord : CGNode) {
      Function *Callee = CallRecord.second->getFunction();
      if (!Callee || Callee->isDeclaration())
        continue;
      Graph->addEdge(Node, Graph->getNode(*Callee),
                     splitmodule::Graph::EdgeKind::DirectCall);
    }
  }

#ifndef NDEBUG
  if (EnablePrintSimplifiedCallGraph)
    for (const splitmodule::Graph::Node *Node : Graph->nodes()) {
      LLVM_DEBUG(dbgs() << "Call graph node for function: '" << Node->getName()
                        << "' #uses="
                        << std::distance(Node->incoming_edges().begin(),
                                         Node->incoming_edges().end())
                        << "\n");
      for (const splitmodule::Graph::Edge *Edge : Node->outgoing_edges())
        LLVM_DEBUG(dbgs() << "          Calls function : '"
                          << Edge->Dst->getName() << " '\n");
    }
#endif
}

void SplitModuleCG::dealWithMpart(Module &MPart, unsigned I) {
  // Downgrade duplicate definitions of external functions to
  // available_externally. The first partition to define such a function keeps
  // the real definition; all other partitions get available_externally copies.
  for (auto &PartFunc : MPart.functions()) {
    if (PartFunc.isDeclaration())
      continue;
    // Look up the corresponding function in the original module M to check
    // its ExternalFunction status.
    auto *OrigFn = M.getFunction(PartFunc.getName());
    if (!ExternalFunction.contains(OrigFn))
      continue;
    if (!ExternalFunction[OrigFn]) {
      PartFunc.setLinkage(GlobalValue::AvailableExternallyLinkage);
      PartFunc.setComdat(nullptr);
    } else {
      ExternalFunction[OrigFn] = false;
    }
  }

  // Erase declarations that are no longer used.
  for (auto &GV : make_early_inc_range(MPart.global_values()))
    if (GV.isDeclaration() && GV.use_empty())
      GV.eraseFromParent();

  // Rename GlobalValues whose linkage was promoted from internal to external,
  // to avoid duplicate symbols across partitions in ThinLTO. Use the naming
  // convention "name.llvm.<suffix>" so the promoted internal cannot clash with
  // an external that happens to share the same name. The suffix is derived
  // from the module via getUniqueModuleId, so it is consistent across all
  // partitions.
  std::string Suffix = getUniqueModuleId(&M);
  if (Suffix.empty())
    Suffix = computeFallbackSuffix(M);
  for (auto &GV : MPart.global_values()) {
    // Only rename symbols that were promoted from internal to external: skip
    // those that are still internal, and those that were already external in
    // the source module (recorded in OriginalExternals).
    if (GV.hasLocalLinkage() || OriginalExternals.contains(GV.getName()))
      continue;
    GV.setName((GV.getName() + ".llvm" + Suffix).str());
  }

#ifndef NDEBUG
  LLVM_DEBUG(dbgs() << MPart.getModuleIdentifier() << "  : \n");
  for (auto &F : MPart)
    if (!F.isDeclaration())
      LLVM_DEBUG(dbgs() << "   [Function: ] " << I << "  " << F.getName() << " "
                        << F.getLinkage() << "\n");
#endif
}

void SplitModuleCG::createWorkList() {
  BitVector SeenNodes = Graph->createNodesBitVector();
  auto AddRoot = [&](splitmodule::Graph::Node &Root) {
    Graph->markAsGraphEntry(Root);
    BitVector Dependencies = Graph->createNodesBitVector();
    Root.getDependencies(Dependencies);
    WorkList.push_back({&Root.getFunction(), Dependencies,
                        Graph->calculateCost(Dependencies)});
    SeenNodes |= Dependencies;
  };

  // Start with ordinary roots (functions that have no incoming call edge).
  for (splitmodule::Graph::Node *Node : Graph->nodes())
    if (!Node->hasAnyIncomingEdges())
      AddRoot(*Node);

  // A rootless call cycle is not reachable from the roots above. Select the
  // first unseen member in module order as an additional root.
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    splitmodule::Graph::Node &Node = Graph->getNode(F);
    if (!SeenNodes.test(Node.getID()))
      AddRoot(Node);
  }

#ifndef NDEBUG
  assert(Graph->verify());
#endif
}

void SplitModuleCG::sortWorkList() {
  // Sort the worklist so the most expensive roots are seen first.
  sort(WorkList, [](const auto &A, const auto &B) {
    // Sort by total cost, and if the total cost is identical, sort
    // alphabetically
    if (A.TotalCost == B.TotalCost)
      return A.Root->getName() < B.Root->getName();
    return A.TotalCost > B.TotalCost;
  });

  LLVM_DEBUG(dbgs() << "Number of callgraphs to be allocated: "
                    << WorkList.size()
                    << "   Module cost: " << Graph->getModuleCost() << "\n");
  LLVM_DEBUG(dbgs() << "callgraphs: \n");
#ifndef NDEBUG
  for (const WorkListEntry &Entry : WorkList)
    LLVM_DEBUG(dbgs() << "[root] " << Entry.Root->getName() << " (totalCost:"
                      << Entry.TotalCost << ";   root function cost: "
                      << Graph->getCost(*Entry.Root) << ";   has dependency: "
                      << Entry.Nodes.count() << ")\n");
#endif
}

void SplitModuleCG::splitModule(ModuleCreationCallback ModuleCallback) {
  splitmodule::externalize(M, [](const GlobalValue &GV) {
    const auto *F = dyn_cast<Function>(&GV);
    return !F || !F->hasLocalLinkage() || !F->hasOneUse() ||
           F->hasAddressTaken();
  });

  // Record functions that may be defined in multiple partitions so that
  // dealWithMpart can downgrade duplicate copies to available_externally.
  for (Function &F : M)
    if (canDowngradeToAvailableExternally(F))
      ExternalFunction[&F] = true;

  // TODO: The verifier requires an alias to point to a definition and an
  // ifunc resolver to be a definition, but the aliasee/resolver may be
  // assigned to a different partition than its alias/ifunc. Handling this
  // is deferred to a follow-up patch.

  // Sort the worklist here, after all potential renaming has occurred.
  sortWorkList();

  // Assign callgraphs into NumPartitions partitions.
  auto Partitions = doPartitioning();
  assert(Partitions.size() == NumPartitions);

  auto ShouldCloneDefinition = [&](const GlobalValue *GV, unsigned I) {
    const auto &FnsInPart = Partitions[I];

    // Functions go in their assigned partition.
    if (const auto *FnToClone = dyn_cast<Function>(GV))
      return FnsInPart.contains(FnToClone);
    // Everything else goes in the first partition.
    return I == 0;
  };

  SmallVector<unsigned> PartitionIds;
  for (unsigned I = 0; I < NumPartitions; ++I)
    PartitionIds.push_back(I);

  splitmodule::materializePartitions(
      M, PartitionIds, ShouldCloneDefinition,
      [&](std::unique_ptr<Module> MPart, unsigned I) {
        dealWithMpart(*MPart, I);
        ModuleCallback(std::move(MPart), I);
      });
}

SplitModuleCG::SplitModuleCG(Module &M, unsigned LimitPartition)
    : NumPartitions(LimitPartition), M(M), CG(M) {
  // Track existing non-local symbols. This ensures that when we promote
  // internal symbols to external for partitioning, we can handle renaming
  // and avoid conflicts.
  for (const auto &GV : M.global_values())
    if (!GV.hasLocalLinkage())
      OriginalExternals.insert(GV.getName());

  buildGraph();

  // Populate the worklist with root functions and their transitive
  // dependencies. This worklist serves as the foundation for the
  // subsequent module partitioning.
  createWorkList();

  if (NumPartitions == 0) {
    // Auto mode: one partition per call-graph root, but capped to the
    // available hardware parallelism so that a module with a very large
    // number of entry functions does not create an excessive number of
    // partitions (and thus output objects).
    unsigned MaxPartitions =
        heavyweight_hardware_concurrency().compute_thread_count();
    NumPartitions = std::min<unsigned>(WorkList.size(), MaxPartitions);
  } else if (NumPartitions > WorkList.size()) {
    // Do not create more partitions than there are call-graph roots.
    NumPartitions = WorkList.size();
  }
  if (NumPartitions == 0)
    NumPartitions = 1;
}

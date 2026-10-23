//===- SplitModuleUtils.h - Shared module splitting utilities ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines policy-neutral infrastructure shared by module splitting
// implementations. Callers supply graph construction, cost, partitioning, and
// definition selection policies. This file provides graph storage, dependency
// traversal, partition bookkeeping, and module cloning.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_SPLITMODULEUTILS_H
#define LLVM_TRANSFORMS_UTILS_SPLITMODULEUTILS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/InstructionCost.h"
#include <cstdint>
#include <memory>
#include <type_traits>

namespace llvm {

class Function;
class GlobalValue;
class Module;

namespace splitmodule {

using FunctionCost = InstructionCost::CostType;
using FunctionCostMap = DenseMap<const Function *, FunctionCost>;

/// A policy-neutral graph representation used by module splitting
/// implementations. Each node represents a function definition. Callers
/// decide which edges to create and which nodes are entries or non-copyable.
class Graph {
public:
  class Node;

  enum class EdgeKind : uint8_t { DirectCall, IndirectCall };

  struct Edge {
    Edge(Node *Src, Node *Dst, EdgeKind Kind)
        : Src(Src), Dst(Dst), Kind(Kind) {}

    Node *Src;
    Node *Dst;
    EdgeKind Kind;
  };

  using Edges = SmallVector<const Edge *, 0>;
  using edges_iterator = Edges::const_iterator;
  using mutable_nodes_iterator = Node **;
  using nodes_iterator = const Node *const *;

  class Node {
    friend class Graph;

  public:
    unsigned getID() const { return ID; }
    const Function &getFunction() const { return F; }
    StringRef getName() const;
    FunctionCost getIndividualCost() const { return IndividualCost; }
    bool isNonCopyable() const { return IsNonCopyable; }
    bool isGraphEntryPoint() const { return IsGraphEntry; }
    bool followsIndirectEdges() const { return FollowIndirectEdges; }

    bool hasAnyIncomingEdges() const { return !IncomingEdges.empty(); }
    bool hasAnyIncomingEdgesOfKind(EdgeKind Kind) const;
    bool hasAnyOutgoingEdges() const { return !OutgoingEdges.empty(); }
    bool hasAnyOutgoingEdgesOfKind(EdgeKind Kind) const;

    iterator_range<edges_iterator> incoming_edges() const {
      return IncomingEdges;
    }
    iterator_range<edges_iterator> outgoing_edges() const {
      return OutgoingEdges;
    }

    /// Visit this node and all of its transitive dependencies. Indirect edges
    /// are followed only when this traversal's root was configured to do so.
    LLVM_ABI void
    visitAllDependencies(function_ref<void(const Node &)> Visitor) const;
    LLVM_ABI void getDependencies(BitVector &Nodes) const;

  private:
    Node(unsigned ID, const Function &F, FunctionCost Cost, bool IsNonCopyable,
         bool FollowIndirectEdges)
        : ID(ID), F(F), IndividualCost(Cost), IsNonCopyable(IsNonCopyable),
          FollowIndirectEdges(FollowIndirectEdges), IsGraphEntry(false) {}

    unsigned ID;
    const Function &F;
    FunctionCost IndividualCost;
    bool IsNonCopyable : 1;
    bool FollowIndirectEdges : 1;
    bool IsGraphEntry : 1;
    Edges IncomingEdges;
    Edges OutgoingEdges;
  };

  Graph(const Module &M, FunctionCost ModuleCost)
      : M(M), ModuleCost(ModuleCost) {}

  /// Nodes receive IDs in insertion order. Callers control this order when
  /// their partitioning strategy uses IDs to break ties.
  LLVM_ABI Node &getOrInsertNode(const Function &F, FunctionCost Cost,
                                 bool IsNonCopyable = false,
                                 bool FollowIndirectEdges = false);
  LLVM_ABI Node &getNode(const Function &F);
  LLVM_ABI const Node &getNode(const Function &F) const;
  /// Returns null for declarations and other functions absent from the graph.
  LLVM_ABI const Node *findNode(const Function &F) const;
  const Node &getNode(unsigned ID) const { return *Nodes[ID]; }

  LLVM_ABI const Edge &addEdge(Node &Src, Node &Dst, EdgeKind Kind);
  void markAsGraphEntry(Node &N) { N.IsGraphEntry = true; }

  bool empty() const { return Nodes.empty(); }
  iterator_range<mutable_nodes_iterator> nodes() { return Nodes; }
  iterator_range<nodes_iterator> nodes() const { return Nodes; }
  unsigned getNumNodes() const { return Nodes.size(); }
  BitVector createNodesBitVector() const { return BitVector(Nodes.size()); }
  const Module &getModule() const { return M; }
  FunctionCost getModuleCost() const { return ModuleCost; }
  FunctionCost getCost(const Function &F) const {
    return getNode(F).getIndividualCost();
  }

  LLVM_ABI FunctionCost calculateCost(const BitVector &Nodes) const;

#ifndef NDEBUG
  LLVM_ABI bool verify() const;
#endif

private:
  const Module &M;
  FunctionCost ModuleCost;
  DenseMap<const Function *, Node *> NodeMap;
  SmallVector<Node *> Nodes;
  SpecificBumpPtrAllocator<Node> NodesPool;
  static_assert(std::is_trivially_destructible_v<Edge>);
  BumpPtrAllocator EdgesPool;
};

/// A set of graph nodes assigned to one partition and their aggregate cost.
/// Construct partitions after graph construction is complete. The graph must
/// outlive its partitions and must not gain new nodes while they are in use.
/// Copies are independent, allowing partitioning strategies to explore multiple
/// assignments without changing the graph.
class Partition {
public:
  explicit Partition(const Graph &G) : G(&G), Nodes(G.createNodesBitVector()) {}

  const BitVector &getNodes() const { return Nodes; }
  FunctionCost getCost() const { return Cost; }
  bool empty() const { return Nodes.none(); }

  /// Merge nodes into this partition, counting each node's cost only once.
  LLVM_ABI void add(const BitVector &AdditionalNodes);
  LLVM_ABI bool contains(const Function &F) const;

private:
  const Graph *G;
  BitVector Nodes;
  FunctionCost Cost = 0;
};

/// Find the least expensive partition, choosing the highest index on ties.
/// Partitions must not be empty.
LLVM_ABI unsigned findCheapestPartition(ArrayRef<Partition> Partitions);

LLVM_ABI void
externalize(Module &M,
            function_ref<bool(const GlobalValue &GV)> ShouldExternalize);

LLVM_ABI FunctionCost calculateFunctionCosts(
    Module &M, function_ref<FunctionCost(Function &F)> CalculateFunctionCost,
    FunctionCostMap &FunctionCosts);

/// Control whether module-level inline assembly is copied into every output or
/// only the first materialized partition (which need not have partition ID 0).
enum class ModuleAsmPolicy { PreserveAll, PreserveFirst };

/// Clone the selected definitions into each partition, in PartitionIds order.
/// Scheduling, partition cleanup, and LLVMContext transfer belong to the
/// caller.
LLVM_ABI void materializePartitions(
    const Module &M, ArrayRef<unsigned> PartitionIds,
    function_ref<bool(const GlobalValue *GV, unsigned PartitionId)>
        ShouldCloneDefinition,
    function_ref<void(std::unique_ptr<Module> MPart, unsigned PartitionId)>
        ModuleCallback,
    ModuleAsmPolicy AsmPolicy);

} // namespace splitmodule
} // namespace llvm

#endif // LLVM_TRANSFORMS_UTILS_SPLITMODULEUTILS_H

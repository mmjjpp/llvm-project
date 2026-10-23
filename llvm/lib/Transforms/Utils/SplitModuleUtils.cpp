//===- SplitModuleUtils.cpp - Shared module splitting utilities ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/SplitModuleUtils.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <cassert>

using namespace llvm;
using namespace llvm::splitmodule;

StringRef Graph::Node::getName() const { return F.getName(); }

bool Graph::Node::hasAnyIncomingEdgesOfKind(EdgeKind Kind) const {
  return any_of(IncomingEdges,
                [Kind](const Edge *E) { return E->Kind == Kind; });
}

bool Graph::Node::hasAnyOutgoingEdgesOfKind(EdgeKind Kind) const {
  return any_of(OutgoingEdges,
                [Kind](const Edge *E) { return E->Kind == Kind; });
}

void Graph::Node::visitAllDependencies(
    function_ref<void(const Node &)> Visitor) const {
  DenseSet<const Node *> Seen;
  SmallVector<const Node *, 8> WorkList({this});
  while (!WorkList.empty()) {
    const Node *Current = WorkList.pop_back_val();
    if (!Seen.insert(Current).second)
      continue;

    Visitor(*Current);
    for (const Edge *E : Current->outgoing_edges()) {
      if (!FollowIndirectEdges && E->Kind == EdgeKind::IndirectCall)
        continue;
      WorkList.push_back(E->Dst);
    }
  }
}

void Graph::Node::getDependencies(BitVector &Nodes) const {
  visitAllDependencies([&](const Node &N) { Nodes.set(N.getID()); });
}

Graph::Node &Graph::getOrInsertNode(const Function &F, FunctionCost Cost,
                                    bool IsNonCopyable,
                                    bool FollowIndirectEdges) {
  auto [It, Inserted] = NodeMap.try_emplace(&F);
  if (!Inserted) {
    Node &N = *It->second;
    assert(N.IndividualCost == Cost && "inconsistent node cost");
    N.IsNonCopyable |= IsNonCopyable;
    N.FollowIndirectEdges |= FollowIndirectEdges;
    return N;
  }

  Node *N = new (NodesPool.Allocate())
      Node(Nodes.size(), F, Cost, IsNonCopyable, FollowIndirectEdges);
  It->second = N;
  Nodes.push_back(N);
  return *It->second;
}

Graph::Node &Graph::getNode(const Function &F) {
  auto It = NodeMap.find(&F);
  assert(It != NodeMap.end() && "function has no module split graph node");
  return *It->second;
}

const Graph::Node &Graph::getNode(const Function &F) const {
  const Node *N = findNode(F);
  assert(N && "function has no module split graph node");
  return *N;
}

const Graph::Node *Graph::findNode(const Function &F) const {
  auto It = NodeMap.find(&F);
  return It == NodeMap.end() ? nullptr : It->second;
}

const Graph::Edge &Graph::addEdge(Node &Src, Node &Dst, EdgeKind Kind) {
  for (const Edge *E : Src.OutgoingEdges)
    if (E->Dst == &Dst && E->Kind == Kind)
      return *E;

  auto *E = new (EdgesPool.Allocate<Edge>(1)) Edge(&Src, &Dst, Kind);
  Src.OutgoingEdges.push_back(E);
  Dst.IncomingEdges.push_back(E);
  return *E;
}

FunctionCost Graph::calculateCost(const BitVector &NodeBits) const {
  FunctionCost Cost = 0;
  for (unsigned NodeID : NodeBits.set_bits())
    Cost += getNode(NodeID).getIndividualCost();
  return Cost;
}

void Partition::add(const BitVector &AdditionalNodes) {
  assert(AdditionalNodes.size() == Nodes.size() &&
         "partition and dependency sets must use the same graph");
  for (unsigned ID : AdditionalNodes.set_bits())
    if (!Nodes.test(ID))
      Cost += G->getNode(ID).getIndividualCost();
  Nodes |= AdditionalNodes;
}

bool Partition::contains(const Function &F) const {
  const Graph::Node *N = G->findNode(F);
  return N && Nodes.test(N->getID());
}

unsigned
llvm::splitmodule::findCheapestPartition(ArrayRef<Partition> Partitions) {
  assert(!Partitions.empty() && "partition list must not be empty");
  unsigned Cheapest = 0;
  for (unsigned I = 1; I < Partitions.size(); ++I)
    if (Partitions[I].getCost() <= Partitions[Cheapest].getCost())
      Cheapest = I;
  return Cheapest;
}

#ifndef NDEBUG
bool Graph::verify() const {
  DenseSet<const Node *> SeenNodes;
  DenseSet<const Function *> SeenFunctions;
  unsigned ExpectedID = 0;
  for (const Node *N : Nodes) {
    if (!N || N->getID() != ExpectedID++ || !SeenNodes.insert(N).second ||
        &getNode(N->getID()) != N)
      return false;
    if (N->getFunction().isDeclaration() ||
        !SeenFunctions.insert(&N->getFunction()).second)
      return false;

    for (const Edge *E : N->IncomingEdges)
      if (!E || !E->Src || !E->Dst || E->Dst != N ||
          find(E->Src->OutgoingEdges, E) == E->Src->OutgoingEdges.end())
        return false;
    for (const Edge *E : N->OutgoingEdges)
      if (!E || !E->Src || !E->Dst || E->Src != N ||
          find(E->Dst->IncomingEdges, E) == E->Dst->IncomingEdges.end())
        return false;
  }

  for (const Function &F : M)
    if (!F.isDeclaration() && !SeenFunctions.contains(&F))
      return false;

  BitVector Reachable = createNodesBitVector();
  for (const Node *N : nodes())
    if (N->isGraphEntryPoint())
      N->getDependencies(Reachable);
  return Reachable.all();
}
#endif

void llvm::splitmodule::externalize(
    Module &M, function_ref<bool(const GlobalValue &GV)> ShouldExternalize) {
  for (GlobalValue &GV : M.global_values())
    if (ShouldExternalize(GV))
      GV.externalize();
}

FunctionCost llvm::splitmodule::calculateFunctionCosts(
    Module &M, function_ref<FunctionCost(Function &F)> CalculateFunctionCost,
    FunctionCostMap &FunctionCosts) {
  FunctionCost ModuleCost = 0;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;

    FunctionCost Cost = CalculateFunctionCost(F);
    assert(Cost != 0 && "function must have a non-zero cost");
    FunctionCosts[&F] = Cost;
    assert(static_cast<uint64_t>(ModuleCost) + static_cast<uint64_t>(Cost) >=
               static_cast<uint64_t>(ModuleCost) &&
           "Overflow!");
    ModuleCost += Cost;
  }
  return ModuleCost;
}

void llvm::splitmodule::materializePartitions(
    const Module &M, ArrayRef<unsigned> PartitionIds,
    function_ref<bool(const GlobalValue *GV, unsigned PartitionId)>
        ShouldCloneDefinition,
    function_ref<void(std::unique_ptr<Module> MPart, unsigned PartitionId)>
        ModuleCallback,
    ModuleAsmPolicy AsmPolicy) {
  bool IsFirstPartition = true;
  for (unsigned PartitionId : PartitionIds) {
    ValueToValueMapTy VMap;
    std::unique_ptr<Module> MPart(
        CloneModule(M, VMap, [&](const GlobalValue *GV) {
          return ShouldCloneDefinition(GV, PartitionId);
        }));
    if (AsmPolicy == ModuleAsmPolicy::PreserveFirst && !IsFirstPartition)
      MPart->removeModuleInlineAsm();
    IsFirstPartition = false;
    ModuleCallback(std::move(MPart), PartitionId);
  }
}

//===-- ExperimentalDependencyGraph.cpp - Track intra-module dependencies --==//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Driver/ExperimentalDependencyDriverGraph.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/FileSystem.h"
#include "swift/Basic/ReferenceDependencyKeys.h"
#include "swift/Basic/Statistic.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Driver/Job.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/raw_ostream.h"

// Definitions for the portion experimental dependency system used by the
// driver.

using namespace swift;

using namespace swift::experimental_dependencies;
using namespace swift::driver;

//==============================================================================
// MARK: Interfacing to Compilation
//==============================================================================

using LoadResult = experimental_dependencies::DependencyGraphImpl::LoadResult;

LoadResult DriverGraph::loadFromPath(const Job *Cmd, StringRef path,
                                     DiagnosticEngine &diags) {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer)
    return LoadResult::HadError;
  // For debugging, emit dot files before and after.
  // TODO: add flag to control dot file creation
  emitDotFileForJob(diags, Cmd);
  verify();
  auto r = loadFromBuffer(Cmd, *buffer.get());
  emitDotFileForJob(diags, Cmd);
  verify();
  return r;
}

LoadResult DriverGraph::loadFromBuffer(const Job *job,
                                       llvm::MemoryBuffer &buffer) {

  Optional<FrontendGraph> fg = FrontendGraph::loadFromBuffer(buffer);
  if (!fg)
    return DependencyGraphImpl::LoadResult::HadError;
  addIndependentNode(job);
  return integrate(fg.getValue());
}

bool DriverGraph::isMarked(const Job *cmd) const {
  return cascadingJobs.count(getSwiftDeps(cmd));
}

void DriverGraph::markTransitive(
    SmallVectorImpl<const Job *> &visited, const Job *job,
    DependencyGraph<const Job *>::MarkTracer *tracer) {
  std::unordered_set<const DriverNode *> visitedNodeSet;
  const StringRef swiftDeps = getSwiftDeps(job);
  // Do the traversal.
  for (auto &fileAndNode : nodeMap[swiftDeps]) {
    checkTransitiveClosureForCascading(visitedNodeSet, fileAndNode.second);
  }
  // Copy back visited jobs.
  std::unordered_set<std::string> visitedSwiftDeps;
  for (const DriverNode *n : visitedNodeSet) {
    if (!n->getSwiftDeps().hasValue())
      continue;
    const std::string &swiftDeps = n->getSwiftDeps().getValue();
    if (visitedSwiftDeps.insert(swiftDeps).second) {
      assert(n->assertImplementationsMustBeInFiles());
      ensureJobIsTracked(swiftDeps);
      visited.push_back(getJob(swiftDeps));
    }
  }
}

bool DriverGraph::markIntransitive(const Job *node) {
  return cascadingJobs.insert(getSwiftDeps(node)).second;
}

void DriverGraph::addIndependentNode(const Job *job) {
  // No need to create any nodes; that will happen when the swiftdeps file is
  // read. Just record the correspondence.
  jobsBySwiftDeps.insert(std::make_pair(getSwiftDeps(job), job));
}

std::vector<std::string> DriverGraph::getExternalDependencies() const {
  std::vector<std::string> out;
  std::copy(externalDependencies.begin(), externalDependencies.end(),
            std::back_inserter(out));
  return out;
}

// Add every (swiftdeps) use of the external dependency to uses.
void DriverGraph::markExternal(SmallVectorImpl<const Job *> &uses,
                               StringRef externalDependency) {
  // TODO move nameForDep into key
  // These nodes will depend on the *interface* of the external Decl.
  DependencyKey key =
      DependencyKey::createDependedUponKey<NodeKind::externalDepend>(
          externalDependency.str());
  // collect answers into useSet
  std::unordered_set<std::string> visitedSet;
  for (const DependencyKey &keyOfUse : usesByDef[key]) {
    nodeMap.forEachValueMatching(keyOfUse,
                                 [&](const std::string &, DriverNode *n) {
                                   const Job *job = getJob(n->getSwiftDeps());
                                   if (isMarked(job))
                                     return;
                                   uses.push_back(job);
                                   markTransitive(uses, job);
                                 });
  }
}

//==============================================================================
// MARK: Integrating FrontendGraph into DriverGraph
//==============================================================================

LoadResult DriverGraph::integrate(const FrontendGraph &g) {
  StringRef swiftDeps = g.getSwiftDepsFromSourceFileProvide();
  // When done, disappearedNodes contains the nodes which no longer exist.
  auto disappearedNodes = nodeMap[swiftDeps];
  // When done, changeDependencyKeys contains a list of keys that changed
  // as a result of this integration.
  auto changedNodes = std::unordered_set<DependencyKey>();

  g.forEachNode([&](const FrontendNode *integrand) {
    verify(); // xxx
    integrateUsesByDef(integrand, g);
    const auto key = integrand->getKey();
    Optional<DriverNode *> prexistingNodeInPlace =
        integrand->getSwiftDeps().hasValue()
            ? nodeMap.find(integrand->getSwiftDeps().getValue(), key)
            : None;
    if (prexistingNodeInPlace)
      disappearedNodes.erase(key);
    const bool changed =
        integrateFrontendNode(integrand, swiftDeps, prexistingNodeInPlace);
    if (changed)
      changedNodes.insert(key);

    // Track externalDependencies so Compilation can check them.
    if (integrand->getKey().getKind() == NodeKind::externalDepend)
      externalDependencies.insert(integrand->getKey().getName());

    verify(); // xxx
  });

  for (auto &p : disappearedNodes) {
    changedNodes.insert(p.second->getKey());
    removeNode(p.second);
  }

  // TODO: use changedKeys sometime, for instance by returning them
  // as part of return value so that the driver can only mark from them.
  return changedNodes.empty() ? LoadResult::UpToDate
                              : LoadResult::AffectsDownstream;
}

bool DriverGraph::integrateFrontendNode(
    const FrontendNode *integrand, StringRef swiftDepsOfFrontendGraph,
    const Optional<DriverNode *> preexistingNodeInPlace) {

  const auto key = integrand->getKey();

  const Optional<DriverNode *> preexistingExpat =
      preexistingNodeInPlace ? None : nodeMap.find("", key);

  const size_t preexistingCount = nodeMap[key].size();

  assert((!preexistingExpat || preexistingCount == 1) &&
         "If there is an expat, must not be any Decls in files.");

  const bool dupsExistInOtherFiles =
      !preexistingNodeInPlace && !preexistingExpat && preexistingCount;

  return integrand->getSwiftDeps().hasValue()
             ? integrateFrontendDeclNode(integrand, preexistingNodeInPlace,
                                         preexistingExpat)
             : integrateFrontendExpatNode(
                   integrand, swiftDepsOfFrontendGraph, preexistingNodeInPlace,
                   preexistingExpat, dupsExistInOtherFiles);
}

bool DriverGraph::integrateFrontendDeclNode(
    const FrontendNode *integrand,
    const Optional<DriverNode *> preexistingNodeInPlace,
    const Optional<DriverNode *> preexistingExpat) {

  const auto key = integrand->getKey();
  if (preexistingNodeInPlace)
    return preexistingNodeInPlace.getValue()->integrateFingerprintFrom(
        integrand);

  if (preexistingExpat) {
    // Some other file depended on this, but didn't know where it was.
    moveNodeToDifferentFile(preexistingExpat.getValue(),
                            integrand->getSwiftDeps());
    preexistingExpat.getValue()->integrateFingerprintFrom(integrand);
    return true; // New Decl, assume changed
  }
  integrateByCreatingANewNode(integrand);
  return true; // New node
}

bool DriverGraph::integrateFrontendExpatNode(
    const FrontendNode *integrand, StringRef swiftDepsOfFrontendGraph,
    const Optional<DriverNode *> preexistingNodeInPlace,
    const Optional<DriverNode *> preexistingExpat, bool dupsExistInOtherFiles) {

  const auto key = integrand->getKey();

  if (dupsExistInOtherFiles || preexistingExpat) {
    // Integrand is a dependency from another file, and we already have a def
    // node for that. Nothing to be done.
    assert(!integrand->getFingerprint().hasValue() &&
           "If extra-file dependencies were to have fingerprints, would need "
           "to do something more.");
    return false;
  }
  if (preexistingNodeInPlace) {
    preexistingNodeInPlace.getValue()->integrateFingerprintFrom(integrand);
    moveNodeToDifferentFile(preexistingNodeInPlace.getValue(), None);
  } else
    integrateByCreatingANewNode(integrand);
  return true;
}

DriverNode *
DriverGraph::integrateByCreatingANewNode(const FrontendNode *integrand) {
  const auto &key = integrand->getKey();
  DriverNode *newNode = new DriverNode(key, integrand->getFingerprint());
  newNode->setSwiftDeps(integrand->getSwiftDeps());
  assert(integrand->assertImplementationsMustBeInFiles());
  addToMap(newNode);
  return newNode;
}

void DriverGraph::integrateUsesByDef(const FrontendNode *n,
                                     const FrontendGraph &g) {
  const auto &def = n->getKey();
  auto &uses = usesByDef[def];
  g.forEachUseOf(n, [&](const FrontendNode *useNode) {
    const auto &use = useNode->getKey();
    if (use != def)
      uses.insert(use);
  });
}

void DriverGraph::removeNode(DriverNode *n) {
  eraseNodeFromMap(n);
  delete n;
}

//==============================================================================
// MARK: DriverGraph access
//==============================================================================

void DriverGraph::forEachUseOf(
    const DriverNode *def, llvm::function_ref<void(const DriverNode *)> fn) {
  auto iter = usesByDef.find(def->getKey());
  if (iter == usesByDef.end())
    return;
  for (const DependencyKey &useKey : iter->second)
    forEachMatchingNode(useKey, fn);
}

void DriverGraph::forEachNode(
    llvm::function_ref<void(const DriverNode *)> fn) const {
  nodeMap.forEachEntry([&](const std::string &, const DependencyKey &,
                           DriverNode *n) { fn(n); });
}

void DriverGraph::forEachMatchingNode(
    const DependencyKey &key, function_ref<void(const DriverNode *)> fn) const {
  nodeMap.forEachValueMatching(
      key, [&](const std::string &, DriverNode *n) { fn(n); });
}

void DriverGraph::forEachArc(
    llvm::function_ref<void(const DriverNode *, const DriverNode *)> fn) const {
  /// Use find instead of [] because this is const
  for (const auto &defUse : usesByDef)
    forEachMatchingNode(defUse.first, [&](const DriverNode *defNode) {
      for (const auto &useKey : defUse.second)
        forEachMatchingNode(
            useKey, [&](const DriverNode *useNode) { fn(defNode, useNode); });
    });
}

//==============================================================================
// MARK: DriverGraph traversal
//==============================================================================

// Could be faster by passing in a file, not a node, but we are trying for
// generality.
// The status quo system doesn't traverse past "Marked" nodes.
// I'm not sure that will be safe when we get fingerprints.
// Seems like no harm, just more time spent, by traversing through "Marked"
// nodes.
void DriverGraph::checkTransitiveClosureForCascading(
    std::unordered_set<const DriverNode *> &visited,
    const DriverNode *potentiallyCascadingDef) {
  // Cycle recording and check.
  if (!visited.insert(potentiallyCascadingDef).second)
    return;
  // Moved this out of the following loop for effieciency.
  assert(potentiallyCascadingDef->getSwiftDeps().hasValue() &&
         "Should only call me for Decl nodes.");
  const StringRef swiftDeps =
      potentiallyCascadingDef->getSwiftDeps().getValue();

  forEachUseOf(potentiallyCascadingDef, [&](const DriverNode *u) {
    if (u->getKey().isInterface())
      rememberThatJobCascades(swiftDeps);
    checkTransitiveClosureForCascading(visited, u);
  });
}

// Emitting Dot file for DriverGraph ===========================================

void DriverGraph::emitDotFileForJob(DiagnosticEngine &diags, const Job *job) {
  emitDotFile(diags, dotFilenameForJob(job));
}

std::string DriverGraph::dotFilenameForJob(const Job *job) {
  StringRef dependenciesFile = getSwiftDeps(job);
  unsigned seqNo = dotFileSequenceNumberByJob[job]++;
  return dependenciesFile.str() + "." + std::to_string(seqNo) + ".dot";
}

void DriverGraph::emitDotFile(DiagnosticEngine &diags, StringRef outputPath) {
  withOutputFile(diags, outputPath, [&](llvm::raw_ostream &out) {
    emitDotFile(out);
    return false;
  });
}

void DriverGraph::emitDotFile(llvm::raw_ostream &out) {
  DotFileEmitter<DriverGraph>(out, *this, true, false).emit();
}

//==============================================================================
// MARK: DriverGraph debugging
//==============================================================================

void DriverGraph::verify() const {
  // TODO: disable when not debugging
  std::array<std::unordered_map<DependencyKey,
                                std::unordered_map<std::string, DriverNode *>>,
             2>
      nodesByKey;
  nodeMap.verify([&](const std::string &swiftDepsString,
                     const DependencyKey &key, DriverNode *n,
                     unsigned mapIndex) {
    assert(mapIndex < nodesByKey.size());

    auto &nodesBySwiftDeps = nodesByKey[mapIndex][n->getKey()];
    auto iterInserted = nodesBySwiftDeps.insert(std::make_pair(
        n->getSwiftDeps().hasValue() ? n->getSwiftDeps().getValue()
                                     : std::string(),
        n));
    if (!iterInserted.second) {
      llvm_unreachable("duplicate driver keys");
    }
    const DependencyKey &nodeKey = n->getKey();
    const Optional<std::string> swiftDeps =
        swiftDepsString.empty() ? None : Optional<std::string>(swiftDepsString);
    assert(n->getSwiftDeps() == swiftDeps ||
           mapCorruption("Node misplaced for swiftDeps"));
    assert(nodeKey == key || mapCorruption("Node misplaced for key"));
    nodeKey.verify();
    assert((nodeKey.getKind() != NodeKind::externalDepend ||
            externalDependencies.count(nodeKey.getName()) == 1) &&
           "Ensure each external dependency is tracked exactly once");
  });
  for (const auto p : jobsBySwiftDeps) {
    getJob(p.first);
  }
  nodeMap.forEachKey1(
      [&](const std::string &swiftDeps, const typename NodeMap::Key2Map &) {
        ensureJobIsTracked(swiftDeps);
      });
}
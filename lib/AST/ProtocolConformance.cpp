//===--- ProtocolConformance.cpp - AST Protocol Conformance -----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements the protocol conformance data structures.
//
//===----------------------------------------------------------------------===//
#include "swift/AST/ASTContext.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Module.h"
#include "swift/AST/Types.h"

using namespace swift;

void *ProtocolConformance::operator new(size_t bytes, ASTContext &context,
                                        unsigned alignment) {
  return context.Allocate(bytes, alignment);

}

#define CONFORMANCE_SUBCLASS_DISPATCH(Method, Args)                        \
switch (getKind()) {                                                       \
  case ProtocolConformanceKind::Normal:                                    \
    static_assert(&ProtocolConformance::Method !=                          \
                    &NormalProtocolConformance::Method,                    \
                  "Must override NormalProtocolConformance::" #Method);    \
    return cast<NormalProtocolConformance>(this)->Method Args;             \
  case ProtocolConformanceKind::Specialized:                               \
    static_assert(&ProtocolConformance::Method !=                          \
                    &InheritedProtocolConformance::Method,                 \
                  "Must override InheritedProtocolConformance::" #Method); \
    return cast<SpecializedProtocolConformance>(this)->Method Args;        \
  case ProtocolConformanceKind::Inherited:                                 \
    static_assert(&ProtocolConformance::Method !=                          \
                    &InheritedProtocolConformance::Method,                 \
                  "Must override InheritedProtocolConformance::" #Method); \
    return cast<InheritedProtocolConformance>(this)->Method Args;          \
}

/// Get the protocol being conformed to.
ProtocolDecl *ProtocolConformance::getProtocol() const {
  CONFORMANCE_SUBCLASS_DISPATCH(getProtocol, ())
}

DeclContext *ProtocolConformance::getDeclContext() const {
  CONFORMANCE_SUBCLASS_DISPATCH(getDeclContext, ())
}

/// Retrieve the state of this conformance.
ProtocolConformanceState ProtocolConformance::getState() const {
  CONFORMANCE_SUBCLASS_DISPATCH(getState, ())
}

const Substitution &
ProtocolConformance::getTypeWitness(AssociatedTypeDecl *assocType, 
                                    LazyResolver *resolver) const {
  CONFORMANCE_SUBCLASS_DISPATCH(getTypeWitness, (assocType, resolver))
}

ConcreteDeclRef ProtocolConformance::getWitness(ValueDecl *requirement,
                                               LazyResolver *resolver) const {
  CONFORMANCE_SUBCLASS_DISPATCH(getWitness, (requirement, resolver))
}

const InheritedConformanceMap &
ProtocolConformance::getInheritedConformances() const {
  CONFORMANCE_SUBCLASS_DISPATCH(getInheritedConformances, ())
}

/// Determine whether the witness for the given requirement
/// is either the default definition or was otherwise deduced.
bool ProtocolConformance::usesDefaultDefinition(ValueDecl *requirement) const {
  CONFORMANCE_SUBCLASS_DISPATCH(usesDefaultDefinition, (requirement))
}

GenericParamList *ProtocolConformance::getGenericParams() const {
  switch (getKind()) {
  case ProtocolConformanceKind::Normal: {
    // FIXME: This should be an independent property of the conformance.
    // Assuming a BoundGenericType conformance is always for the
    // DeclaredTypeInContext is unsound if we ever add constrained extensions.
    Type ty = getType();
    while (ty) {
      if (auto nt = ty->getAs<NominalType>())
        ty = nt->getParent();
      else
        break;
    }
    
    if (!ty)
      return nullptr;
    
    if (auto bgt = ty->getAs<BoundGenericType>()) {
      auto decl = bgt->getDecl();
      assert(bgt->isEqual(decl->getDeclaredTypeInContext())
             && "conformance for constrained generic type not implemented");
      return decl->getGenericParams();
    }
    return nullptr;
  }
  case ProtocolConformanceKind::Specialized:
  case ProtocolConformanceKind::Inherited:
    // FIXME: These could reasonably have open type variables.
    return nullptr;
  }
}

void NormalProtocolConformance::setTypeWitness(
       AssociatedTypeDecl *assocType,
       const Substitution &substitution) {
  assert(getProtocol() == cast<ProtocolDecl>(assocType->getDeclContext()) &&
         "associated type in wrong protocol");
  assert(TypeWitnesses.count(assocType) == 0 && "Type witness already known");
  assert(!isComplete() && "Conformance already complete?");
  TypeWitnesses[assocType] = substitution;
}

void NormalProtocolConformance::setWitness(ValueDecl *requirement,
                                           ConcreteDeclRef witness) {
  assert(!isa<AssociatedTypeDecl>(requirement) && "Request type witness");
  assert(getProtocol() == cast<ProtocolDecl>(requirement->getDeclContext()) &&
         "requirement in wrong protocol");
  assert(Mapping.count(requirement) == 0 && "Witness already known");
  assert(!isComplete() && "Conformance already complete?");
  Mapping[requirement] = witness;
}

const Substitution &SpecializedProtocolConformance::getTypeWitness(
                      AssociatedTypeDecl *assocType, 
                      LazyResolver *resolver) const {
  // If we've already created this type witness, return it.
  auto known = TypeWitnesses.find(assocType);
  if (known != TypeWitnesses.end()) {
    return known->second;
  }

  // Otherwise, perform substitutions to create this witness now.
  TypeSubstitutionMap substitutionMap;
  for (const auto &substitution : GenericSubstitutions) {
    substitutionMap[substitution.Archetype] = substitution.Replacement;
  }

  auto &genericWitness = GenericConformance->getTypeWitness(assocType, resolver);
  auto conformingDC = getDeclContext();
  auto conformingModule = conformingDC->getParentModule();
  auto specializedType
    = genericWitness.Replacement.subst(conformingModule,
                                       substitutionMap,
                                       /*ignoreMissing=*/false,
                                       resolver);

  // If the type witness was unchanged, just copy it directly.
  if (specializedType.getPointer() == genericWitness.Replacement.getPointer()) {
    TypeWitnesses[assocType] = genericWitness;
    return TypeWitnesses[assocType];
  }

  // Gather the conformances for the type witness. These should never fail.
  SmallVector<ProtocolConformance *, 4> conformances;
  auto archetype = genericWitness.Archetype;
  for (auto proto : archetype->getConformsTo()) {
    auto conforms = conformingModule->lookupConformance(specializedType, proto,
                                                    resolver);
    assert(conforms.getInt() == ConformanceKind::Conforms &&
           "Improperly checked substitution");
    conformances.push_back(conforms.getPointer());
  }

  // Form the substitution.
  auto &ctx = assocType->getASTContext();
  TypeWitnesses[assocType] = Substitution{archetype, specializedType,
                                          ctx.AllocateCopy(conformances)};
  return TypeWitnesses[assocType];
}

ConcreteDeclRef
SpecializedProtocolConformance::getWitness(ValueDecl *requirement,
                                           LazyResolver *resolver) const {
  // FIXME: Apply substitutions here!
  return GenericConformance->getWitness(requirement, resolver);
}


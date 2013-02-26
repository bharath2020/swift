//===--- GenObjC.cpp - Objective-C interaction ----------------------------===//
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
//  This file implements bridging to Objective-C.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"

#include "swift/IRGen/Options.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Types.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclObjC.h"

#include "ASTVisitor.h"
#include "CallEmission.h"
#include "Cleanup.h"
#include "Explosion.h"
#include "FixedTypeInfo.h"
#include "FunctionRef.h"
#include "GenFunc.h"
#include "GenMeta.h"
#include "GenType.h"
#include "HeapTypeInfo.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "ScalarTypeInfo.h"
#include "TypeVisitor.h"

#include "GenObjC.h"

using namespace swift;
using namespace irgen;

/// Create an Objective-C runtime function.  The ObjC runtime uses the
/// standard C conventions.
static llvm::Constant *createObjCRuntimeFunction(IRGenModule &IGM, StringRef name,
                                                 llvm::FunctionType *fnType) {
  llvm::Constant *addr = IGM.Module.getOrInsertFunction(name, fnType);
  return addr;
}

namespace {
  template<llvm::Constant * (IRGenModule::*FIELD), char const *NAME>
  llvm::Constant *getObjCSendFn(IRGenModule &IGM) {
    if (IGM.*FIELD)
      return IGM.*FIELD;

    // We use a totally bogus signature to make sure we *always* cast.
    llvm::FunctionType *fnType =
      llvm::FunctionType::get(IGM.VoidTy, ArrayRef<llvm::Type*>(), false);
    IGM.*FIELD = createObjCRuntimeFunction(IGM, NAME, fnType);
    return IGM.*FIELD;
  }
  // T objc_msgSend(id, SEL*, U...);
  static const char objc_msgSend_name[] = "objc_msgSend";
  // void objc_msgSend_stret([[sret]] T *, id, SEL, U...);
  static const char objc_msgSend_stret_name[] = "objc_msgSend_stret";
  // T objc_msgSendSuper2(struct objc_super *, SEL, U...);
  static const char objc_msgSendSuper_name[] = "objc_msgSendSuper2";
  // void objc_msgSendSuper2_stret([[sret]] T *, struct objc_super *, SEL, U...);
  static const char objc_msgSendSuper_stret_name[]
    = "objc_msgSendSuper2_stret";

} // end anonymous namespace



llvm::Constant *IRGenModule::getObjCMsgSendFn() {
  return getObjCSendFn<&IRGenModule::ObjCMsgSendFn, objc_msgSend_name>(*this);
}

llvm::Constant *IRGenModule::getObjCMsgSendStretFn() {
  return getObjCSendFn<&IRGenModule::ObjCMsgSendStretFn,
                       objc_msgSend_stret_name>(*this);
}

llvm::Constant *IRGenModule::getObjCMsgSendSuperFn() {
  return getObjCSendFn<&IRGenModule::ObjCMsgSendSuperFn,
                       objc_msgSendSuper_name>(*this);
}

llvm::Constant *IRGenModule::getObjCMsgSendSuperStretFn() {
  return getObjCSendFn<&IRGenModule::ObjCMsgSendSuperStretFn,
                       objc_msgSendSuper_stret_name>(*this);
}

llvm::Constant *IRGenModule::getObjCSelRegisterNameFn() {
  if (ObjCSelRegisterNameFn) return ObjCSelRegisterNameFn;

  // SEL sel_registerName(const char *str);
  llvm::Type *argTypes[1] = { Int8PtrTy };
  auto fnType = llvm::FunctionType::get(ObjCSELTy, argTypes, false);
  ObjCSelRegisterNameFn = createObjCRuntimeFunction(*this, "sel_registerName",
                                                    fnType);
  return ObjCSelRegisterNameFn;
}

#define DEFINE_OBJC_RUNTIME_FUNCTION(LABEL, NAME, RETTY)            \
llvm::Constant *IRGenModule::getObjC##LABEL##Fn() {                 \
  if (ObjC##LABEL##Fn) return ObjC##LABEL##Fn;                      \
                                                                    \
  llvm::FunctionType *fnType =                                      \
    llvm::FunctionType::get(RETTY, ObjCPtrTy, false);               \
  ObjC##LABEL##Fn = createObjCRuntimeFunction(*this, NAME, fnType); \
  return ObjC##LABEL##Fn;                                           \
}

DEFINE_OBJC_RUNTIME_FUNCTION(Retain,
                             "objc_retain",
                             ObjCPtrTy)
DEFINE_OBJC_RUNTIME_FUNCTION(RetainAutoreleasedReturnValue,
                             "objc_retainAutoreleasedReturnValue",
                             ObjCPtrTy)
DEFINE_OBJC_RUNTIME_FUNCTION(Release,
                             "objc_release",
                             VoidTy)
DEFINE_OBJC_RUNTIME_FUNCTION(AutoreleaseReturnValue,
                             "objc_autoreleaseReturnValue",
                             ObjCPtrTy);

void IRGenFunction::emitObjCRelease(llvm::Value *value) {
  // Get an appropriately-casted function pointer.
  auto fn = IGM.getObjCReleaseFn();
  if (value->getType() != IGM.ObjCPtrTy) {
    auto fnTy = llvm::FunctionType::get(IGM.VoidTy, value->getType(),
                                        false)->getPointerTo();
    fn = llvm::ConstantExpr::getBitCast(fn, fnTy);
  }

  auto call = Builder.CreateCall(fn, value);
  call->setDoesNotThrow();
}

/// Given a function of type %objc* (%objc*)*, cast it as appropriate
/// to be used with values of type T.
static llvm::Constant *getCastOfRetainFn(IRGenModule &IGM,
                                         llvm::Constant *fn,
                                         llvm::Type *valueTy) {
#ifndef NDEBUG
  auto origFnTy = cast<llvm::FunctionType>(fn->getType()->getPointerElementType());
  assert(origFnTy->getReturnType() == IGM.ObjCPtrTy);
  assert(origFnTy->getNumParams() == 1);
  assert(origFnTy->getParamType(0) == IGM.ObjCPtrTy);
  assert(isa<llvm::PointerType>(valueTy));
#endif
  if (valueTy == IGM.ObjCPtrTy)
    return fn;

  auto fnTy = llvm::FunctionType::get(valueTy, valueTy, false);
  return llvm::ConstantExpr::getBitCast(fn, fnTy->getPointerTo(0));
}

llvm::Value *IRGenFunction::emitObjCRetainCall(llvm::Value *value) {
  // Get an appropriately-casted function pointer.
  auto fn = IGM.getObjCRetainFn();
  fn = getCastOfRetainFn(IGM, fn, value->getType());

  auto call = Builder.CreateCall(fn, value);
  call->setDoesNotThrow();
  return call;
}

/// Reclaim an autoreleased return value.
llvm::Value *irgen::emitObjCRetainAutoreleasedReturnValue(IRGenFunction &IGF,
                                                          llvm::Value *value) {
  auto fn = IGF.IGM.getObjCRetainAutoreleasedReturnValueFn();
  fn = getCastOfRetainFn(IGF.IGM, fn, value->getType());

  auto call = IGF.Builder.CreateCall(fn, value);
  call->setDoesNotThrow();
  return call;
}

/// Autorelease a return value.
static llvm::Value *emitObjCAutoreleaseReturnValue(IRGenFunction &IGF,
                                                   llvm::Value *value) {
  auto fn = IGF.IGM.getObjCAutoreleaseReturnValueFn();
  fn = getCastOfRetainFn(IGF.IGM, fn, value->getType());

  auto call = IGF.Builder.CreateCall(fn, value);
  call->setDoesNotThrow();
  call->setTailCall(); // force tail calls at -O0
  return call;
}

namespace {
  struct CallObjCRelease : Cleanup {
    llvm::Value *Value;
    CallObjCRelease(llvm::Value *value) : Value(value) {}

    void emit(IRGenFunction &IGF) const {
      IGF.emitObjCRelease(Value);
    }
  };
}

ManagedValue IRGenFunction::enterObjCReleaseCleanup(llvm::Value *value) {
  pushFullExprCleanup<CallObjCRelease>(value);
  return ManagedValue(value, getCleanupsDepth());
}

namespace {
  /// A type-info implementation suitable for an ObjC pointer type.
  class ObjCTypeInfo : public HeapTypeInfo<ObjCTypeInfo> {
  public:
    ObjCTypeInfo(llvm::PointerType *storageType, Size size, Alignment align)
      : HeapTypeInfo(storageType, size, align) {
    }

    /// Builtin.ObjCPointer requires ObjC reference-counting.
    bool hasSwiftRefcount() const { return false; }
  };
}

const TypeInfo *TypeConverter::convertBuiltinObjCPointer() {
  return new ObjCTypeInfo(IGM.ObjCPtrTy, IGM.getPointerSize(),
                          IGM.getPointerAlignment());
}

/// Get or create a global Objective-C method name.  Always returns an i8*.
llvm::Constant *IRGenModule::getAddrOfObjCMethodName(StringRef selector) {
  // Check whether this selector already exists.
  auto &entry = ObjCMethodNames[selector];
  if (entry) return entry;

  // If not, create it.  This implicitly adds a trailing null.
  auto init = llvm::ConstantDataArray::getString(LLVMContext, selector);
  auto global = new llvm::GlobalVariable(Module, init->getType(), true,
                                         llvm::GlobalValue::InternalLinkage,
                                         init,
                          llvm::Twine("\01L_selector_data(") + selector + ")");
  global->setSection("__TEXT,__objc_methname,cstring_literals");
  global->setAlignment(1);

  // Drill down to make an i8*.
  auto zero = llvm::ConstantInt::get(SizeTy, 0);
  llvm::Constant *indices[] = { zero, zero };
  auto address = llvm::ConstantExpr::getInBoundsGetElementPtr(global, indices);

  // Cache and return.
  entry = address;
  return address;
}

/// Get or create an Objective-C selector reference.  Always returns
/// an i8**.  The design is that the compiler will emit a load of this
/// pointer, and the linker will ensure that that pointer is unique.
llvm::Constant *IRGenModule::getAddrOfObjCSelectorRef(StringRef selector) {
  // Check whether a reference for this selector already exists.
  auto &entry = ObjCSelectorRefs[selector];
  if (entry) return entry;

  // If not, create it.  The initializer is just a pointer to the
  // method name.  Note that the label here is unimportant, so we
  // choose something descriptive to make the IR readable.
  auto init = getAddrOfObjCMethodName(selector);
  auto global = new llvm::GlobalVariable(Module, init->getType(), false,
                                         llvm::GlobalValue::InternalLinkage,
                                         init,
                                llvm::Twine("\01L_selector(") + selector + ")");
  global->setAlignment(getPointerAlignment().getValue());

  // This section name is magical for the Darwin static and dynamic linkers.
  global->setSection("__DATA,__objc_selrefs,literal_pointers,no_dead_strip");

  // Make sure that this reference does not get optimized away.
  addUsedGlobal(global);

  // Cache and return.
  entry = global;
  return global;
}

/// Determine the natural limits on how we can call the given method
/// using Objective-C method dispatch.
AbstractCallee irgen::getAbstractObjCMethodCallee(IRGenFunction &IGF,
                                                  FuncDecl *fn) {
  return AbstractCallee(AbstractCC::C, ExplosionKind::Minimal,
                        /*minUncurry*/ 1, /*maxUncurry*/ 1,
                        ExtraData::None);
}

namespace {
  struct ObjCMethodSignature {
    bool IsIndirectReturn;
    llvm::FunctionType *FnTy;
    CanType ResultType;
    llvm::AttributeSet Attrs;
    
    void addFormalArg(IRGenModule &IGM,
                      CanType inputTy,
                      SmallVectorImpl<llvm::Type*> &argTys) {
      // This is a totally wrong and shameful hack, but it lets us pass
      // NSRect correctly.
      if (requiresExternalByvalArgument(IGM, inputTy)) {
        const TypeInfo &ti = IGM.getFragileTypeInfo(inputTy);
        llvm::Constant *alignConstant = ti.getStaticAlignment(IGM);
        unsigned alignValue = alignConstant
          ? alignConstant->getUniqueInteger().getZExtValue()
          : 1;
        addByvalArgumentAttributes(IGM, Attrs, argTys.size(),
                                   Alignment(alignValue));
        argTys.push_back(ti.getStorageType()->getPointerTo());
      } else {
        auto argSchema = IGM.getSchema(inputTy,
                                       ExplosionKind::Minimal);
        argSchema.addToArgTypes(IGM, argTys);
      }
    }

    ObjCMethodSignature(IRGenModule &IGM, CanType formalType, bool isSuper) {
      auto selfFnType = cast<AnyFunctionType>(formalType);
      auto formalFnType = cast<AnyFunctionType>(CanType(selfFnType->getResult()));

      llvm::Type *resultTy;
      SmallVector<llvm::Type*, 8> argTys;

      // Consider the result type first.
      ResultType = CanType(formalFnType->getResult());
      if (auto ptrTy = requiresExternalIndirectResult(IGM, ResultType)) {
        IsIndirectReturn = true;
        resultTy = IGM.VoidTy;
        argTys.push_back(ptrTy);
        addIndirectReturnAttributes(IGM, Attrs);
      } else {
        IsIndirectReturn = false;

        auto resultSchema = IGM.getSchema(ResultType, ExplosionKind::Minimal);
        assert(!resultSchema.containsAggregate());
        resultTy = resultSchema.getScalarResultType(IGM);
      }

      // Add the 'self' or 'super' argument.
      if (isSuper)
        argTys.push_back(IGM.ObjCSuperPtrTy);
      else
        argTys.push_back(IGM.getFragileType(CanType(selfFnType->getInput())));

      // Add the _cmd argument.
      argTys.push_back(IGM.ObjCSELTy);

      // Add the formal arguments.
      CanType inputs(formalFnType->getInput());
      if (TupleType *tuple = dyn_cast<TupleType>(inputs)) {
        for (const TupleTypeElt &field : tuple->getFields())
          addFormalArg(IGM, CanType(field.getType()), argTys);
      } else
        addFormalArg(IGM, inputs, argTys);

      FnTy = llvm::FunctionType::get(resultTy, argTys, /*variadic*/ false);
    }
  };

  class Selector {
    llvm::SmallString<80> Text;

  public:

#define FOREACH_FAMILY(FAMILY)         \
    FAMILY(Alloc, "alloc")             \
    FAMILY(Copy, "copy")               \
    FAMILY(Init, "init")               \
    FAMILY(MutableCopy, "mutableCopy") \
    FAMILY(New, "new")

    // Note that these are in parallel with 'prefixes', below.
    enum class Family {
      None,
#define GET_LABEL(LABEL, PREFIX) LABEL,
      FOREACH_FAMILY(GET_LABEL)
#undef GET_LABEL
    };

    Selector(FuncDecl *method) {
      method->getObjCSelector(Text);
    }

    StringRef str() const {
      return Text;
    }

    /// Return the family string of this selector.
    Family getFamily() const {
      StringRef text = str();
      while (!text.empty() && text[0] == '_') text = text.substr(1);

#define CHECK_PREFIX(LABEL, PREFIX) \
      if (hasPrefix(text, PREFIX)) return Family::LABEL;
      FOREACH_FAMILY(CHECK_PREFIX)
#undef CHECK_PREFIX

      return Family::None;
    }

  private:
    /// Does the given selector start with the given string as a
    /// prefix, in the sense of the selector naming conventions?
    static bool hasPrefix(StringRef text, StringRef prefix) {
      if (!text.startswith(prefix)) return false;
      if (text.size() == prefix.size()) return true;
      assert(text.size() > prefix.size());
      return !islower(text[prefix.size()]);
    }

#undef FOREACH_FAMILY
  };

  /// A CRTP class for emitting an expression as an ObjC class
  /// reference.
  class ObjCClassEmitter
      : public irgen::ExprVisitor<ObjCClassEmitter, llvm::Value*> {
    IRGenFunction &IGF;
    typedef irgen::ExprVisitor<ObjCClassEmitter,llvm::Value*> super;
  public:
    ObjCClassEmitter(IRGenFunction &IGF) : IGF(IGF) {}

    llvm::Value *visit(Expr *E) {
      assert(E->getType()->is<MetaTypeType>());
      assert(E->getType()->castTo<MetaTypeType>()->getInstanceType()
                         ->getClassOrBoundGenericClass() != nullptr);
      return super::visit(E);
    }

    /// Look through metatype conversions.
    llvm::Value *visitMetatypeConversionExpr(MetatypeConversionExpr *E) {
      return visit(E->getSubExpr());
    }

    llvm::Value *emitGetMetatype(Expr *base) {
      Explosion temp(ExplosionKind::Maximal);
      IGF.emitRValue(base, temp);
      auto value = temp.claimNext().getValue(); // let the cleanup happen
      auto baseType = base->getType()->getCanonicalType();
      return emitHeapMetadataRefForHeapObject(IGF, value, baseType);
    }

    /// Special-case the .metatype implicit conversion.
    llvm::Value *visitGetMetatypeExpr(GetMetatypeExpr *E) {
      return emitGetMetatype(E->getSubExpr());
    }

    /// Special-case explicit .metatype expressions.
    llvm::Value *visitMetatypeExpr(MetatypeExpr *E) {
      // If there's a base, we need to evaluate it and then grab the
      // ObjC class for that.
      if (Expr *base = E->getBase()) {
        return emitGetMetatype(base);
      }

      // Otherwise, we need to emit a class reference.
      return emitClassHeapMetadataRef(IGF, getInstanceType(E));
    }

    /// Special-case direct type references.
    llvm::Value *visitDeclRefExpr(DeclRefExpr *E) {
      assert(isa<TypeDecl>(E->getDecl()));
      auto typeDecl = cast<TypeDecl>(E->getDecl());
      auto type = typeDecl->getDeclaredType()->getCanonicalType();
      return emitClassHeapMetadataRef(IGF, type);
    }

    /// In the fallback case, emit as a swift metatype and remap it to
    /// an ObjC class type.
    llvm::Value *visitExpr(Expr *E) {
      Explosion temp(ExplosionKind::Maximal);
      IGF.emitRValue(E, temp);
      assert(temp.size() == 1);
      llvm::Value *metatype = temp.claimUnmanagedNext();
      return emitClassHeapMetadataRefForMetatype(IGF, metatype,
                                                 getInstanceType(E));
    }

    /// Given an expression of metatype type, return the instance
    /// type of the metatype.
    static CanType getInstanceType(Expr *E) {
      auto type = E->getType()->getCanonicalType();
      return CanType(cast<MetaTypeType>(type)->getInstanceType());
    }
  };

  /// Ownership conventions derived from a Clang method declaration.
  class ObjCMethodConventions : public OwnershipConventions {
    clang::ObjCMethodDecl *Method;
  public:
    ObjCMethodConventions(clang::ObjCMethodDecl *method)
      : Method(method) {}

    bool isResultAutoreleased(IRGenModule &IGM,
                              const Callee &callee) const override {
      return (Method->getResultType()->isObjCRetainableType() &&
              !Method->hasAttr<clang::NSReturnsRetainedAttr>());
    }

    void getConsumedArgs(IRGenModule &IGM, const Callee &callee,
                         SmallVectorImpl<unsigned> &set) const override {
      // 'self'
      if (Method->hasAttr<clang::NSConsumesSelfAttr>())
        set.push_back(0);

      // Formal parameters.
      unsigned nextArgIndex = 2;
      unsigned methodParamIndex = 0;

      auto type = cast<FunctionType>(callee.getOrigFormalType());
      type = cast<FunctionType>(CanType(type->getResult()));
      addConsumedArgs(IGM, callee, set, CanType(type->getInput()),
                      nextArgIndex, methodParamIndex);
    }

    void addConsumedArgs(IRGenModule &IGM, const Callee &callee,
                         SmallVectorImpl<unsigned> &set, CanType argType,
                         unsigned &nextArgIndex,
                         unsigned &methodParamIndex) const {
      if (auto tuple = dyn_cast<TupleType>(argType)) {
        for (auto &elt : tuple->getFields()) {
          addConsumedArgs(IGM, callee, set, CanType(elt.getType()),
                          nextArgIndex, methodParamIndex);
        }
        return;
      }

      assert(methodParamIndex < Method->param_size());
      auto param = *(Method->param_begin() + methodParamIndex);
      assert(param);
      if (param->hasAttr<clang::NSConsumedAttr>()) {
        set.push_back(nextArgIndex++);
      } else {
        nextArgIndex +=
          IGM.getExplosionSize(argType, callee.getExplosionLevel());
      }
      methodParamIndex++;
    }
  };

  /// Ownership conventions derived from a selector family.
  class ObjCSelectorConventions : public OwnershipConventions {
    CanType SubstResultType;
    Selector::Family Family;
  public:
    ObjCSelectorConventions(CanType substResultType, Selector::Family family)
      : SubstResultType(substResultType), Family(family) {}

    bool isResultAutoreleased(IRGenModule &IGM,
                              const Callee &callee) const override {
      // If the result type isn't a retainable object pointer, this
      // isn't applicable.
      if (!SubstResultType->getClassOrBoundGenericClass())
        return false;

      switch (Family) {
      case Selector::Family::Alloc:
      case Selector::Family::Init:
      case Selector::Family::Copy:
      case Selector::Family::MutableCopy:
      case Selector::Family::New:
        return false;

      case Selector::Family::None:
        return true;
      }
      llvm_unreachable("bad selector family!");
    }

    void getConsumedArgs(IRGenModule &IGM, const Callee &callee,
                         SmallVectorImpl<unsigned> &set) const override {
      // The only conventionally-consumed argument is an init method's self.
      if (Family == Selector::Family::Init)
        set.push_back(0);
    }
  };
}

/// Emit the given expression (of class-metatype type) as an
/// Objective-C class reference.
static void emitObjCClassRValue(IRGenFunction &IGF, Expr *E, Explosion &out) {
  out.addUnmanaged(ObjCClassEmitter(IGF).visit(E));
}

/// Try to find a clang method declaration for the given function.
static clang::ObjCMethodDecl *findClangMethod(FuncDecl *method) {
  if (auto decl = method->getClangDecl())
    return cast<clang::ObjCMethodDecl>(decl);

  if (auto overridden = method->getOverriddenDecl())
    return findClangMethod(overridden);

  return nullptr;
}

/// Set the appropriate ownership conventions for an Objective-C
/// method on the given callee.
static const OwnershipConventions &setOwnershipConventions(Callee &callee,
                                                           FuncDecl *method,
                                                     const Selector &selector) {
  if (auto clangDecl = findClangMethod(method)) {
    callee.setOwnershipConventions<ObjCMethodConventions>(clangDecl);
  } else {
    auto substResultType = callee.getSubstResultType();
    auto selectorFamily = selector.getFamily();
    callee.setOwnershipConventions<ObjCSelectorConventions>(substResultType,
                                                            selectorFamily);
  }
  return callee.getOwnershipConventions();
}

static void emitSelfArgument(IRGenFunction &IGF, bool isInstanceMethod,
                             Expr *self, Explosion &selfValues) {
  if (isInstanceMethod) {
    IGF.emitRValue(self, selfValues);
  } else {
    emitObjCClassRValue(IGF, self, selfValues);
  }
}

static void emitSuperArgument(IRGenFunction &IGF, bool isInstanceMethod,
                              Expr *self, Explosion &selfValues,
                              CanType searchClass) {
  // Allocate an objc_super struct.
  Address super = IGF.createAlloca(IGF.IGM.ObjCSuperStructTy,
                                   IGF.IGM.getPointerAlignment(),
                                   "objc_super");
  // Generate the 'self' receiver.
  Explosion selfValueTmp(ExplosionKind::Minimal);
  emitSelfArgument(IGF, isInstanceMethod, self, selfValueTmp);
  llvm::Value *selfValue = selfValueTmp.claimNext().forward(IGF);
  selfValue = IGF.Builder.CreateBitCast(selfValue, IGF.IGM.ObjCPtrTy);
  
  // Generate the search class object reference.
  llvm::Value *searchValue;
  if (isInstanceMethod) {
    searchValue = emitClassHeapMetadataRef(IGF, searchClass);
  } else {
    ClassDecl *searchClassDecl = searchClass->getClassOrBoundGenericClass();
    searchValue = IGF.IGM.getAddrOfMetaclassObject(searchClassDecl);
  }
  searchValue = IGF.Builder.CreateBitCast(searchValue, IGF.IGM.ObjCClassPtrTy);
  
  // Store the receiver and class to the struct.
  llvm::Value *selfIndices[2] = {
    IGF.Builder.getInt32(0),
    IGF.Builder.getInt32(0)
  };
  llvm::Value *selfAddr = IGF.Builder.CreateGEP(super.getAddress(),
                                                selfIndices);
  IGF.Builder.CreateStore(selfValue, selfAddr, super.getAlignment());

  llvm::Value *searchIndices[2] = {
    IGF.Builder.getInt32(0),
    IGF.Builder.getInt32(1)
  };
  llvm::Value *searchAddr = IGF.Builder.CreateGEP(super.getAddress(),
                                                  searchIndices);
  IGF.Builder.CreateStore(searchValue, searchAddr, super.getAlignment());
  
  // Pass a pointer to the objc_super struct to the messenger.
  selfValues.addUnmanaged(super.getAddress());
}

/// Prepare a call using ObjC method dispatch.
CallEmission irgen::prepareObjCMethodCall(IRGenFunction &IGF, FuncDecl *method,
                                          Expr *self,
                                          CanType substResultType,
                                          ArrayRef<Substitution> subs,
                                          ExplosionKind maxExplosion,
                                          unsigned maxUncurry,
                                          CanType searchType) {
  CanType origFormalType = method->getType()->getCanonicalType();
  ObjCMethodSignature sig(IGF.IGM, origFormalType, bool(searchType));

  // Create the appropriate messenger function.
  // FIXME: this needs to be target-specific.
  llvm::Constant *messenger;
  if (sig.IsIndirectReturn) {
    messenger = searchType
      ? IGF.IGM.getObjCMsgSendSuperStretFn()
      : IGF.IGM.getObjCMsgSendStretFn();
  } else {
    messenger = searchType
      ? IGF.IGM.getObjCMsgSendSuperFn()
      : IGF.IGM.getObjCMsgSendFn();
  }

  // Cast the messenger to the right type.
  messenger = llvm::ConstantExpr::getBitCast(messenger,
                                             sig.FnTy->getPointerTo());

  CallEmission emission(IGF, Callee::forKnownFunction(AbstractCC::C,
                                                      origFormalType,
                                                      substResultType,
                                                      subs,
                                                      messenger,
                                                      ManagedValue(nullptr),
                                                      ExplosionKind::Minimal,
                                                      /*uncurry*/ 1));

  // Compute the selector.
  Selector selector(method);

  // Respect conventions.
  Callee &callee = emission.getMutableCallee();
  setOwnershipConventions(callee, method, selector);

  // Emit the self or super argument.
  Explosion selfValues(ExplosionKind::Minimal);
  if (searchType) {
    emitSuperArgument(IGF, method->isInstanceMember(), self, selfValues,
                      searchType);
  } else {
    emitSelfArgument(IGF, method->isInstanceMember(), self, selfValues);
  }
  assert(selfValues.size() == 1);

  // Add the selector value.
  auto selectorRef = IGF.IGM.getAddrOfObjCSelectorRef(selector.str());
  llvm::Value *selectorV;
  if (IGF.IGM.Opts.UseJIT) {
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    selectorV = IGF.Builder.CreateLoad(Address(selectorRef,
                                               IGF.IGM.getPointerAlignment()));
    selectorV = IGF.Builder.CreateCall(IGF.IGM.getObjCSelRegisterNameFn(),
                                       selectorV);
  } else {
    // When generating statically-compiled code, just build a reference to
    // the selector.
    selectorV = IGF.Builder.CreateLoad(Address(selectorRef,
                                               IGF.IGM.getPointerAlignment()));
  }
  selfValues.addUnmanaged(selectorV);

  // Add that to the emission.
  emission.addArg(selfValues);

  return emission;
}

/// Can we use the given method directly as the IMPL of an Objective-C
/// function?
///
/// It is okay for this to conservatively return false.
static bool canUseSwiftFunctionAsObjCFunction(IRGenModule &IGM,
                                        const ObjCMethodSignature &signature,
                                        const OwnershipConventions &ownership,
                                              CanType origFormalType) {
  // TODO: nullary functions that return compatibly should be okay.
  return false;
}

/// Create the LLVM function declaration for a thunk that acts like
/// an Objective-C method for a Swift method implementation.
static llvm::Function *createSwiftAsObjCThunk(IRGenModule &IGM,
                                              const ObjCMethodSignature &sig,
                                              llvm::Function *impl) {
  // Construct the thunk name.
  auto name = impl->getName();
  llvm::SmallString<128> buffer;
  buffer.reserve(name.size() + 2);
  buffer.append("_TTo");
  assert(name.startswith("_T"));
  buffer.append(name.substr(2));

  auto fn = llvm::Function::Create(sig.FnTy, llvm::Function::InternalLinkage,
                                   buffer.str(), &IGM.Module);
  fn->setAttributes(sig.Attrs);
  fn->setUnnamedAddr(true );
  return fn;
}

namespace {
  class TranslateParameters : public irgen::TypeVisitor<TranslateParameters> {
    IRGenFunction &IGF;
    Explosion &Params;
    SmallVectorImpl<llvm::Value*> &Args;
    SmallVectorImpl<unsigned> &ConsumedArgs;
    SmallVectorImpl<unsigned>::const_iterator NextConsumedArg;
    unsigned NextParamIndex = 0;
    
  public:
    TranslateParameters(IRGenFunction &IGF, Explosion &params,
                        SmallVectorImpl<llvm::Value*> &args,
                        SmallVectorImpl<unsigned> &consumedArgs)
      : IGF(IGF), Params(params), Args(args), ConsumedArgs(consumedArgs),
        NextConsumedArg(consumedArgs.begin()) {
    }

    void ignoreNext(unsigned count) {
      Params.claim(count);
      NextParamIndex += count;
    }

    /// Break apart tuple types and treat the fields separately and in order.
    void visitTupleType(TupleType *type) {
      for (auto &field : type->getFields()) {
        visit(CanType(field.getType()));
      }
    }

    /// Retain class pointers if necessary.
    void visitClassType(ClassType *type) {
      visitAnyClassType(type->getDecl());
    }
    void visitBoundGenericClassType(BoundGenericClassType *type) {
      visitAnyClassType(type->getDecl());
    }
    void visitAnyClassType(ClassDecl *theClass) {
      auto param = Params.claimUnmanagedNext();
      if (shouldRetainNextParam()) {
        Args.push_back(IGF.emitBestRetainCall(param, theClass));
      } else {
        Args.push_back(param);
      }
    }

    // Everything else gets copied to raise the component retain counts to +1.
    // FIXME: blocks could potentially be ns_consumed.
    void visitType(TypeBase *type) {
      const TypeInfo &ti = IGF.getFragileTypeInfo(type);
      if (requiresExternalByvalArgument(IGF.IGM, CanType(type))) {
        // If the argument was passed byval in the C calling convention,
        // reexplode it.
        llvm::Value *addrValue = Params.claimUnmanagedNext();
        llvm::Constant *alignConstant = ti.getStaticAlignment(IGF.IGM);
        unsigned align = alignConstant
          ? alignConstant->getUniqueInteger().getZExtValue()
          : 1;
        Address addr(addrValue, Alignment(align));
        Explosion loaded(IGF.CurExplosionLevel);
        ti.load(IGF, addr, loaded);
        unsigned width = ti.getExplosionSize(IGF.CurExplosionLevel);
        loaded.forward(IGF, width, Args);
        NextParamIndex += 1;
      } else {
        unsigned width = ti.getExplosionSize(IGF.CurExplosionLevel);
        Explosion copied(IGF.CurExplosionLevel);
        ti.copy(IGF, Params, copied);
        copied.forward(IGF, width, Args);
        NextParamIndex += width;
      }
    }

  private:
    /// Given that the next parameter is a retainable type, check
    /// whether its index is in the consumed-arguments set.
    /// If not, we need to retain it.  Regardless, advance the
    /// next-parameter-index counter.
    bool shouldRetainNextParam() {
      auto paramIndex = NextParamIndex++;

      // Note that the consumed-arguments set is just a sorted list.
      // If the remaining set is empty, we're done.
      if (NextConsumedArg == ConsumedArgs.end()) return true;
      if (*NextConsumedArg != paramIndex) return true;
      NextConsumedArg++;
      return false;
    }
  };
}

/// Produce a function pointer, suitable for invocation by
/// objc_msgSend, for the given method implementation.
///
/// Returns a value of type i8*.
static llvm::Constant *getObjCMethodPointer(IRGenModule &IGM,
                                            const Selector &selector,
                                            FuncDecl *method) {
  auto absCallee = AbstractCallee::forDirectGlobalFunction(IGM, method);

  // Find the swift method implementation.
  ExplosionKind explosionLevel;
  unsigned uncurryLevel;
  llvm::Function *swiftImpl;
  
  if (method->isGetterOrSetter()) {
    explosionLevel = absCallee.getBestExplosionLevel();
    uncurryLevel = absCallee.getMaxUncurryLevel();

    if (Decl *gd = method->getGetterDecl()) {
      
      swiftImpl = IGM.getAddrOfGetter(cast<ValueDecl>(gd),
                                      explosionLevel);
    } else if (Decl *sd = method->getSetterDecl()) {
      swiftImpl = IGM.getAddrOfSetter(cast<ValueDecl>(sd),
                                      explosionLevel);
    } else {
      llvm_unreachable("property accessor not getter or setter?!");
    }
  } else {
    auto fnRef = FunctionRef(method, absCallee.getBestExplosionLevel(),
                             absCallee.getMaxUncurryLevel());
    
    swiftImpl = IGM.getAddrOfFunction(fnRef, ExtraData::None);
    explosionLevel = fnRef.getExplosionLevel();
    uncurryLevel = fnRef.getUncurryLevel();
  }

  // Construct a callee and derive its ownership conventions.
  CanType origFormalType = method->getType()->getCanonicalType();
  ObjCMethodSignature sig(IGM, origFormalType, /*isSuper*/ false);
  auto callee = Callee::forMethod(origFormalType, sig.ResultType,
                                  ArrayRef<Substitution>(),
                                  swiftImpl,
                                  explosionLevel,
                                  uncurryLevel);
  auto &conventions = setOwnershipConventions(callee, method, selector);

  // Build the Objective-C function.
  llvm::Function *objcImpl;

  // As a special case, consider whether we really need a thunk.
  if (canUseSwiftFunctionAsObjCFunction(IGM, sig, conventions,
                                        origFormalType)) {
    objcImpl = swiftImpl;

  // Otherwise, build a function.
  } else {
    objcImpl = createSwiftAsObjCThunk(IGM, sig, swiftImpl);
    IRGenFunction IGF(IGM, origFormalType, ArrayRef<Pattern*>(),
                      explosionLevel, uncurryLevel,
                      objcImpl, Prologue::Bare);
    Explosion params = IGF.collectParameters();

    SmallVector<llvm::Value *, 16> args;

    // Remember the out-pointer.
    if (sig.IsIndirectReturn) {
      // TODO: remap return values?
      args.push_back(params.claimUnmanagedNext());
    }

    auto fnType = cast<AnyFunctionType>(origFormalType);
    auto selfType = CanType(fnType->getInput());
    fnType = cast<AnyFunctionType>(CanType(fnType->getResult()));
    auto formalArgType = CanType(fnType->getInput());

    SmallVector<unsigned, 16> consumedArgs;
    conventions.getConsumedArgs(IGM, callee, consumedArgs);

    TranslateParameters translate(IGF, params, args, consumedArgs);

    // Pull off and translate 'self'.
    translate.visit(selfType);
    params.getLastClaimed()->setName(method->isStatic() ? "This" : "this");

    // 'self' just got pushed on, but we need it to come later.
    auto self = args.back();
    args.pop_back();

    // Ignore '_cmd'.
    translate.ignoreNext(1);

    // Translate the formal parameters.
    translate.visit(formalArgType);
    assert(params.empty());

    // Put 'self' in its proper place.
    args.push_back(self);

    // Perform the call.
    // FIXME: other fn attributes?
    auto call = IGF.Builder.CreateCall(swiftImpl, args);
    if (sig.IsIndirectReturn) {
      call->addAttribute(1, llvm::Attribute::get(IGM.getLLVMContext(),
                                                 llvm::Attribute::StructRet));
    }

    if (call->getType()->isVoidTy()) {
      IGF.Builder.CreateRetVoid();
    } else {
      llvm::Value *result = call;
      if (conventions.isResultAutoreleased(IGM, callee)) {
        result = emitObjCAutoreleaseReturnValue(IGF, result);
      }
      IGF.Builder.CreateRet(result);
    }
  }

  return llvm::ConstantExpr::getBitCast(objcImpl, IGM.Int8PtrTy);
}

/// Emit the components of an Objective-C method descriptor: its selector,
/// type encoding, and IMP pointer.
void irgen::emitObjCMethodDescriptorParts(IRGenModule &IGM,
                                          FuncDecl *method,
                                          llvm::Constant *&selectorRef,
                                          llvm::Constant *&atEncoding,
                                          llvm::Constant *&impl) {
  Selector selector(method);
  
  /// The first element is the selector.
  selectorRef = IGM.getAddrOfObjCMethodName(selector.str());
  
  /// The second element is the type @encoding. Handle some simple cases, and
  /// leave the rest as null for now.
  AnyFunctionType *methodType = method->getType()->castTo<AnyFunctionType>();
  // Account for the 'this' pointer being curried.
  methodType = methodType->getResult()->castTo<AnyFunctionType>();
  
  if (methodType->getResult()->getClassOrBoundGenericClass() &&
      methodType->getInput()->isEqual(TupleType::getEmpty(IGM.Context)))
    atEncoding = IGM.getAddrOfGlobalString("@@:");
  else
    atEncoding = llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
  
  /// The third element is the method implementation pointer.
  impl = getObjCMethodPointer(IGM, selector, method);
  
}

/// Emit an Objective-C method descriptor for the given method.
/// struct method_t {
///   SEL name;
///   const char *types;
///   IMP imp;
/// };
llvm::Constant *irgen::emitObjCMethodDescriptor(IRGenModule &IGM,
                                                FuncDecl *method) {
  llvm::Constant *selectorRef, *atEncoding, *impl;
  emitObjCMethodDescriptorParts(IGM, method,
                                selectorRef, atEncoding, impl);
  
  llvm::Constant *fields[] = { selectorRef, atEncoding, impl };
  return llvm::ConstantStruct::getAnon(IGM.getLLVMContext(), fields);
}

bool irgen::requiresObjCMethodDescriptor(FuncDecl *method) {
  if (method->isObjC() ||
      method->getAttrs().isIBAction())
    return true;
  if (auto override = method->getOverriddenDecl())
    return requiresObjCMethodDescriptor(override);
  return false;
}

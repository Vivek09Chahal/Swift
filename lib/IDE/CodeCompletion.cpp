//===--- CodeCompletion.cpp - Code completion implementation --------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/IDE/CodeCompletion.h"
#include "CodeCompletionDiagnostics.h"
#include "CodeCompletionResultBuilder.h"
#include "ExprContextAnalysis.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Comment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/USRGeneration.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/LLVM.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Frontend/FrontendOptions.h"
#include "swift/IDE/CodeCompletionStringPrinter.h"
#include "swift/IDE/CompletionLookup.h"
#include "swift/IDE/CodeCompletionCache.h"
#include "swift/IDE/CodeCompletionResultPrinter.h"
#include "swift/IDE/Utils.h"
#include "swift/Parse/CodeCompletionCallbacks.h"
#include "swift/Sema/CodeCompletionTypeChecking.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/Strings.h"
#include "swift/Subsystems.h"
#include "swift/Syntax/SyntaxKind.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Comment.h"
#include "clang/AST/CommentVisitor.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/Module.h"
#include "clang/Index/USRGeneration.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <string>

using namespace swift;
using namespace ide;

std::string swift::ide::removeCodeCompletionTokens(
    StringRef Input, StringRef TokenName, unsigned *CompletionOffset) {
  assert(TokenName.size() >= 1);

  *CompletionOffset = ~0U;

  std::string CleanFile;
  CleanFile.reserve(Input.size());
  const std::string Token = std::string("#^") + TokenName.str() + "^#";

  for (const char *Ptr = Input.begin(), *End = Input.end();
       Ptr != End; ++Ptr) {
    const char C = *Ptr;
    if (C == '#' && Ptr <= End - Token.size() &&
        StringRef(Ptr, Token.size()) == Token) {
      Ptr += Token.size() - 1;
      *CompletionOffset = CleanFile.size();
      CleanFile += '\0';
      continue;
    }
    if (C == '#' && Ptr <= End - 2 && Ptr[1] == '^') {
      do {
        ++Ptr;
      } while (Ptr < End && *Ptr != '#');
      if (Ptr == End)
        break;
      continue;
    }
    CleanFile += C;
  }
  return CleanFile;
}

ContextFreeCodeCompletionResult *
ContextFreeCodeCompletionResult::createPatternOrBuiltInOperatorResult(
    llvm::BumpPtrAllocator &Allocator, CodeCompletionResultKind Kind,
    CodeCompletionString *CompletionString,
    CodeCompletionOperatorKind KnownOperatorKind,
    NullTerminatedStringRef BriefDocComment,
    CodeCompletionResultType ResultType,
    ContextFreeNotRecommendedReason NotRecommended,
    CodeCompletionDiagnosticSeverity DiagnosticSeverity,
    NullTerminatedStringRef DiagnosticMessage) {
  return new (Allocator) ContextFreeCodeCompletionResult(
      Kind, /*AssociatedKind=*/0, KnownOperatorKind,
      /*IsSystem=*/false, CompletionString, /*ModuleName=*/"", BriefDocComment,
      /*AssociatedUSRs=*/{}, ResultType, NotRecommended, DiagnosticSeverity,
      DiagnosticMessage,
      getCodeCompletionResultFilterName(CompletionString, Allocator));
}

ContextFreeCodeCompletionResult *
ContextFreeCodeCompletionResult::createKeywordResult(
    llvm::BumpPtrAllocator &Allocator, CodeCompletionKeywordKind Kind,
    CodeCompletionString *CompletionString,
    NullTerminatedStringRef BriefDocComment,
    CodeCompletionResultType ResultType) {
  return new (Allocator) ContextFreeCodeCompletionResult(
      CodeCompletionResultKind::Keyword, static_cast<uint8_t>(Kind),
      CodeCompletionOperatorKind::None, /*IsSystem=*/false, CompletionString,
      /*ModuleName=*/"", BriefDocComment,
      /*AssociatedUSRs=*/{}, ResultType, ContextFreeNotRecommendedReason::None,
      CodeCompletionDiagnosticSeverity::None, /*DiagnosticMessage=*/"",
      getCodeCompletionResultFilterName(CompletionString, Allocator));
}

ContextFreeCodeCompletionResult *
ContextFreeCodeCompletionResult::createLiteralResult(
    llvm::BumpPtrAllocator &Allocator, CodeCompletionLiteralKind LiteralKind,
    CodeCompletionString *CompletionString,
    CodeCompletionResultType ResultType) {
  return new (Allocator) ContextFreeCodeCompletionResult(
      CodeCompletionResultKind::Literal, static_cast<uint8_t>(LiteralKind),
      CodeCompletionOperatorKind::None,
      /*IsSystem=*/false, CompletionString, /*ModuleName=*/"",
      /*BriefDocComment=*/"",
      /*AssociatedUSRs=*/{}, ResultType, ContextFreeNotRecommendedReason::None,
      CodeCompletionDiagnosticSeverity::None, /*DiagnosticMessage=*/"",
      getCodeCompletionResultFilterName(CompletionString, Allocator));
}

ContextFreeCodeCompletionResult *
ContextFreeCodeCompletionResult::createDeclResult(
    llvm::BumpPtrAllocator &Allocator, CodeCompletionString *CompletionString,
    const Decl *AssociatedDecl, NullTerminatedStringRef ModuleName,
    NullTerminatedStringRef BriefDocComment,
    ArrayRef<NullTerminatedStringRef> AssociatedUSRs,
    CodeCompletionResultType ResultType,
    ContextFreeNotRecommendedReason NotRecommended,
    CodeCompletionDiagnosticSeverity DiagnosticSeverity,
    NullTerminatedStringRef DiagnosticMessage) {
  assert(AssociatedDecl && "should have a decl");
  return new (Allocator) ContextFreeCodeCompletionResult(
      CodeCompletionResultKind::Declaration,
      static_cast<uint8_t>(getCodeCompletionDeclKind(AssociatedDecl)),
      CodeCompletionOperatorKind::None, getDeclIsSystem(AssociatedDecl),
      CompletionString, ModuleName, BriefDocComment, AssociatedUSRs, ResultType,
      NotRecommended, DiagnosticSeverity, DiagnosticMessage,
      getCodeCompletionResultFilterName(CompletionString, Allocator));
}

CodeCompletionDeclKind
ContextFreeCodeCompletionResult::getCodeCompletionDeclKind(const Decl *D) {
  switch (D->getKind()) {
  case DeclKind::Import:
  case DeclKind::Extension:
  case DeclKind::PatternBinding:
  case DeclKind::EnumCase:
  case DeclKind::TopLevelCode:
  case DeclKind::IfConfig:
  case DeclKind::PoundDiagnostic:
  case DeclKind::MissingMember:
  case DeclKind::OpaqueType:
    llvm_unreachable("not expecting such a declaration result");
  case DeclKind::Module:
    return CodeCompletionDeclKind::Module;
  case DeclKind::TypeAlias:
    return CodeCompletionDeclKind::TypeAlias;
  case DeclKind::AssociatedType:
    return CodeCompletionDeclKind::AssociatedType;
  case DeclKind::GenericTypeParam:
    return CodeCompletionDeclKind::GenericTypeParam;
  case DeclKind::Enum:
    return CodeCompletionDeclKind::Enum;
  case DeclKind::Struct:
    return CodeCompletionDeclKind::Struct;
  case DeclKind::Class:
    if (cast<ClassDecl>(D)->isActor()) {
      return CodeCompletionDeclKind::Actor;
    } else {
      return CodeCompletionDeclKind::Class;
    }
  case DeclKind::Protocol:
    return CodeCompletionDeclKind::Protocol;
  case DeclKind::Var:
  case DeclKind::Param: {
    auto DC = D->getDeclContext();
    if (DC->isTypeContext()) {
      if (cast<VarDecl>(D)->isStatic())
        return CodeCompletionDeclKind::StaticVar;
      else
        return CodeCompletionDeclKind::InstanceVar;
    }
    if (DC->isLocalContext())
      return CodeCompletionDeclKind::LocalVar;
    return CodeCompletionDeclKind::GlobalVar;
  }
  case DeclKind::Constructor:
    return CodeCompletionDeclKind::Constructor;
  case DeclKind::Destructor:
    return CodeCompletionDeclKind::Destructor;
  case DeclKind::Accessor:
  case DeclKind::Func: {
    auto DC = D->getDeclContext();
    auto FD = cast<FuncDecl>(D);
    if (DC->isTypeContext()) {
      if (FD->isStatic())
        return CodeCompletionDeclKind::StaticMethod;
      return CodeCompletionDeclKind::InstanceMethod;
    }
    if (FD->isOperator()) {
      if (auto op = FD->getOperatorDecl()) {
        switch (op->getKind()) {
        case DeclKind::PrefixOperator:
          return CodeCompletionDeclKind::PrefixOperatorFunction;
        case DeclKind::PostfixOperator:
          return CodeCompletionDeclKind::PostfixOperatorFunction;
        case DeclKind::InfixOperator:
          return CodeCompletionDeclKind::InfixOperatorFunction;
        default:
          llvm_unreachable("unexpected operator kind");
        }
      } else {
        return CodeCompletionDeclKind::InfixOperatorFunction;
      }
    }
    return CodeCompletionDeclKind::FreeFunction;
  }
  case DeclKind::InfixOperator:
    return CodeCompletionDeclKind::InfixOperatorFunction;
  case DeclKind::PrefixOperator:
    return CodeCompletionDeclKind::PrefixOperatorFunction;
  case DeclKind::PostfixOperator:
    return CodeCompletionDeclKind::PostfixOperatorFunction;
  case DeclKind::PrecedenceGroup:
    return CodeCompletionDeclKind::PrecedenceGroup;
  case DeclKind::EnumElement:
    return CodeCompletionDeclKind::EnumElement;
  case DeclKind::Subscript:
    return CodeCompletionDeclKind::Subscript;
  }
  llvm_unreachable("invalid DeclKind");
}

bool ContextFreeCodeCompletionResult::getDeclIsSystem(const Decl *D) {
  return D->getModuleContext()->isSystemModule();
}

void CodeCompletionResult::printPrefix(raw_ostream &OS) const {
  llvm::SmallString<64> Prefix;
  switch (getKind()) {
  case CodeCompletionResultKind::Declaration:
    Prefix.append("Decl");
    switch (getAssociatedDeclKind()) {
    case CodeCompletionDeclKind::Class:
      Prefix.append("[Class]");
      break;
    case CodeCompletionDeclKind::Actor:
      Prefix.append("[Actor]");
      break;
    case CodeCompletionDeclKind::Struct:
      Prefix.append("[Struct]");
      break;
    case CodeCompletionDeclKind::Enum:
      Prefix.append("[Enum]");
      break;
    case CodeCompletionDeclKind::EnumElement:
      Prefix.append("[EnumElement]");
      break;
    case CodeCompletionDeclKind::Protocol:
      Prefix.append("[Protocol]");
      break;
    case CodeCompletionDeclKind::TypeAlias:
      Prefix.append("[TypeAlias]");
      break;
    case CodeCompletionDeclKind::AssociatedType:
      Prefix.append("[AssociatedType]");
      break;
    case CodeCompletionDeclKind::GenericTypeParam:
      Prefix.append("[GenericTypeParam]");
      break;
    case CodeCompletionDeclKind::Constructor:
      Prefix.append("[Constructor]");
      break;
    case CodeCompletionDeclKind::Destructor:
      Prefix.append("[Destructor]");
      break;
    case CodeCompletionDeclKind::Subscript:
      Prefix.append("[Subscript]");
      break;
    case CodeCompletionDeclKind::StaticMethod:
      Prefix.append("[StaticMethod]");
      break;
    case CodeCompletionDeclKind::InstanceMethod:
      Prefix.append("[InstanceMethod]");
      break;
    case CodeCompletionDeclKind::PrefixOperatorFunction:
      Prefix.append("[PrefixOperatorFunction]");
      break;
    case CodeCompletionDeclKind::PostfixOperatorFunction:
      Prefix.append("[PostfixOperatorFunction]");
      break;
    case CodeCompletionDeclKind::InfixOperatorFunction:
      Prefix.append("[InfixOperatorFunction]");
      break;
    case CodeCompletionDeclKind::FreeFunction:
      Prefix.append("[FreeFunction]");
      break;
    case CodeCompletionDeclKind::StaticVar:
      Prefix.append("[StaticVar]");
      break;
    case CodeCompletionDeclKind::InstanceVar:
      Prefix.append("[InstanceVar]");
      break;
    case CodeCompletionDeclKind::LocalVar:
      Prefix.append("[LocalVar]");
      break;
    case CodeCompletionDeclKind::GlobalVar:
      Prefix.append("[GlobalVar]");
      break;
    case CodeCompletionDeclKind::Module:
      Prefix.append("[Module]");
      break;
    case CodeCompletionDeclKind::PrecedenceGroup:
      Prefix.append("[PrecedenceGroup]");
      break;
    }
    break;
  case CodeCompletionResultKind::Keyword:
    Prefix.append("Keyword");
    switch (getKeywordKind()) {
    case CodeCompletionKeywordKind::None:
      break;
#define KEYWORD(X) case CodeCompletionKeywordKind::kw_##X: \
      Prefix.append("[" #X "]"); \
      break;
#define POUND_KEYWORD(X) case CodeCompletionKeywordKind::pound_##X: \
      Prefix.append("[#" #X "]"); \
      break;
#include "swift/Syntax/TokenKinds.def"
    }
    break;
  case CodeCompletionResultKind::Pattern:
    Prefix.append("Pattern");
    break;
  case CodeCompletionResultKind::Literal:
    Prefix.append("Literal");
    switch (getLiteralKind()) {
    case CodeCompletionLiteralKind::ArrayLiteral:
      Prefix.append("[Array]");
      break;
    case CodeCompletionLiteralKind::BooleanLiteral:
      Prefix.append("[Boolean]");
      break;
    case CodeCompletionLiteralKind::ColorLiteral:
      Prefix.append("[_Color]");
      break;
    case CodeCompletionLiteralKind::ImageLiteral:
      Prefix.append("[_Image]");
      break;
    case CodeCompletionLiteralKind::DictionaryLiteral:
      Prefix.append("[Dictionary]");
      break;
    case CodeCompletionLiteralKind::IntegerLiteral:
      Prefix.append("[Integer]");
      break;
    case CodeCompletionLiteralKind::NilLiteral:
      Prefix.append("[Nil]");
      break;
    case CodeCompletionLiteralKind::StringLiteral:
      Prefix.append("[String]");
      break;
    case CodeCompletionLiteralKind::Tuple:
      Prefix.append("[Tuple]");
      break;
    }
    break;
  case CodeCompletionResultKind::BuiltinOperator:
    Prefix.append("BuiltinOperator");
    break;
  }
  Prefix.append("/");
  switch (getSemanticContext()) {
  case SemanticContextKind::None:
    Prefix.append("None");
    break;
  case SemanticContextKind::Local:
    Prefix.append("Local");
    break;
  case SemanticContextKind::CurrentNominal:
    Prefix.append("CurrNominal");
    break;
  case SemanticContextKind::Super:
    Prefix.append("Super");
    break;
  case SemanticContextKind::OutsideNominal:
    Prefix.append("OutNominal");
    break;
  case SemanticContextKind::CurrentModule:
    Prefix.append("CurrModule");
    break;
  case SemanticContextKind::OtherModule:
    Prefix.append("OtherModule");
    if (!getModuleName().empty())
      Prefix.append((Twine("[") + StringRef(getModuleName()) + "]").str());
    break;
  }
  if (getFlair().toRaw()) {
    Prefix.append("/Flair[");
    bool isFirstFlair = true;
#define PRINT_FLAIR(KIND, NAME) \
    if (getFlair().contains(CodeCompletionFlairBit::KIND)) { \
      if (isFirstFlair) { isFirstFlair = false; } \
      else { Prefix.append(","); } \
      Prefix.append(NAME); \
    }
    PRINT_FLAIR(ExpressionSpecific, "ExprSpecific");
    PRINT_FLAIR(SuperChain, "SuperChain");
    PRINT_FLAIR(ArgumentLabels, "ArgLabels");
    PRINT_FLAIR(CommonKeywordAtCurrentPosition, "CommonKeyword")
    PRINT_FLAIR(RareKeywordAtCurrentPosition, "RareKeyword")
    PRINT_FLAIR(RareTypeAtCurrentPosition, "RareType")
    PRINT_FLAIR(ExpressionAtNonScriptOrMainFileScope, "ExprAtFileScope")
    Prefix.append("]");
  }
  if (isNotRecommended())
    Prefix.append("/NotRecommended");
  if (isSystem())
    Prefix.append("/IsSystem");
  if (NumBytesToErase != 0) {
    Prefix.append("/Erase[");
    Prefix.append(Twine(NumBytesToErase).str());
    Prefix.append("]");
  }
  switch (getExpectedTypeRelation()) {
  case CodeCompletionResultTypeRelation::Invalid:
    Prefix.append("/TypeRelation[Invalid]");
    break;
  case CodeCompletionResultTypeRelation::Identical:
    Prefix.append("/TypeRelation[Identical]");
    break;
  case CodeCompletionResultTypeRelation::Convertible:
    Prefix.append("/TypeRelation[Convertible]");
    break;
  case CodeCompletionResultTypeRelation::NotApplicable:
  case CodeCompletionResultTypeRelation::Unknown:
  case CodeCompletionResultTypeRelation::Unrelated:
    break;
  }

  Prefix.append(": ");
  while (Prefix.size() < 36) {
    Prefix.append(" ");
  }
  OS << Prefix;
}

void CodeCompletionResult::dump() const {
  printPrefix(llvm::errs());
  getCompletionString()->print(llvm::errs());
  llvm::errs() << "\n";
}

CodeCompletionResult *
CodeCompletionResult::withFlair(CodeCompletionFlair NewFlair,
                                CodeCompletionResultSink &Sink) const {
  return new (*Sink.Allocator) CodeCompletionResult(
      ContextFree, SemanticContext, NewFlair, NumBytesToErase, TypeDistance,
      NotRecommended, DiagnosticSeverity, DiagnosticMessage);
}

CodeCompletionResult *
CodeCompletionResult::withContextFreeResultSemanticContextAndFlair(
    const ContextFreeCodeCompletionResult &NewContextFree,
    SemanticContextKind NewSemanticContext, CodeCompletionFlair NewFlair,
    CodeCompletionResultSink &Sink) const {
  return new (*Sink.Allocator) CodeCompletionResult(
      NewContextFree, NewSemanticContext, NewFlair, NumBytesToErase,
      TypeDistance, NotRecommended, DiagnosticSeverity, DiagnosticMessage);
}

void CodeCompletionResultBuilder::withNestedGroup(
    CodeCompletionString::Chunk::ChunkKind Kind,
    llvm::function_ref<void()> body) {
  ++CurrentNestingLevel;
  addSimpleChunk(Kind);
  body();
  --CurrentNestingLevel;
}

void CodeCompletionResultBuilder::addChunkWithText(
    CodeCompletionString::Chunk::ChunkKind Kind, StringRef Text) {
  addChunkWithTextNoCopy(Kind, Text.copy(*Sink.Allocator));
}

void CodeCompletionResultBuilder::setAssociatedDecl(const Decl *D) {
  assert(Kind == CodeCompletionResultKind::Declaration);

  AssociatedDecl = D;

  if (auto *ClangD = D->getClangDecl())
    CurrentModule = ClangD->getImportedOwningModule();
  // FIXME: macros
  // FIXME: imported header module

  if (!CurrentModule) {
    ModuleDecl *MD = D->getModuleContext();

    // If this is an underscored cross-import overlay, map it to the underlying
    // module that declares it instead.
    if (ModuleDecl *Declaring = MD->getDeclaringModuleIfCrossImportOverlay())
      MD = Declaring;

    CurrentModule = MD;
  }

  if (D->getAttrs().getDeprecated(D->getASTContext()))
    setContextFreeNotRecommended(ContextFreeNotRecommendedReason::Deprecated);
  else if (D->getAttrs().getSoftDeprecated(D->getASTContext()))
    setContextFreeNotRecommended(
        ContextFreeNotRecommendedReason::SoftDeprecated);

  if (D->getClangNode()) {
    if (auto *ClangD = D->getClangDecl()) {
      const auto &ClangContext = ClangD->getASTContext();
      if (const clang::RawComment *RC =
          ClangContext.getRawCommentForAnyRedecl(ClangD)) {
        setBriefDocComment(RC->getBriefText(ClangContext));
      }
    }
  } else {
    setBriefDocComment(AssociatedDecl->getBriefComment());
  }
}

void CodeCompletionResultBuilder::addCallArgument(
    Identifier Name, Identifier LocalName, Type Ty, Type ContextTy,
    bool IsVarArg, bool IsInOut, bool IsIUO, bool IsAutoClosure,
    bool UseUnderscoreLabel, bool IsLabeledTrailingClosure, bool HasDefault) {
  ++CurrentNestingLevel;
  using ChunkKind = CodeCompletionString::Chunk::ChunkKind;

  addSimpleChunk(ChunkKind::CallArgumentBegin);

  if (shouldAnnotateResults()) {
    if (!Name.empty() || !LocalName.empty()) {
      llvm::SmallString<16> EscapedKeyword;

      if (!Name.empty()) {
        addChunkWithText(
            CodeCompletionString::Chunk::ChunkKind::CallArgumentName,
            escapeKeyword(Name.str(), false, EscapedKeyword));

        if (!LocalName.empty() && Name != LocalName) {
          addChunkWithTextNoCopy(ChunkKind::Text, " ");
          getLastChunk().setIsAnnotation();
          addChunkWithText(ChunkKind::CallArgumentInternalName,
              escapeKeyword(LocalName.str(), false, EscapedKeyword));
          getLastChunk().setIsAnnotation();
        }
      } else {
        assert(!LocalName.empty());
        addChunkWithTextNoCopy(ChunkKind::CallArgumentName, "_");
        getLastChunk().setIsAnnotation();
        addChunkWithTextNoCopy(ChunkKind::Text, " ");
        getLastChunk().setIsAnnotation();
        addChunkWithText(ChunkKind::CallArgumentInternalName,
            escapeKeyword(LocalName.str(), false, EscapedKeyword));
      }
      addChunkWithTextNoCopy(ChunkKind::CallArgumentColon, ": ");
    }
  } else {
    if (!Name.empty()) {
      llvm::SmallString<16> EscapedKeyword;
      addChunkWithText(
          CodeCompletionString::Chunk::ChunkKind::CallArgumentName,
          escapeKeyword(Name.str(), false, EscapedKeyword));
      addChunkWithTextNoCopy(
          CodeCompletionString::Chunk::ChunkKind::CallArgumentColon, ": ");
    } else if (UseUnderscoreLabel) {
      addChunkWithTextNoCopy(
          CodeCompletionString::Chunk::ChunkKind::CallArgumentName, "_");
      addChunkWithTextNoCopy(
          CodeCompletionString::Chunk::ChunkKind::CallArgumentColon, ": ");
    } else if (!LocalName.empty()) {
      // Use local (non-API) parameter name if we have nothing else.
      llvm::SmallString<16> EscapedKeyword;
      addChunkWithText(
          CodeCompletionString::Chunk::ChunkKind::CallArgumentInternalName,
            escapeKeyword(LocalName.str(), false, EscapedKeyword));
      addChunkWithTextNoCopy(
          CodeCompletionString::Chunk::ChunkKind::CallArgumentColon, ": ");
    }
  }

  // 'inout' arguments are printed specially.
  if (IsInOut) {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::Ampersand, "&");
    Ty = Ty->getInOutObjectType();
  }

  // If the parameter is of the type @autoclosure ()->output, then the
  // code completion should show the parameter of the output type
  // instead of the function type ()->output.
  if (IsAutoClosure) {
    // 'Ty' may be ErrorType.
    if (auto funcTy = Ty->getAs<FunctionType>())
      Ty = funcTy->getResult();
  }

  PrintOptions PO;
  PO.SkipAttributes = true;
  PO.PrintOptionalAsImplicitlyUnwrapped = IsIUO;
  PO.OpaqueReturnTypePrinting =
      PrintOptions::OpaqueReturnTypePrintingMode::WithoutOpaqueKeyword;
  if (ContextTy)
    PO.setBaseType(ContextTy);
  if (shouldAnnotateResults()) {
    withNestedGroup(ChunkKind::CallArgumentTypeBegin, [&]() {
      CodeCompletionStringPrinter printer(*this);
      auto TL = TypeLoc::withoutLoc(Ty);
      printer.printTypePre(TL);
      Ty->print(printer, PO);
      printer.printTypePost(TL);
    });
  } else {
    std::string TypeName = Ty->getString(PO);
    addChunkWithText(ChunkKind::CallArgumentType, TypeName);
  }

  if (HasDefault) {
    withNestedGroup(ChunkKind::CallArgumentDefaultBegin, []() {
      // Possibly add the actual value in the future
    });
  }

  // Look through optional types and type aliases to find out if we have
  // function type.
  Ty = Ty->lookThroughAllOptionalTypes();
  if (auto AFT = Ty->getAs<AnyFunctionType>()) {
    // If this is a closure type, add ChunkKind::CallArgumentClosureType or
    // ChunkKind::CallArgumentClosureExpr for labeled trailing closures.
    PrintOptions PO;
    PO.PrintFunctionRepresentationAttrs =
      PrintOptions::FunctionRepresentationMode::None;
    PO.SkipAttributes = true;
    PO.OpaqueReturnTypePrinting =
        PrintOptions::OpaqueReturnTypePrintingMode::WithoutOpaqueKeyword;
    PO.AlwaysTryPrintParameterLabels = true;
    if (ContextTy)
      PO.setBaseType(ContextTy);

    if (IsLabeledTrailingClosure) {
      // Expand the closure body.
      SmallString<32> buffer;
      llvm::raw_svector_ostream OS(buffer);

      bool firstParam = true;
      for (const auto &param : AFT->getParams()) {
        if (!firstParam)
          OS << ", ";
        firstParam = false;

        if (param.hasLabel()) {
          OS << param.getLabel();
        } else if (param.hasInternalLabel()) {
          OS << param.getInternalLabel();
        } else {
          OS << "<#";
          if (param.isInOut())
            OS << "inout ";
          OS << param.getPlainType()->getString(PO);
          if (param.isVariadic())
            OS << "...";
          OS << "#>";
        }
      }

      if (!firstParam)
        OS << " in";

      addChunkWithText(
         CodeCompletionString::Chunk::ChunkKind::CallArgumentClosureExpr,
         OS.str());
    } else {
      // Add the closure type.
      addChunkWithText(
          CodeCompletionString::Chunk::ChunkKind::CallArgumentClosureType,
          AFT->getString(PO));
    }
  }

  if (IsVarArg)
    addEllipsis();

  --CurrentNestingLevel;
}

void CodeCompletionResultBuilder::addTypeAnnotation(Type T, PrintOptions PO,
                                                    StringRef suffix) {
  T = T->getReferenceStorageReferent();

  // Replace '()' with 'Void'.
  if (T->isVoid())
    T = T->getASTContext().getVoidDecl()->getDeclaredInterfaceType();

  if (shouldAnnotateResults()) {
    withNestedGroup(CodeCompletionString::Chunk::ChunkKind::TypeAnnotationBegin,
                    [&]() {
                      CodeCompletionStringPrinter printer(*this);
                      auto TL = TypeLoc::withoutLoc(T);
                      printer.printTypePre(TL);
                      T->print(printer, PO);
                      printer.printTypePost(TL);
                      if (!suffix.empty())
                        printer.printText(suffix);
                    });
  } else {
    auto str = T.getString(PO);
    if (!suffix.empty())
      str += suffix.str();
    addTypeAnnotation(str);
  }
}

StringRef CodeCompletionContext::copyString(StringRef Str) {
  return Str.copy(*CurrentResults.Allocator);
}

bool shouldCopyAssociatedUSRForDecl(const ValueDecl *VD) {
  // Avoid trying to generate a USR for some declaration types.
  if (isa<AbstractTypeParamDecl>(VD) && !isa<AssociatedTypeDecl>(VD))
    return false;
  if (isa<ParamDecl>(VD))
    return false;
  if (isa<ModuleDecl>(VD))
    return false;
  if (VD->hasClangNode() && !VD->getClangDecl())
    return false;

  return true;
}

template <typename FnTy>
static void walkValueDeclAndOverriddenDecls(const Decl *D, const FnTy &Fn) {
  if (auto *VD = dyn_cast<ValueDecl>(D)) {
    Fn(VD);
    walkOverriddenDecls(VD, Fn);
  }
}

ArrayRef<NullTerminatedStringRef>
copyAssociatedUSRs(llvm::BumpPtrAllocator &Allocator, const Decl *D) {
  llvm::SmallVector<NullTerminatedStringRef, 4> USRs;
  walkValueDeclAndOverriddenDecls(D, [&](llvm::PointerUnion<const ValueDecl*,
                                                  const clang::NamedDecl*> OD) {
    llvm::SmallString<128> SS;
    bool Ignored = true;
    if (auto *OVD = OD.dyn_cast<const ValueDecl*>()) {
      if (shouldCopyAssociatedUSRForDecl(OVD)) {
        llvm::raw_svector_ostream OS(SS);
        Ignored = printValueDeclUSR(OVD, OS);
      }
    } else if (auto *OND = OD.dyn_cast<const clang::NamedDecl*>()) {
      Ignored = clang::index::generateUSRForDecl(OND, SS);
    }

    if (!Ignored)
      USRs.emplace_back(SS, Allocator);
  });

  if (!USRs.empty())
    return makeArrayRef(USRs).copy(Allocator);

  return {};
}

CodeCompletionResult::CodeCompletionResult(
    const ContextFreeCodeCompletionResult &ContextFree,
    SemanticContextKind SemanticContext, CodeCompletionFlair Flair,
    uint8_t NumBytesToErase, const ExpectedTypeContext *TypeContext,
    const DeclContext *DC, ContextualNotRecommendedReason NotRecommended,
    CodeCompletionDiagnosticSeverity DiagnosticSeverity,
    NullTerminatedStringRef DiagnosticMessage)
    : ContextFree(ContextFree), SemanticContext(SemanticContext),
      Flair(Flair.toRaw()), NotRecommended(NotRecommended),
      DiagnosticSeverity(DiagnosticSeverity),
      DiagnosticMessage(DiagnosticMessage), NumBytesToErase(NumBytesToErase),
      TypeDistance(
          ContextFree.getResultType().calculateTypeRelation(TypeContext, DC)) {}

CodeCompletionOperatorKind
ContextFreeCodeCompletionResult::getCodeCompletionOperatorKind(
    const CodeCompletionString *str) {
  StringRef name = str->getFirstTextChunk(/*includeLeadingPunctuation=*/true);
  using CCOK = CodeCompletionOperatorKind;
  using OpPair = std::pair<StringRef, CCOK>;

  // This list must be kept in alphabetical order.
  static OpPair ops[] = {
      std::make_pair("!", CCOK::Bang),
      std::make_pair("!=", CCOK::NotEq),
      std::make_pair("!==", CCOK::NotEqEq),
      std::make_pair("%", CCOK::Modulo),
      std::make_pair("%=", CCOK::ModuloEq),
      std::make_pair("&", CCOK::Amp),
      std::make_pair("&&", CCOK::AmpAmp),
      std::make_pair("&*", CCOK::AmpStar),
      std::make_pair("&+", CCOK::AmpPlus),
      std::make_pair("&-", CCOK::AmpMinus),
      std::make_pair("&=", CCOK::AmpEq),
      std::make_pair("(", CCOK::LParen),
      std::make_pair("*", CCOK::Star),
      std::make_pair("*=", CCOK::StarEq),
      std::make_pair("+", CCOK::Plus),
      std::make_pair("+=", CCOK::PlusEq),
      std::make_pair("-", CCOK::Minus),
      std::make_pair("-=", CCOK::MinusEq),
      std::make_pair(".", CCOK::Dot),
      std::make_pair("...", CCOK::DotDotDot),
      std::make_pair("..<", CCOK::DotDotLess),
      std::make_pair("/", CCOK::Slash),
      std::make_pair("/=", CCOK::SlashEq),
      std::make_pair("<", CCOK::Less),
      std::make_pair("<<", CCOK::LessLess),
      std::make_pair("<<=", CCOK::LessLessEq),
      std::make_pair("<=", CCOK::LessEq),
      std::make_pair("=", CCOK::Eq),
      std::make_pair("==", CCOK::EqEq),
      std::make_pair("===", CCOK::EqEqEq),
      std::make_pair(">", CCOK::Greater),
      std::make_pair(">=", CCOK::GreaterEq),
      std::make_pair(">>", CCOK::GreaterGreater),
      std::make_pair(">>=", CCOK::GreaterGreaterEq),
      std::make_pair("?.", CCOK::QuestionDot),
      std::make_pair("^", CCOK::Caret),
      std::make_pair("^=", CCOK::CaretEq),
      std::make_pair("|", CCOK::Pipe),
      std::make_pair("|=", CCOK::PipeEq),
      std::make_pair("||", CCOK::PipePipe),
      std::make_pair("~=", CCOK::TildeEq),
  };
  static auto opsSize = sizeof(ops) / sizeof(ops[0]);

  auto I = std::lower_bound(
      ops, &ops[opsSize], std::make_pair(name, CCOK::None),
      [](const OpPair &a, const OpPair &b) { return a.first < b.first; });

  if (I == &ops[opsSize] || I->first != name)
    return CCOK::Unknown;
  return I->second;
}

CodeCompletionResult *CodeCompletionResultBuilder::takeResult() {
  auto &Allocator = *Sink.Allocator;
  auto *CCS = CodeCompletionString::create(Allocator, Chunks);

  CodeCompletionDiagnosticSeverity ContextFreeDiagnosticSeverity =
      CodeCompletionDiagnosticSeverity::None;
  NullTerminatedStringRef ContextFreeDiagnosticMessage;
  if (ContextFreeNotRecReason != ContextFreeNotRecommendedReason::None) {
    // FIXME: We should generate the message lazily.
    if (const auto *VD = dyn_cast<ValueDecl>(AssociatedDecl)) {
      CodeCompletionDiagnosticSeverity severity;
      SmallString<256> message;
      llvm::raw_svector_ostream messageOS(message);
      if (!getContextFreeCompletionDiagnostics(ContextFreeNotRecReason, VD,
                                               severity, messageOS)) {
        ContextFreeDiagnosticSeverity = severity;
        ContextFreeDiagnosticMessage =
            NullTerminatedStringRef(message, Allocator);
      }
    }
  }

  /// This variable should be initialized by all the switch cases below.
  ContextFreeCodeCompletionResult *ContextFreeResult = nullptr;

  switch (Kind) {
  case CodeCompletionResultKind::Declaration: {
    NullTerminatedStringRef ModuleName;
    if (CurrentModule) {
      if (Sink.LastModule.first == CurrentModule.getOpaqueValue()) {
        ModuleName = Sink.LastModule.second;
      } else {
        if (auto *C = CurrentModule.dyn_cast<const clang::Module *>()) {
          ModuleName =
              NullTerminatedStringRef(C->getFullModuleName(), Allocator);
        } else {
          ModuleName = NullTerminatedStringRef(
              CurrentModule.get<const swift::ModuleDecl *>()->getName().str(),
              Allocator);
        }
        Sink.LastModule.first = CurrentModule.getOpaqueValue();
        Sink.LastModule.second = ModuleName;
      }
    }

    ContextFreeResult = ContextFreeCodeCompletionResult::createDeclResult(
        Allocator, CCS, AssociatedDecl, ModuleName,
        NullTerminatedStringRef(BriefDocComment, Allocator),
        copyAssociatedUSRs(Allocator, AssociatedDecl), ResultType,
        ContextFreeNotRecReason, ContextFreeDiagnosticSeverity,
        ContextFreeDiagnosticMessage);
    break;
  }

  case CodeCompletionResultKind::Keyword:
    ContextFreeResult = ContextFreeCodeCompletionResult::createKeywordResult(
        Allocator, KeywordKind, CCS,
        NullTerminatedStringRef(BriefDocComment, Allocator), ResultType);
    break;
  case CodeCompletionResultKind::BuiltinOperator:
  case CodeCompletionResultKind::Pattern:
    ContextFreeResult =
        ContextFreeCodeCompletionResult::createPatternOrBuiltInOperatorResult(
            Allocator, Kind, CCS, CodeCompletionOperatorKind::None,
            NullTerminatedStringRef(BriefDocComment, Allocator), ResultType,
            ContextFreeNotRecReason, ContextFreeDiagnosticSeverity,
            ContextFreeDiagnosticMessage);
    break;
  case CodeCompletionResultKind::Literal:
    assert(LiteralKind.hasValue());
    ContextFreeResult = ContextFreeCodeCompletionResult::createLiteralResult(
        Allocator, *LiteralKind, CCS, ResultType);
    break;
  }

  CodeCompletionDiagnosticSeverity ContextualDiagnosticSeverity =
      CodeCompletionDiagnosticSeverity::None;
  NullTerminatedStringRef ContextualDiagnosticMessage;
  if (ContextualNotRecReason != ContextualNotRecommendedReason::None) {
    // FIXME: We should generate the message lazily.
    if (const auto *VD = dyn_cast<ValueDecl>(AssociatedDecl)) {
      CodeCompletionDiagnosticSeverity severity;
      SmallString<256> message;
      llvm::raw_svector_ostream messageOS(message);
      if (!getContextualCompletionDiagnostics(ContextualNotRecReason, VD,
                                              severity, messageOS)) {
        ContextualDiagnosticSeverity = severity;
        ContextualDiagnosticMessage =
            NullTerminatedStringRef(message, Allocator);
      }
    }
  }

  assert(ContextFreeResult != nullptr &&
         "ContextFreeResult should have been constructed by the switch above");
  CodeCompletionResult *result = new (Allocator) CodeCompletionResult(
      *ContextFreeResult, SemanticContext, Flair, NumBytesToErase, TypeContext,
      DC, ContextualNotRecReason, ContextualDiagnosticSeverity,
      ContextualDiagnosticMessage);
  return result;
}

void CodeCompletionResultBuilder::finishResult() {
  if (!Cancelled)
    Sink.Results.push_back(takeResult());
}

std::vector<CodeCompletionResult *>
CodeCompletionContext::sortCompletionResults(
    ArrayRef<CodeCompletionResult *> Results) {
  std::vector<CodeCompletionResult *> SortedResults(Results.begin(),
                                                    Results.end());

  std::sort(SortedResults.begin(), SortedResults.end(),
            [](const auto &LHS, const auto &RHS) {
              int Result = StringRef(LHS->getFilterName())
                               .compare_insensitive(RHS->getFilterName());
              // If the case insensitive comparison is equal, then secondary
              // sort order should be case sensitive.
              if (Result == 0)
                Result = LHS->getFilterName().compare(RHS->getFilterName());
              return Result < 0;
            });

  return SortedResults;
}

namespace {

class CodeCompletionCallbacksImpl : public CodeCompletionCallbacks {
  CodeCompletionContext &CompletionContext;
  CodeCompletionConsumer &Consumer;
  CodeCompletionExpr *CodeCompleteTokenExpr = nullptr;
  CompletionKind Kind = CompletionKind::None;
  Expr *ParsedExpr = nullptr;
  SourceLoc DotLoc;
  TypeLoc ParsedTypeLoc;
  DeclContext *CurDeclContext = nullptr;
  DeclAttrKind AttrKind;

  /// In situations when \c SyntaxKind hints or determines
  /// completions, i.e. a precedence group attribute, this
  /// can be set and used to control the code completion scenario.
  SyntaxKind SyntxKind;

  int AttrParamIndex;
  bool IsInSil = false;
  bool HasSpace = false;
  bool ShouldCompleteCallPatternAfterParen = true;
  bool PreferFunctionReferencesToCalls = false;
  bool AttTargetIsIndependent = false;
  bool IsAtStartOfLine = false;
  Optional<DeclKind> AttTargetDK;
  Optional<StmtKind> ParentStmtKind;

  SmallVector<StringRef, 3> ParsedKeywords;
  SourceLoc introducerLoc;

  std::vector<std::pair<std::string, bool>> SubModuleNameVisibilityPairs;

  void addSuperKeyword(CodeCompletionResultSink &Sink) {
    auto *DC = CurDeclContext->getInnermostTypeContext();
    if (!DC)
      return;
    auto *CD = DC->getSelfClassDecl();
    if (!CD)
      return;
    Type ST = CD->getSuperclass();
    if (ST.isNull() || ST->is<ErrorType>())
      return;

    CodeCompletionResultBuilder Builder(Sink, CodeCompletionResultKind::Keyword,
                                        SemanticContextKind::CurrentNominal,
                                        {});
    if (auto *AFD = dyn_cast<AbstractFunctionDecl>(CurDeclContext)) {
      if (AFD->getOverriddenDecl() != nullptr) {
        Builder.addFlair(CodeCompletionFlairBit::CommonKeywordAtCurrentPosition);
      }
    }

    Builder.setKeywordKind(CodeCompletionKeywordKind::kw_super);
    Builder.addKeyword("super");
    Builder.addTypeAnnotation(ST, PrintOptions());
  }

  Optional<std::pair<Type, ConcreteDeclRef>> typeCheckParsedExpr() {
    assert(ParsedExpr && "should have an expression");

    // Figure out the kind of type-check we'll be performing.
    auto CheckKind = CompletionTypeCheckKind::Normal;
    if (Kind == CompletionKind::KeyPathExprObjC)
      CheckKind = CompletionTypeCheckKind::KeyPath;

    // If we've already successfully type-checked the expression for some
    // reason, just return the type.
    // FIXME: if it's ErrorType but we've already typechecked we shouldn't
    // typecheck again. rdar://21466394
    if (CheckKind == CompletionTypeCheckKind::Normal &&
        ParsedExpr->getType() && !ParsedExpr->getType()->is<ErrorType>()) {
      return getReferencedDecl(ParsedExpr);
    }

    ConcreteDeclRef ReferencedDecl = nullptr;
    Expr *ModifiedExpr = ParsedExpr;
    if (auto T = getTypeOfCompletionContextExpr(P.Context, CurDeclContext,
                                                CheckKind, ModifiedExpr,
                                                ReferencedDecl)) {
      // FIXME: even though we don't apply the solution, the type checker may
      // modify the original expression. We should understand what effect that
      // may have on code completion.
      ParsedExpr = ModifiedExpr;

      return std::make_pair(*T, ReferencedDecl);
    }
    return None;
  }

  /// \returns true on success, false on failure.
  bool typecheckParsedType() {
    assert(ParsedTypeLoc.getTypeRepr() && "should have a TypeRepr");
    if (ParsedTypeLoc.wasValidated() && !ParsedTypeLoc.isError()) {
      return true;
    }

    const auto ty = swift::performTypeResolution(
        ParsedTypeLoc.getTypeRepr(), P.Context,
        /*isSILMode=*/false,
        /*isSILType=*/false,
        CurDeclContext->getGenericEnvironmentOfContext(),
        /*GenericParams=*/nullptr,
        CurDeclContext,
        /*ProduceDiagnostics=*/false);
    if (!ty->hasError()) {
      ParsedTypeLoc.setType(CurDeclContext->mapTypeIntoContext(ty));
      return true;
    }

    ParsedTypeLoc.setType(ty);

    // It doesn't type check as a type, so see if it's a qualifying module name.
    if (auto *ITR = dyn_cast<IdentTypeRepr>(ParsedTypeLoc.getTypeRepr())) {
      const auto &componentRange = ITR->getComponentRange();
      // If it has more than one component, it can't be a module name.
      if (std::distance(componentRange.begin(), componentRange.end()) != 1)
        return false;

      const auto &component = componentRange.front();
      ImportPath::Module::Builder builder(
          component->getNameRef().getBaseIdentifier(),
          component->getLoc());

      if (auto Module = Context.getLoadedModule(builder.get()))
        ParsedTypeLoc.setType(ModuleType::get(Module));
      return true;
    }
    return false;
  }

public:
  CodeCompletionCallbacksImpl(Parser &P,
                              CodeCompletionContext &CompletionContext,
                              CodeCompletionConsumer &Consumer)
      : CodeCompletionCallbacks(P), CompletionContext(CompletionContext),
        Consumer(Consumer) {
  }

  void setAttrTargetDeclKind(Optional<DeclKind> DK) override {
    if (DK == DeclKind::PatternBinding)
      DK = DeclKind::Var;
    else if (DK == DeclKind::Param)
      // For params, consider the attribute is always for the decl.
      AttTargetIsIndependent = false;

    if (!AttTargetIsIndependent)
      AttTargetDK = DK;
  }

  void completeDotExpr(CodeCompletionExpr *E, SourceLoc DotLoc) override;
  void completeStmtOrExpr(CodeCompletionExpr *E) override;
  void completePostfixExprBeginning(CodeCompletionExpr *E) override;
  void completeForEachSequenceBeginning(CodeCompletionExpr *E) override;
  void completePostfixExpr(Expr *E, bool hasSpace) override;
  void completePostfixExprParen(Expr *E, Expr *CodeCompletionE) override;
  void completeExprKeyPath(KeyPathExpr *KPE, SourceLoc DotLoc) override;

  void completeTypeDeclResultBeginning() override;
  void completeTypeSimpleBeginning() override;
  void completeTypeIdentifierWithDot(IdentTypeRepr *ITR) override;
  void completeTypeIdentifierWithoutDot(IdentTypeRepr *ITR) override;

  void completeCaseStmtKeyword() override;
  void completeCaseStmtBeginning(CodeCompletionExpr *E) override;
  void completeDeclAttrBeginning(bool Sil, bool isIndependent) override;
  void completeDeclAttrParam(DeclAttrKind DK, int Index) override;
  void completeEffectsSpecifier(bool hasAsync, bool hasThrows) override;
  void completeInPrecedenceGroup(SyntaxKind SK) override;
  void completeNominalMemberBeginning(
      SmallVectorImpl<StringRef> &Keywords, SourceLoc introducerLoc) override;
  void completeAccessorBeginning(CodeCompletionExpr *E) override;

  void completePoundAvailablePlatform() override;
  void completeImportDecl(ImportPath::Builder &Path) override;
  void completeUnresolvedMember(CodeCompletionExpr *E,
                                SourceLoc DotLoc) override;
  void completeCallArg(CodeCompletionExpr *E, bool isFirst) override;
  void completeLabeledTrailingClosure(CodeCompletionExpr *E,
                                      bool isAtStartOfLine) override;

  bool canPerformCompleteLabeledTrailingClosure() const override {
    return true;
  }

  void completeReturnStmt(CodeCompletionExpr *E) override;
  void completeYieldStmt(CodeCompletionExpr *E,
                         Optional<unsigned> yieldIndex) override;
  void completeAfterPoundExpr(CodeCompletionExpr *E,
                              Optional<StmtKind> ParentKind) override;
  void completeAfterPoundDirective() override;
  void completePlatformCondition() override;
  void completeGenericRequirement() override;
  void completeAfterIfStmt(bool hasElse) override;
  void completeStmtLabel(StmtKind ParentKind) override;
  void completeForEachPatternBeginning(bool hasTry, bool hasAwait) override;
  void completeTypeAttrBeginning() override;

  void doneParsing() override;

private:
  void addKeywords(CodeCompletionResultSink &Sink, bool MaybeFuncBody);
  bool trySolverCompletion(bool MaybeFuncBody);
};
} // end anonymous namespace

namespace {

class CompletionOverrideLookup : public swift::VisibleDeclConsumer {
  CodeCompletionResultSink &Sink;
  ASTContext &Ctx;
  const DeclContext *CurrDeclContext;
  SmallVectorImpl<StringRef> &ParsedKeywords;
  SourceLoc introducerLoc;

  bool hasFuncIntroducer = false;
  bool hasVarIntroducer = false;
  bool hasTypealiasIntroducer = false;
  bool hasInitializerModifier = false;
  bool hasAccessModifier = false;
  bool hasOverride = false;
  bool hasOverridabilityModifier = false;
  bool hasStaticOrClass = false;

public:
  CompletionOverrideLookup(CodeCompletionResultSink &Sink, ASTContext &Ctx,
                           const DeclContext *CurrDeclContext,
                           SmallVectorImpl<StringRef> &ParsedKeywords,
                           SourceLoc introducerLoc)
      : Sink(Sink), Ctx(Ctx), CurrDeclContext(CurrDeclContext),
        ParsedKeywords(ParsedKeywords), introducerLoc(introducerLoc) {
    hasFuncIntroducer = isKeywordSpecified("func");
    hasVarIntroducer = isKeywordSpecified("var") ||
                       isKeywordSpecified("let");
    hasTypealiasIntroducer = isKeywordSpecified("typealias");
    hasInitializerModifier = isKeywordSpecified("required") ||
                             isKeywordSpecified("convenience");
    hasAccessModifier = isKeywordSpecified("private") ||
                        isKeywordSpecified("fileprivate") ||
                        isKeywordSpecified("internal") ||
                        isKeywordSpecified("public") ||
                        isKeywordSpecified("open");
    hasOverride = isKeywordSpecified("override");
    hasOverridabilityModifier = isKeywordSpecified("final") ||
                                isKeywordSpecified("open");
    hasStaticOrClass = isKeywordSpecified(getTokenText(tok::kw_class)) ||
                       isKeywordSpecified(getTokenText(tok::kw_static));
  }

  bool isKeywordSpecified(StringRef Word) {
    return std::find(ParsedKeywords.begin(), ParsedKeywords.end(), Word)
      != ParsedKeywords.end();
  }

  bool missingOverride(DeclVisibilityKind Reason) {
    return !hasOverride && Reason == DeclVisibilityKind::MemberOfSuper &&
           !CurrDeclContext->getSelfProtocolDecl();
  }

  /// Add an access modifier (i.e. `public`) to \p Builder is necessary.
  /// Returns \c true if the modifier is actually added, \c false otherwise.
  bool addAccessControl(const ValueDecl *VD,
                        CodeCompletionResultBuilder &Builder) {
    auto CurrentNominal = CurrDeclContext->getSelfNominalTypeDecl();
    assert(CurrentNominal);

    auto AccessOfContext = CurrentNominal->getFormalAccess();
    if (AccessOfContext < AccessLevel::Public)
      return false;

    auto Access = VD->getFormalAccess();
    // Use the greater access between the protocol requirement and the witness.
    // In case of:
    //
    //   public protocol P { func foo() }
    //   public class B { func foo() {} }
    //   public class C: B, P {
    //     <complete>
    //   }
    //
    // 'VD' is 'B.foo()' which is implicitly 'internal'. But as the overriding
    // declaration, the user needs to write both 'public' and 'override':
    //
    //   public class C: B {
    //     public override func foo() {}
    //   }
    if (Access < AccessLevel::Public &&
        !isa<ProtocolDecl>(VD->getDeclContext())) {
      for (auto Conformance : CurrentNominal->getAllConformances()) {
        Conformance->getRootConformance()->forEachValueWitness(
            [&](ValueDecl *req, Witness witness) {
              if (witness.getDecl() == VD)
                Access = std::max(
                    Access, Conformance->getProtocol()->getFormalAccess());
            });
      }
    }

    Access = std::min(Access, AccessOfContext);
    // Only emit 'public', not needed otherwise.
    if (Access < AccessLevel::Public)
      return false;

    Builder.addAccessControlKeyword(Access);
    return true;
  }

  /// Return type if the result type if \p VD should be represented as opaque
  /// result type.
  Type getOpaqueResultType(const ValueDecl *VD, DeclVisibilityKind Reason,
                           DynamicLookupInfo dynamicLookupInfo) {
    if (Reason !=
        DeclVisibilityKind::MemberOfProtocolConformedToByCurrentNominal)
      return nullptr;

    auto currTy = CurrDeclContext->getDeclaredTypeInContext();
    if (!currTy)
      return nullptr;

    Type ResultT;
    if (auto *FD = dyn_cast<FuncDecl>(VD)) {
      if (FD->getGenericParams()) {
        // Generic function cannot have opaque result type.
        return nullptr;
      }
      ResultT = FD->getResultInterfaceType();
    } else if (auto *SD = dyn_cast<SubscriptDecl>(VD)) {
      if (SD->getGenericParams()) {
        // Generic subscript cannot have opaque result type.
        return nullptr;
      }
      ResultT = SD->getElementInterfaceType();
    } else if (auto *VarD = dyn_cast<VarDecl>(VD)) {
      ResultT = VarD->getInterfaceType();
    } else {
      return nullptr;
    }

    if (!ResultT->is<DependentMemberType>() ||
        !ResultT->castTo<DependentMemberType>()->getAssocType())
      // The result is not a valid associatedtype.
      return nullptr;

    // Try substitution to see if the associated type is resolved to concrete
    // type.
    auto substMap = currTy->getMemberSubstitutionMap(
        CurrDeclContext->getParentModule(), VD);
    if (!ResultT.subst(substMap)->is<DependentMemberType>())
      // If resolved print it.
      return nullptr;

    auto genericSig = VD->getDeclContext()->getGenericSignatureOfContext();

    if (genericSig->isConcreteType(ResultT))
      // If it has same type requrement, we will emit the concrete type.
      return nullptr;

    // Collect requirements on the associatedtype.
    SmallVector<Type, 2> opaqueTypes;
    bool hasExplicitAnyObject = false;
    if (auto superTy = genericSig->getSuperclassBound(ResultT))
      opaqueTypes.push_back(superTy);
    for (const auto proto : genericSig->getRequiredProtocols(ResultT))
      opaqueTypes.push_back(proto->getDeclaredInterfaceType());
    if (auto layout = genericSig->getLayoutConstraint(ResultT))
      hasExplicitAnyObject = layout->isClass();

    if (!hasExplicitAnyObject) {
      if (opaqueTypes.empty())
        return nullptr;
      if (opaqueTypes.size() == 1)
        return opaqueTypes.front();
    }
    return ProtocolCompositionType::get(
        VD->getASTContext(), opaqueTypes, hasExplicitAnyObject);
  }

  void addValueOverride(const ValueDecl *VD, DeclVisibilityKind Reason,
                        DynamicLookupInfo dynamicLookupInfo,
                        CodeCompletionResultBuilder &Builder,
                        bool hasDeclIntroducer) {
    Type opaqueResultType = getOpaqueResultType(VD, Reason, dynamicLookupInfo);

    class DeclPrinter : public CodeCompletionStringPrinter {
      Type OpaqueBaseTy;

    public:
      DeclPrinter(CodeCompletionResultBuilder &Builder, Type OpaqueBaseTy)
          : CodeCompletionStringPrinter(Builder), OpaqueBaseTy(OpaqueBaseTy) { }

      // As for FuncDecl, SubscriptDecl, and VarDecl, substitute the result type
      // with 'OpaqueBaseTy' if specified.
      void printDeclResultTypePre(ValueDecl *VD, TypeLoc &TL) override {
        if (!OpaqueBaseTy.isNull()) {
          setNextChunkKind(ChunkKind::Keyword);
          printText("some ");
          setNextChunkKind(ChunkKind::Text);
          TL = TypeLoc::withoutLoc(OpaqueBaseTy);
        }
        CodeCompletionStringPrinter::printDeclResultTypePre(VD, TL);
      }
    };

    DeclPrinter Printer(Builder, opaqueResultType);
    Printer.startPreamble();

    bool modifierAdded = false;

    // 'public' if needed.
    modifierAdded |= !hasAccessModifier && addAccessControl(VD, Builder);

    // 'override' if needed
    if (missingOverride(Reason)) {
      Builder.addOverrideKeyword();
      modifierAdded |= true;
    }

    // Erase existing introducer (e.g. 'func') if any modifiers are added.
    if (hasDeclIntroducer && modifierAdded) {
      auto dist = Ctx.SourceMgr.getByteDistance(
          introducerLoc, Ctx.SourceMgr.getCodeCompletionLoc());
      if (dist <= CodeCompletionResult::MaxNumBytesToErase) {
        Builder.setNumBytesToErase(dist);
        hasDeclIntroducer = false;
      }
    }

    PrintOptions PO;
    if (auto transformType = CurrDeclContext->getDeclaredTypeInContext())
      PO.setBaseType(transformType);
    PO.PrintPropertyAccessors = false;
    PO.PrintSubscriptAccessors = false;

    PO.SkipUnderscoredKeywords = true;
    PO.PrintImplicitAttrs = false;
    PO.ExclusiveAttrList.push_back(TAK_escaping);
    PO.ExclusiveAttrList.push_back(TAK_autoclosure);
    // Print certain modifiers only when the introducer is not written.
    // Otherwise, the user can add it after the completion.
    if (!hasDeclIntroducer) {
      PO.ExclusiveAttrList.push_back(DAK_Nonisolated);
    }

    PO.PrintAccess = false;
    PO.PrintOverrideKeyword = false;
    PO.PrintSelfAccessKindKeyword = false;

    PO.PrintStaticKeyword = !hasStaticOrClass && !hasDeclIntroducer;
    PO.SkipIntroducerKeywords = hasDeclIntroducer;
    VD->print(Printer, PO);
  }

  void addMethodOverride(const FuncDecl *FD, DeclVisibilityKind Reason,
                         DynamicLookupInfo dynamicLookupInfo) {
    CodeCompletionResultBuilder Builder(Sink,
                                        CodeCompletionResultKind::Declaration,
                                        SemanticContextKind::Super, {});
    Builder.setResultTypeNotApplicable();
    Builder.setAssociatedDecl(FD);
    addValueOverride(FD, Reason, dynamicLookupInfo, Builder, hasFuncIntroducer);
    Builder.addBraceStmtWithCursor();
  }

  void addVarOverride(const VarDecl *VD, DeclVisibilityKind Reason,
                      DynamicLookupInfo dynamicLookupInfo) {
    // Overrides cannot use 'let', but if the 'override' keyword is specified
    // then the intention is clear, so provide the results anyway.  The compiler
    // can then provide an error telling you to use 'var' instead.
    // If we don't need override then it's a protocol requirement, so show it.
    if (missingOverride(Reason) && hasVarIntroducer &&
        isKeywordSpecified("let"))
      return;

    CodeCompletionResultBuilder Builder(Sink,
                                        CodeCompletionResultKind::Declaration,
                                        SemanticContextKind::Super, {});
    Builder.setAssociatedDecl(VD);
    addValueOverride(VD, Reason, dynamicLookupInfo, Builder, hasVarIntroducer);
  }

  void addSubscriptOverride(const SubscriptDecl *SD, DeclVisibilityKind Reason,
                            DynamicLookupInfo dynamicLookupInfo) {
    CodeCompletionResultBuilder Builder(Sink,
                                        CodeCompletionResultKind::Declaration,
                                        SemanticContextKind::Super, {});
    Builder.setResultTypeNotApplicable();
    Builder.setAssociatedDecl(SD);
    addValueOverride(SD, Reason, dynamicLookupInfo, Builder, false);
    Builder.addBraceStmtWithCursor();
  }

  void addTypeAlias(const AssociatedTypeDecl *ATD, DeclVisibilityKind Reason,
                    DynamicLookupInfo dynamicLookupInfo) {
    CodeCompletionResultBuilder Builder(Sink,
                                        CodeCompletionResultKind::Declaration,
                                        SemanticContextKind::Super, {});
    Builder.setResultTypeNotApplicable();
    Builder.setAssociatedDecl(ATD);
    if (!hasTypealiasIntroducer && !hasAccessModifier)
      (void)addAccessControl(ATD, Builder);
    if (!hasTypealiasIntroducer)
      Builder.addDeclIntroducer("typealias ");
    Builder.addBaseName(ATD->getName().str());
    Builder.addTextChunk(" = ");
    Builder.addSimpleNamedParameter("Type");
  }

  void addConstructor(const ConstructorDecl *CD, DeclVisibilityKind Reason,
                      DynamicLookupInfo dynamicLookupInfo) {
    CodeCompletionResultBuilder Builder(Sink,
                                        CodeCompletionResultKind::Declaration,
                                        SemanticContextKind::Super, {});
    Builder.setResultTypeNotApplicable();
    Builder.setAssociatedDecl(CD);

    CodeCompletionStringPrinter printer(Builder);
    printer.startPreamble();

    if (!hasAccessModifier)
      (void)addAccessControl(CD, Builder);

    if (missingOverride(Reason) && CD->isDesignatedInit() && !CD->isRequired())
      Builder.addOverrideKeyword();

    // Emit 'required' if we're in class context, 'required' is not specified,
    // and 1) this is a protocol conformance and the class is not final, or 2)
    // this is subclass and the initializer is marked as required.
    bool needRequired = false;
    auto C = CurrDeclContext->getSelfClassDecl();
    if (C && !isKeywordSpecified("required")) {
      switch (Reason) {
      case DeclVisibilityKind::MemberOfProtocolConformedToByCurrentNominal:
      case DeclVisibilityKind::MemberOfProtocolDerivedByCurrentNominal:
        if (!C->isSemanticallyFinal())
          needRequired = true;
        break;
      case DeclVisibilityKind::MemberOfSuper:
        if (CD->isRequired())
          needRequired = true;
        break;
      default: break;
      }
    }
    if (needRequired)
      Builder.addRequiredKeyword();

    {
      PrintOptions Options;
      if (auto transformType = CurrDeclContext->getDeclaredTypeInContext())
        Options.setBaseType(transformType);
      Options.PrintImplicitAttrs = false;
      Options.SkipAttributes = true;
      CD->print(printer, Options);
    }
    printer.flush();

    Builder.addBraceStmtWithCursor();
  }

  // Implement swift::VisibleDeclConsumer.
  void foundDecl(ValueDecl *D, DeclVisibilityKind Reason,
                 DynamicLookupInfo dynamicLookupInfo) override {
    if (Reason == DeclVisibilityKind::MemberOfCurrentNominal)
      return;

    if (D->shouldHideFromEditor())
      return;

    if (D->isSemanticallyFinal())
      return;

    bool hasIntroducer = hasFuncIntroducer ||
                         hasVarIntroducer ||
                         hasTypealiasIntroducer;

    if (hasStaticOrClass && !D->isStatic())
      return;

    // As per the current convention, only instance members are
    // suggested if an introducer is not accompanied by a 'static' or
    // 'class' modifier.
    if (hasIntroducer && !hasStaticOrClass && D->isStatic())
      return;

    if (auto *FD = dyn_cast<FuncDecl>(D)) {
      // We cannot override operators as members.
      if (FD->isBinaryOperator() || FD->isUnaryOperator())
        return;

      // We cannot override individual accessors.
      if (isa<AccessorDecl>(FD))
        return;

      if (hasFuncIntroducer || (!hasIntroducer && !hasInitializerModifier))
        addMethodOverride(FD, Reason, dynamicLookupInfo);
      return;
    }

    if (auto *VD = dyn_cast<VarDecl>(D)) {
      if (hasVarIntroducer || (!hasIntroducer && !hasInitializerModifier))
        addVarOverride(VD, Reason, dynamicLookupInfo);
      return;
    }

    if (auto *SD = dyn_cast<SubscriptDecl>(D)) {
      if (!hasIntroducer && !hasInitializerModifier)
        addSubscriptOverride(SD, Reason, dynamicLookupInfo);
    }

    if (auto *CD = dyn_cast<ConstructorDecl>(D)) {
      if (!isa<ProtocolDecl>(CD->getDeclContext()))
        return;
      if (hasIntroducer || hasOverride || hasOverridabilityModifier ||
          hasStaticOrClass)
        return;
      if (CD->isRequired() || CD->isDesignatedInit())
        addConstructor(CD, Reason, dynamicLookupInfo);
      return;
    }
  }

  void addDesignatedInitializers(NominalTypeDecl *NTD) {
    if (hasFuncIntroducer || hasVarIntroducer || hasTypealiasIntroducer ||
        hasOverridabilityModifier || hasStaticOrClass)
      return;

    const auto *CD = dyn_cast<ClassDecl>(NTD);
    if (!CD)
      return;
    if (!CD->hasSuperclass())
      return;
    CD = CD->getSuperclassDecl();
    for (const auto *Member : CD->getMembers()) {
      const auto *Constructor = dyn_cast<ConstructorDecl>(Member);
      if (!Constructor)
        continue;
      if (Constructor->hasStubImplementation())
        continue;
      if (Constructor->isDesignatedInit())
        addConstructor(Constructor, DeclVisibilityKind::MemberOfSuper, {});
    }
  }

  void addAssociatedTypes(NominalTypeDecl *NTD) {
    if (!hasTypealiasIntroducer &&
        (hasFuncIntroducer || hasVarIntroducer || hasInitializerModifier ||
         hasOverride || hasOverridabilityModifier || hasStaticOrClass))
      return;

    for (auto Conformance : NTD->getAllConformances()) {
      auto Proto = Conformance->getProtocol();
      if (!Proto->isAccessibleFrom(CurrDeclContext))
        continue;
      for (auto *ATD : Proto->getAssociatedTypeMembers()) {
        // FIXME: Also exclude the type alias that has already been specified.
        if (!Conformance->hasTypeWitness(ATD) ||
            ATD->hasDefaultDefinitionType())
          continue;
        addTypeAlias(
            ATD,
            DeclVisibilityKind::MemberOfProtocolConformedToByCurrentNominal,
            {});
      }
    }
  }

  static StringRef getResultBuilderDocComment(
      ResultBuilderBuildFunction function) {
    switch (function) {
    case ResultBuilderBuildFunction::BuildArray:
      return "Enables support for..in loops in a result builder by "
        "combining the results of all iterations into a single result";

    case ResultBuilderBuildFunction::BuildBlock:
      return "Required by every result builder to build combined results "
          "from statement blocks";

    case ResultBuilderBuildFunction::BuildEitherFirst:
      return "With buildEither(second:), enables support for 'if-else' and "
          "'switch' statements by folding conditional results into a single "
          "result";

    case ResultBuilderBuildFunction::BuildEitherSecond:
      return "With buildEither(first:), enables support for 'if-else' and "
          "'switch' statements by folding conditional results into a single "
          "result";

    case ResultBuilderBuildFunction::BuildExpression:
      return "If declared, provides contextual type information for statement "
          "expressions to translate them into partial results";

    case ResultBuilderBuildFunction::BuildFinalResult:
      return "If declared, this will be called on the partial result from the "
          "outermost block statement to produce the final returned result";

    case ResultBuilderBuildFunction::BuildLimitedAvailability:
      return "If declared, this will be called on the partial result of "
        "an 'if #available' block to allow the result builder to erase "
        "type information";

    case ResultBuilderBuildFunction::BuildOptional:
      return "Enables support for `if` statements that do not have an `else`";
    }
  }

  void addResultBuilderBuildCompletion(
      NominalTypeDecl *builder, Type componentType,
      ResultBuilderBuildFunction function) {
    CodeCompletionResultBuilder Builder(Sink, CodeCompletionResultKind::Pattern,
                                        SemanticContextKind::CurrentNominal,
                                        {});
    Builder.setResultTypeNotApplicable();

    if (!hasFuncIntroducer) {
      if (!hasAccessModifier &&
          builder->getFormalAccess() >= AccessLevel::Public)
        Builder.addAccessControlKeyword(AccessLevel::Public);

      if (!hasStaticOrClass)
        Builder.addTextChunk("static ");

      Builder.addTextChunk("func ");
    }

    std::string declStringWithoutFunc;
    {
      llvm::raw_string_ostream out(declStringWithoutFunc);
      printResultBuilderBuildFunction(
          builder, componentType, function, None, out);
    }
    Builder.addTextChunk(declStringWithoutFunc);
    Builder.addBraceStmtWithCursor();
    Builder.setBriefDocComment(getResultBuilderDocComment(function));
  }

  /// Add completions for the various "build" functions in a result builder.
  void addResultBuilderBuildCompletions(NominalTypeDecl *builder) {
    Type componentType = inferResultBuilderComponentType(builder);

    addResultBuilderBuildCompletion(
        builder, componentType, ResultBuilderBuildFunction::BuildBlock);
    addResultBuilderBuildCompletion(
        builder, componentType, ResultBuilderBuildFunction::BuildExpression);
    addResultBuilderBuildCompletion(
        builder, componentType, ResultBuilderBuildFunction::BuildOptional);
    addResultBuilderBuildCompletion(
        builder, componentType, ResultBuilderBuildFunction::BuildEitherFirst);
    addResultBuilderBuildCompletion(
        builder, componentType, ResultBuilderBuildFunction::BuildEitherSecond);
    addResultBuilderBuildCompletion(
        builder, componentType, ResultBuilderBuildFunction::BuildArray);
    addResultBuilderBuildCompletion(
        builder, componentType,
        ResultBuilderBuildFunction::BuildLimitedAvailability);
    addResultBuilderBuildCompletion(
        builder, componentType, ResultBuilderBuildFunction::BuildFinalResult);
  }

  void getOverrideCompletions(SourceLoc Loc) {
    if (!CurrDeclContext->isTypeContext())
      return;
    if (isa<ProtocolDecl>(CurrDeclContext))
      return;

    Type CurrTy = CurrDeclContext->getSelfTypeInContext();
    auto *NTD = CurrDeclContext->getSelfNominalTypeDecl();
    if (CurrTy && !CurrTy->is<ErrorType>()) {
      // Look for overridable static members too.
      Type Meta = MetatypeType::get(CurrTy);
      lookupVisibleMemberDecls(*this, Meta, CurrDeclContext,
                               /*includeInstanceMembers=*/true,
                               /*includeDerivedRequirements*/true,
                               /*includeProtocolExtensionMembers*/false);
      addDesignatedInitializers(NTD);
      addAssociatedTypes(NTD);
    }

    if (NTD && NTD->getAttrs().hasAttribute<ResultBuilderAttr>()) {
      addResultBuilderBuildCompletions(NTD);
    }
  }
};
} // end anonymous namespace

static void addSelectorModifierKeywords(CodeCompletionResultSink &sink) {
  auto addKeyword = [&](StringRef Name, CodeCompletionKeywordKind Kind) {
    CodeCompletionResultBuilder Builder(sink, CodeCompletionResultKind::Keyword,
                                        SemanticContextKind::None, {});
    Builder.setKeywordKind(Kind);
    Builder.addTextChunk(Name);
    Builder.addCallParameterColon();
    Builder.addSimpleTypedParameter("@objc property", /*IsVarArg=*/false);
  };

  addKeyword("getter", CodeCompletionKeywordKind::None);
  addKeyword("setter", CodeCompletionKeywordKind::None);
}

void CodeCompletionCallbacksImpl::completeDotExpr(CodeCompletionExpr *E,
                                                  SourceLoc DotLoc) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::DotExpr;
  if (ParseExprSelectorContext != ObjCSelectorContext::None) {
    PreferFunctionReferencesToCalls = true;
    CompleteExprSelectorContext = ParseExprSelectorContext;
  }

  ParsedExpr = E->getBase();
  this->DotLoc = DotLoc;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completeStmtOrExpr(CodeCompletionExpr *E) {
  assert(P.Tok.is(tok::code_complete));
  Kind = CompletionKind::StmtOrExpr;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completePostfixExprBeginning(CodeCompletionExpr *E) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::PostfixExprBeginning;
  if (ParseExprSelectorContext != ObjCSelectorContext::None) {
    PreferFunctionReferencesToCalls = true;
    CompleteExprSelectorContext = ParseExprSelectorContext;
    if (CompleteExprSelectorContext == ObjCSelectorContext::MethodSelector) {
      addSelectorModifierKeywords(CompletionContext.getResultSink());
    }
  }


  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completeForEachSequenceBeginning(
    CodeCompletionExpr *E) {
  assert(P.Tok.is(tok::code_complete));
  Kind = CompletionKind::ForEachSequence;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completePostfixExpr(Expr *E, bool hasSpace) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  HasSpace = hasSpace;
  Kind = CompletionKind::PostfixExpr;
  if (ParseExprSelectorContext != ObjCSelectorContext::None) {
    PreferFunctionReferencesToCalls = true;
    CompleteExprSelectorContext = ParseExprSelectorContext;
  }

  ParsedExpr = E;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completePostfixExprParen(Expr *E,
                                                           Expr *CodeCompletionE) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::PostfixExprParen;
  ParsedExpr = E;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = static_cast<CodeCompletionExpr*>(CodeCompletionE);

  ShouldCompleteCallPatternAfterParen = true;
  if (CompletionContext.getCallPatternHeuristics()) {
    // Lookahead one token to decide what kind of call completions to provide.
    // When it appears that there is already code for the call present, just
    // complete values and/or argument labels.  Otherwise give the entire call
    // pattern.
    Token next = P.peekToken();
    if (!next.isAtStartOfLine() && !next.is(tok::eof) && !next.is(tok::r_paren)) {
      ShouldCompleteCallPatternAfterParen = false;
    }
  }
}

void CodeCompletionCallbacksImpl::completeExprKeyPath(KeyPathExpr *KPE,
                                                      SourceLoc DotLoc) {
  Kind = (!KPE || KPE->isObjC()) ? CompletionKind::KeyPathExprObjC
                                 : CompletionKind::KeyPathExprSwift;
  ParsedExpr = KPE;
  this->DotLoc = DotLoc;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completePoundAvailablePlatform() {
  Kind = CompletionKind::PoundAvailablePlatform;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeDeclResultBeginning() {
  Kind = CompletionKind::TypeDeclResultBeginning;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeSimpleBeginning() {
  Kind = CompletionKind::TypeSimpleBeginning;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeDeclAttrParam(DeclAttrKind DK,
                                                        int Index) {
  Kind = CompletionKind::AttributeDeclParen;
  AttrKind = DK;
  AttrParamIndex = Index;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeEffectsSpecifier(bool hasAsync,
                                                           bool hasThrows) {
  Kind = CompletionKind::EffectsSpecifier;
  CurDeclContext = P.CurDeclContext;
  ParsedKeywords.clear();
  if (hasAsync)
    ParsedKeywords.emplace_back("async");
  if (hasThrows)
    ParsedKeywords.emplace_back("throws");
}

void CodeCompletionCallbacksImpl::completeDeclAttrBeginning(
    bool Sil, bool isIndependent) {
  Kind = CompletionKind::AttributeBegin;
  IsInSil = Sil;
  CurDeclContext = P.CurDeclContext;
  AttTargetIsIndependent = isIndependent;
}

void CodeCompletionCallbacksImpl::completeInPrecedenceGroup(SyntaxKind SK) {
  assert(P.Tok.is(tok::code_complete));

  SyntxKind = SK;
  Kind = CompletionKind::PrecedenceGroup;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeIdentifierWithDot(
    IdentTypeRepr *ITR) {
  if (!ITR) {
    completeTypeSimpleBeginning();
    return;
  }
  Kind = CompletionKind::TypeIdentifierWithDot;
  ParsedTypeLoc = TypeLoc(ITR);
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeIdentifierWithoutDot(
    IdentTypeRepr *ITR) {
  assert(ITR);
  Kind = CompletionKind::TypeIdentifierWithoutDot;
  ParsedTypeLoc = TypeLoc(ITR);
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeCaseStmtKeyword() {
  Kind = CompletionKind::CaseStmtKeyword;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeCaseStmtBeginning(CodeCompletionExpr *E) {
  assert(!InEnumElementRawValue);

  Kind = CompletionKind::CaseStmtBeginning;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completeImportDecl(
    ImportPath::Builder &Path) {
  Kind = CompletionKind::Import;
  CurDeclContext = P.CurDeclContext;
  DotLoc = Path.empty() ? SourceLoc() : Path.back().Loc;
  if (DotLoc.isInvalid())
    return;
  auto Importer = static_cast<ClangImporter *>(CurDeclContext->getASTContext().
                                               getClangModuleLoader());
  std::vector<std::string> SubNames;
  Importer->collectSubModuleNames(Path.get().getModulePath(false), SubNames);
  ASTContext &Ctx = CurDeclContext->getASTContext();
  Path.push_back(Identifier());
  for (StringRef Sub : SubNames) {
    Path.back().Item = Ctx.getIdentifier(Sub);
    SubModuleNameVisibilityPairs.push_back(
      std::make_pair(Sub.str(),
                     Ctx.getLoadedModule(Path.get().getModulePath(false))));
  }
  Path.pop_back();
}

void CodeCompletionCallbacksImpl::completeUnresolvedMember(CodeCompletionExpr *E,
    SourceLoc DotLoc) {
  Kind = CompletionKind::UnresolvedMember;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  this->DotLoc = DotLoc;
}

void CodeCompletionCallbacksImpl::completeCallArg(CodeCompletionExpr *E,
                                                  bool isFirst) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  Kind = CompletionKind::CallArg;

  ShouldCompleteCallPatternAfterParen = false;
  if (isFirst) {
    ShouldCompleteCallPatternAfterParen = true;
    if (CompletionContext.getCallPatternHeuristics()) {
      // Lookahead one token to decide what kind of call completions to provide.
      // When it appears that there is already code for the call present, just
      // complete values and/or argument labels.  Otherwise give the entire call
      // pattern.
      Token next = P.peekToken();
      if (!next.isAtStartOfLine() && !next.is(tok::eof) && !next.is(tok::r_paren)) {
        ShouldCompleteCallPatternAfterParen = false;
      }
    }
  }
}

void CodeCompletionCallbacksImpl::completeLabeledTrailingClosure(
    CodeCompletionExpr *E, bool isAtStartOfLine) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  Kind = CompletionKind::LabeledTrailingClosure;
  IsAtStartOfLine = isAtStartOfLine;
}

void CodeCompletionCallbacksImpl::completeReturnStmt(CodeCompletionExpr *E) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  Kind = CompletionKind::ReturnStmtExpr;
}

void CodeCompletionCallbacksImpl::completeYieldStmt(CodeCompletionExpr *E,
                                                    Optional<unsigned> index) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  // TODO: use a different completion kind when completing without an index
  // in a multiple-value context.
  Kind = CompletionKind::YieldStmtExpr;
}

void CodeCompletionCallbacksImpl::completeAfterPoundExpr(
    CodeCompletionExpr *E, Optional<StmtKind> ParentKind) {
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
  Kind = CompletionKind::AfterPoundExpr;
  ParentStmtKind = ParentKind;
}

void CodeCompletionCallbacksImpl::completeAfterPoundDirective() {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::AfterPoundDirective;
}

void CodeCompletionCallbacksImpl::completePlatformCondition() {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::PlatformConditon;
}

void CodeCompletionCallbacksImpl::completeAfterIfStmt(bool hasElse) {
  CurDeclContext = P.CurDeclContext;
  if (hasElse) {
    Kind = CompletionKind::AfterIfStmtElse;
  } else {
    Kind = CompletionKind::StmtOrExpr;
  }
}

void CodeCompletionCallbacksImpl::completeGenericRequirement() {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::GenericRequirement;
}

void CodeCompletionCallbacksImpl::completeNominalMemberBeginning(
    SmallVectorImpl<StringRef> &Keywords, SourceLoc introducerLoc) {
  assert(!InEnumElementRawValue);
  this->introducerLoc = introducerLoc;
  ParsedKeywords.clear();
  ParsedKeywords.append(Keywords.begin(), Keywords.end());
  Kind = CompletionKind::NominalMemberBeginning;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeAccessorBeginning(
    CodeCompletionExpr *E) {
  Kind = CompletionKind::AccessorBeginning;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completeStmtLabel(StmtKind ParentKind) {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::StmtLabel;
  ParentStmtKind = ParentKind;
}

void CodeCompletionCallbacksImpl::completeForEachPatternBeginning(
    bool hasTry, bool hasAwait) {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::ForEachPatternBeginning;
  ParsedKeywords.clear();
  if (hasTry)
    ParsedKeywords.emplace_back("try");
  if (hasAwait)
    ParsedKeywords.emplace_back("await");
}

void CodeCompletionCallbacksImpl::completeTypeAttrBeginning() {
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::TypeAttrBeginning;
}

static bool isDynamicLookup(Type T) {
  return T->getRValueType()->isAnyObject();
}

static bool isClangSubModule(ModuleDecl *TheModule) {
  if (auto ClangMod = TheModule->findUnderlyingClangModule())
    return ClangMod->isSubModule();
  return false;
}

static void addKeyword(CodeCompletionResultSink &Sink, StringRef Name,
                       CodeCompletionKeywordKind Kind,
                       StringRef TypeAnnotation = "",
                       CodeCompletionFlair Flair = {}) {
  CodeCompletionResultBuilder Builder(Sink, CodeCompletionResultKind::Keyword,
                                      SemanticContextKind::None, {});
  Builder.setKeywordKind(Kind);
  Builder.addKeyword(Name);
  Builder.addFlair(Flair);
  if (!TypeAnnotation.empty())
    Builder.addTypeAnnotation(TypeAnnotation);
  Builder.setResultTypeNotApplicable();
}

static void addDeclKeywords(CodeCompletionResultSink &Sink, DeclContext *DC,
                            bool IsConcurrencyEnabled,
                            bool IsDistributedEnabled) {
  auto isTypeDeclIntroducer = [](CodeCompletionKeywordKind Kind,
                                 Optional<DeclAttrKind> DAK) -> bool {
    switch (Kind) {
    case CodeCompletionKeywordKind::kw_protocol:
    case CodeCompletionKeywordKind::kw_class:
    case CodeCompletionKeywordKind::kw_struct:
    case CodeCompletionKeywordKind::kw_enum:
    case CodeCompletionKeywordKind::kw_extension:
      return true;
    case CodeCompletionKeywordKind::None:
      if (DAK && *DAK == DeclAttrKind::DAK_Actor) {
        return true;
      }
      break;
    default:
      break;
    }
    return false;
  };
  auto isTopLevelOnlyDeclIntroducer = [](CodeCompletionKeywordKind Kind,
                                         Optional<DeclAttrKind> DAK) -> bool {
    switch (Kind) {
    case CodeCompletionKeywordKind::kw_operator:
    case CodeCompletionKeywordKind::kw_precedencegroup:
    case CodeCompletionKeywordKind::kw_import:
    case CodeCompletionKeywordKind::kw_protocol:
    case CodeCompletionKeywordKind::kw_extension:
      return true;
    default:
      return false;
    }
  };

  auto getFlair = [&](CodeCompletionKeywordKind Kind,
                      Optional<DeclAttrKind> DAK) -> CodeCompletionFlair {
    if (isCodeCompletionAtTopLevelOfLibraryFile(DC)) {
      // Type decls are common in library file top-level.
      if (isTypeDeclIntroducer(Kind, DAK))
        return CodeCompletionFlairBit::CommonKeywordAtCurrentPosition;
    }
    if (isa<ProtocolDecl>(DC)) {
      // Protocols cannot have nested type decls (other than 'typealias').
      if (isTypeDeclIntroducer(Kind, DAK))
        return CodeCompletionFlairBit::RareKeywordAtCurrentPosition;
    }
    if (DC->isTypeContext()) {
      // Top-level only decls are invalid in type context.
      if (isTopLevelOnlyDeclIntroducer(Kind, DAK))
        return CodeCompletionFlairBit::RareKeywordAtCurrentPosition;
    }
    if (isCompletionDeclContextLocalContext(DC)) {
      // Local type decl are valid, but not common.
      if (isTypeDeclIntroducer(Kind, DAK))
        return CodeCompletionFlairBit::RareKeywordAtCurrentPosition;

      // Top-level only decls are invalid in function body.
      if (isTopLevelOnlyDeclIntroducer(Kind, DAK))
        return CodeCompletionFlairBit::RareKeywordAtCurrentPosition;

      // 'init', 'deinit' and 'subscript' are invalid in function body.
      // Access control modifiers are invalid in function body.
      switch (Kind) {
      case CodeCompletionKeywordKind::kw_init:
      case CodeCompletionKeywordKind::kw_deinit:
      case CodeCompletionKeywordKind::kw_subscript:
      case CodeCompletionKeywordKind::kw_private:
      case CodeCompletionKeywordKind::kw_fileprivate:
      case CodeCompletionKeywordKind::kw_internal:
      case CodeCompletionKeywordKind::kw_public:
      case CodeCompletionKeywordKind::kw_static:
        return CodeCompletionFlairBit::RareKeywordAtCurrentPosition;

      default:
        break;
      }

      // These modifiers are invalid for decls in function body.
      if (DAK) {
        switch (*DAK) {
        case DeclAttrKind::DAK_Lazy:
        case DeclAttrKind::DAK_Final:
        case DeclAttrKind::DAK_Infix:
        case DeclAttrKind::DAK_Frozen:
        case DeclAttrKind::DAK_Prefix:
        case DeclAttrKind::DAK_Postfix:
        case DeclAttrKind::DAK_Dynamic:
        case DeclAttrKind::DAK_Override:
        case DeclAttrKind::DAK_Optional:
        case DeclAttrKind::DAK_Required:
        case DeclAttrKind::DAK_Convenience:
        case DeclAttrKind::DAK_AccessControl:
        case DeclAttrKind::DAK_Nonisolated:
          return CodeCompletionFlairBit::RareKeywordAtCurrentPosition;

        default:
          break;
        }
      }
    }
    return None;
  };

  auto AddDeclKeyword = [&](StringRef Name, CodeCompletionKeywordKind Kind,
                            Optional<DeclAttrKind> DAK) {
    if (Name == "let" || Name == "var") {
      // Treat keywords that could be the start of a pattern specially.
      return;
    }

    // FIXME: This should use canUseAttributeOnDecl.

    // Remove user inaccessible keywords.
    if (DAK.hasValue() && DeclAttribute::isUserInaccessible(*DAK))
      return;

    // Remove keywords only available when concurrency is enabled.
    if (DAK.hasValue() && !IsConcurrencyEnabled &&
        DeclAttribute::isConcurrencyOnly(*DAK))
      return;

    // Remove keywords only available when distributed is enabled.
    if (DAK.hasValue() && !IsDistributedEnabled &&
        DeclAttribute::isDistributedOnly(*DAK))
      return;

    addKeyword(Sink, Name, Kind, /*TypeAnnotation=*/"", getFlair(Kind, DAK));
  };

#define DECL_KEYWORD(kw)                                                       \
  AddDeclKeyword(#kw, CodeCompletionKeywordKind::kw_##kw, None);
#include "swift/Syntax/TokenKinds.def"

  // Context-sensitive keywords.
  auto AddCSKeyword = [&](StringRef Name, DeclAttrKind Kind) {
    AddDeclKeyword(Name, CodeCompletionKeywordKind::None, Kind);
  };

#define CONTEXTUAL_CASE(KW, CLASS) AddCSKeyword(#KW, DAK_##CLASS);
#define CONTEXTUAL_DECL_ATTR(KW, CLASS, ...) CONTEXTUAL_CASE(KW, CLASS)
#define CONTEXTUAL_DECL_ATTR_ALIAS(KW, CLASS) CONTEXTUAL_CASE(KW, CLASS)
#define CONTEXTUAL_SIMPLE_DECL_ATTR(KW, CLASS, ...) CONTEXTUAL_CASE(KW, CLASS)
#include <swift/AST/Attr.def>
#undef CONTEXTUAL_CASE
}

static void addStmtKeywords(CodeCompletionResultSink &Sink, DeclContext *DC,
                            bool MaybeFuncBody) {
  CodeCompletionFlair flair;
  // Starting a statement at top-level in non-script files is invalid.
  if (isCodeCompletionAtTopLevelOfLibraryFile(DC)) {
    flair |= CodeCompletionFlairBit::ExpressionAtNonScriptOrMainFileScope;
  }

  auto AddStmtKeyword = [&](StringRef Name, CodeCompletionKeywordKind Kind) {
    if (!MaybeFuncBody && Kind == CodeCompletionKeywordKind::kw_return)
      return;
    addKeyword(Sink, Name, Kind, "", flair);
  };
#define STMT_KEYWORD(kw) AddStmtKeyword(#kw, CodeCompletionKeywordKind::kw_##kw);
#include "swift/Syntax/TokenKinds.def"
}

static void addCaseStmtKeywords(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "case", CodeCompletionKeywordKind::kw_case);
  addKeyword(Sink, "default", CodeCompletionKeywordKind::kw_default);
}

static void addLetVarKeywords(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "let", CodeCompletionKeywordKind::kw_let);
  addKeyword(Sink, "var", CodeCompletionKeywordKind::kw_var);
}

static void addAccessorKeywords(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "get", CodeCompletionKeywordKind::None);
  addKeyword(Sink, "set", CodeCompletionKeywordKind::None);
}

static void addObserverKeywords(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "willSet", CodeCompletionKeywordKind::None);
  addKeyword(Sink, "didSet", CodeCompletionKeywordKind::None);
}

static void addExprKeywords(CodeCompletionResultSink &Sink, DeclContext *DC) {
  // Expression is invalid at top-level of non-script files.
  CodeCompletionFlair flair;
  if (isCodeCompletionAtTopLevelOfLibraryFile(DC)) {
    flair |= CodeCompletionFlairBit::ExpressionAtNonScriptOrMainFileScope;
  }

  // Expr keywords.
  addKeyword(Sink, "try", CodeCompletionKeywordKind::kw_try, "", flair);
  addKeyword(Sink, "try!", CodeCompletionKeywordKind::kw_try, "", flair);
  addKeyword(Sink, "try?", CodeCompletionKeywordKind::kw_try, "", flair);
  addKeyword(Sink, "await", CodeCompletionKeywordKind::None, "", flair);
}

static void addOpaqueTypeKeyword(CodeCompletionResultSink &Sink) {
  addKeyword(Sink, "some", CodeCompletionKeywordKind::None, "some");
}

static void addAnyTypeKeyword(CodeCompletionResultSink &Sink, Type T) {
  CodeCompletionResultBuilder Builder(Sink, CodeCompletionResultKind::Keyword,
                                      SemanticContextKind::None, {});
  Builder.setKeywordKind(CodeCompletionKeywordKind::None);
  Builder.addKeyword("Any");
  Builder.addTypeAnnotation(T, PrintOptions());
}

void CodeCompletionCallbacksImpl::addKeywords(CodeCompletionResultSink &Sink,
                                              bool MaybeFuncBody) {
  switch (Kind) {
  case CompletionKind::None:
  case CompletionKind::DotExpr:
  case CompletionKind::AttributeDeclParen:
  case CompletionKind::AttributeBegin:
  case CompletionKind::PoundAvailablePlatform:
  case CompletionKind::Import:
  case CompletionKind::UnresolvedMember:
  case CompletionKind::LabeledTrailingClosure:
  case CompletionKind::AfterPoundExpr:
  case CompletionKind::AfterPoundDirective:
  case CompletionKind::PlatformConditon:
  case CompletionKind::GenericRequirement:
  case CompletionKind::KeyPathExprObjC:
  case CompletionKind::KeyPathExprSwift:
  case CompletionKind::PrecedenceGroup:
  case CompletionKind::StmtLabel:
  case CompletionKind::TypeAttrBeginning:
    break;

  case CompletionKind::EffectsSpecifier: {
    if (!llvm::is_contained(ParsedKeywords, "async"))
      addKeyword(Sink, "async", CodeCompletionKeywordKind::None);
    if (!llvm::is_contained(ParsedKeywords, "throws"))
      addKeyword(Sink, "throws", CodeCompletionKeywordKind::kw_throws);
    break;
  }

  case CompletionKind::AccessorBeginning: {
    // TODO: Omit already declared or mutally exclusive accessors.
    //       E.g. If 'get' is already declared, emit 'set' only.
    addAccessorKeywords(Sink);

    // Only 'var' for non-protocol context can have 'willSet' and 'didSet'.
    assert(ParsedDecl);
    VarDecl *var = dyn_cast<VarDecl>(ParsedDecl);
    if (auto accessor = dyn_cast<AccessorDecl>(ParsedDecl))
      var = dyn_cast<VarDecl>(accessor->getStorage());
    if (var && !var->getDeclContext()->getSelfProtocolDecl())
      addObserverKeywords(Sink);

    if (!isa<AccessorDecl>(ParsedDecl))
      break;

    MaybeFuncBody = true;
    LLVM_FALLTHROUGH;
  }
  case CompletionKind::StmtOrExpr:
    addDeclKeywords(Sink, CurDeclContext,
                    Context.LangOpts.EnableExperimentalConcurrency,
                    Context.LangOpts.EnableExperimentalDistributed);
    addStmtKeywords(Sink, CurDeclContext, MaybeFuncBody);
    LLVM_FALLTHROUGH;
  case CompletionKind::ReturnStmtExpr:
  case CompletionKind::YieldStmtExpr:
  case CompletionKind::PostfixExprBeginning:
  case CompletionKind::ForEachSequence:
    addSuperKeyword(Sink);
    addLetVarKeywords(Sink);
    addExprKeywords(Sink, CurDeclContext);
    addAnyTypeKeyword(Sink, CurDeclContext->getASTContext().TheAnyType);
    break;

  case CompletionKind::CallArg:
  case CompletionKind::PostfixExprParen:
    // Note that we don't add keywords here as the completion might be for
    // an argument list pattern. We instead add keywords later in
    // CodeCompletionCallbacksImpl::doneParsing when we know we're not
    // completing for a argument list pattern.
    break;

  case CompletionKind::CaseStmtKeyword:
    addCaseStmtKeywords(Sink);
    break;

  case CompletionKind::PostfixExpr:
  case CompletionKind::CaseStmtBeginning:
  case CompletionKind::TypeIdentifierWithDot:
  case CompletionKind::TypeIdentifierWithoutDot:
    break;

  case CompletionKind::TypeDeclResultBeginning: {
    auto DC = CurDeclContext;
    if (ParsedDecl && ParsedDecl == CurDeclContext->getAsDecl())
      DC = ParsedDecl->getDeclContext();
    if (!isa<ProtocolDecl>(DC))
      if (DC->isTypeContext() || isa_and_nonnull<FuncDecl>(ParsedDecl))
        addOpaqueTypeKeyword(Sink);

    LLVM_FALLTHROUGH;
  }
  case CompletionKind::TypeSimpleBeginning:
    addAnyTypeKeyword(Sink, CurDeclContext->getASTContext().TheAnyType);
    break;

  case CompletionKind::NominalMemberBeginning: {
    bool HasDeclIntroducer = llvm::find_if(ParsedKeywords,
                                           [this](const StringRef kw) {
      return llvm::StringSwitch<bool>(kw)
        .Case("associatedtype", true)
        .Case("class", !CurDeclContext || !isa<ClassDecl>(CurDeclContext))
        .Case("deinit", true)
        .Case("enum", true)
        .Case("extension", true)
        .Case("func", true)
        .Case("import", true)
        .Case("init", true)
        .Case("let", true)
        .Case("operator", true)
        .Case("precedencegroup", true)
        .Case("protocol", true)
        .Case("struct", true)
        .Case("subscript", true)
        .Case("typealias", true)
        .Case("var", true)
        .Default(false);
    }) != ParsedKeywords.end();
    if (!HasDeclIntroducer) {
      addDeclKeywords(Sink, CurDeclContext,
                      Context.LangOpts.EnableExperimentalConcurrency,
                      Context.LangOpts.EnableExperimentalDistributed);
      addLetVarKeywords(Sink);
    }
    break;
  }

  case CompletionKind::AfterIfStmtElse:
    addKeyword(Sink, "if", CodeCompletionKeywordKind::kw_if);
    break;
  case CompletionKind::ForEachPatternBeginning:
    if (!llvm::is_contained(ParsedKeywords, "try"))
      addKeyword(Sink, "try", CodeCompletionKeywordKind::kw_try);
    if (!llvm::is_contained(ParsedKeywords, "await"))
      addKeyword(Sink, "await", CodeCompletionKeywordKind::None);
    addKeyword(Sink, "var", CodeCompletionKeywordKind::kw_var);
    addKeyword(Sink, "case", CodeCompletionKeywordKind::kw_case);
  }
}

static void addPoundDirectives(CodeCompletionResultSink &Sink) {
  auto addWithName =
      [&](StringRef name, CodeCompletionKeywordKind K,
          llvm::function_ref<void(CodeCompletionResultBuilder &)> consumer =
              nullptr) {
        CodeCompletionResultBuilder Builder(Sink,
                                            CodeCompletionResultKind::Keyword,
                                            SemanticContextKind::None, {});
        Builder.addBaseName(name);
        Builder.setKeywordKind(K);
        if (consumer)
          consumer(Builder);
      };

  addWithName("sourceLocation", CodeCompletionKeywordKind::pound_sourceLocation,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addLeftParen();
    Builder.addTextChunk("file");
    Builder.addCallParameterColon();
    Builder.addSimpleTypedParameter("String");
    Builder.addComma();
    Builder.addTextChunk("line");
    Builder.addCallParameterColon();
    Builder.addSimpleTypedParameter("Int");
    Builder.addRightParen();
  });
  addWithName("warning", CodeCompletionKeywordKind::pound_warning,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addLeftParen();
    Builder.addTextChunk("\"");
    Builder.addSimpleNamedParameter("message");
    Builder.addTextChunk("\"");
    Builder.addRightParen();
  });
  addWithName("error", CodeCompletionKeywordKind::pound_error,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addLeftParen();
    Builder.addTextChunk("\"");
    Builder.addSimpleNamedParameter("message");
    Builder.addTextChunk("\"");
    Builder.addRightParen();
  });

  addWithName("if ", CodeCompletionKeywordKind::pound_if,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("condition");
  });

  // FIXME: These directives are only valid in conditional completion block.
  addWithName("elseif ", CodeCompletionKeywordKind::pound_elseif,
              [&] (CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("condition");
  });
  addWithName("else", CodeCompletionKeywordKind::pound_else);
  addWithName("endif", CodeCompletionKeywordKind::pound_endif);
}

/// Add platform conditions used in '#if' and '#elseif' directives.
static void addPlatformConditions(CodeCompletionResultSink &Sink) {
  auto addWithName =
      [&](StringRef Name,
          llvm::function_ref<void(CodeCompletionResultBuilder & Builder)>
              consumer) {
        CodeCompletionResultBuilder Builder(
            Sink, CodeCompletionResultKind::Pattern,
            // FIXME: SemanticContextKind::CurrentModule is not correct.
            // Use 'None' (and fix prioritization) or introduce a new context.
            SemanticContextKind::CurrentModule, {});
        Builder.addFlair(CodeCompletionFlairBit::ExpressionSpecific);
        Builder.addBaseName(Name);
        Builder.addLeftParen();
        consumer(Builder);
        Builder.addRightParen();
      };

  addWithName("os", [](CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("name");
  });
  addWithName("arch", [](CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("name");
  });
  addWithName("canImport", [](CodeCompletionResultBuilder &Builder) {
    Builder.addSimpleNamedParameter("module");
  });
  addWithName("targetEnvironment", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk("simulator");
  });
  addWithName("swift", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk(">=");
    Builder.addSimpleNamedParameter("version");
  });
  addWithName("swift", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk("<");
    Builder.addSimpleNamedParameter("version");
  });
  addWithName("compiler", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk(">=");
    Builder.addSimpleNamedParameter("version");
  });
  addWithName("compiler", [](CodeCompletionResultBuilder &Builder) {
    Builder.addTextChunk("<");
    Builder.addSimpleNamedParameter("version");
  });

  addKeyword(Sink, "true", CodeCompletionKeywordKind::kw_true, "Bool");
  addKeyword(Sink, "false", CodeCompletionKeywordKind::kw_false, "Bool");
}

/// Add flags specified by '-D' to completion results.
static void addConditionalCompilationFlags(ASTContext &Ctx,
                                           CodeCompletionResultSink &Sink) {
  for (auto Flag : Ctx.LangOpts.getCustomConditionalCompilationFlags()) {
    // TODO: Should we filter out some flags?
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResultKind::Keyword,
        // FIXME: SemanticContextKind::CurrentModule is not correct.
        // Use 'None' (and fix prioritization) or introduce a new context.
        SemanticContextKind::CurrentModule, {});
    Builder.addFlair(CodeCompletionFlairBit::ExpressionSpecific);
    Builder.addTextChunk(Flag);
  }
}

/// Add flairs to the each item in \p results .
///
/// If \p Sink is passed, the pointer of the each result may be replaced with a
/// pointer to the new item allocated in \p Sink.
/// If \p Sink is nullptr, the pointee of each result may be modified in place.
static void postProcessResults(MutableArrayRef<CodeCompletionResult *> results,
                               CompletionKind Kind, DeclContext *DC,
                               CodeCompletionResultSink *Sink) {
  for (CodeCompletionResult *&result : results) {
    bool modified = false;
    auto flair = result->getFlair();

    // Starting a statement with a protocol name is not common. So protocol
    // names at non-type name position are "rare".
    if (result->getKind() == CodeCompletionResultKind::Declaration &&
        result->getAssociatedDeclKind() == CodeCompletionDeclKind::Protocol &&
        Kind != CompletionKind::TypeSimpleBeginning &&
        Kind != CompletionKind::TypeIdentifierWithoutDot &&
        Kind != CompletionKind::TypeIdentifierWithDot &&
        Kind != CompletionKind::TypeDeclResultBeginning &&
        Kind != CompletionKind::GenericRequirement) {
      flair |= CodeCompletionFlairBit::RareTypeAtCurrentPosition;
      modified = true;
    }

    // Starting a statement at top-level in non-script files is invalid.
    if (Kind == CompletionKind::StmtOrExpr &&
        result->getKind() == CodeCompletionResultKind::Declaration &&
        isCodeCompletionAtTopLevelOfLibraryFile(DC)) {
      flair |= CodeCompletionFlairBit::ExpressionAtNonScriptOrMainFileScope;
      modified = true;
    }

    if (!modified)
      continue;

    if (Sink) {
      // Replace the result with a new result with the flair.
      result = result->withFlair(flair, *Sink);
    } else {
      // 'Sink' == nullptr means the result is modifiable in place.
      result->setFlair(flair);
    }
  }
}

static void deliverCompletionResults(CodeCompletionContext &CompletionContext,
                                     CompletionLookup &Lookup,
                                     DeclContext *DC,
                                     CodeCompletionConsumer &Consumer) {
  auto &SF = *DC->getParentSourceFile();
  llvm::SmallPtrSet<Identifier, 8> seenModuleNames;
  std::vector<RequestedCachedModule> RequestedModules;

  SmallPtrSet<ModuleDecl *, 4> explictlyImportedModules;
  {
    // Collect modules directly imported in this SourceFile.
    SmallVector<ImportedModule, 4> directImport;
    SF.getImportedModules(directImport,
                          {ModuleDecl::ImportFilterKind::Default,
                           ModuleDecl::ImportFilterKind::ImplementationOnly});
    for (auto import : directImport)
      explictlyImportedModules.insert(import.importedModule);

    // Exclude modules implicitly imported in the current module.
    auto implicitImports = SF.getParentModule()->getImplicitImports();
    for (auto import : implicitImports.imports)
      explictlyImportedModules.erase(import.module.importedModule);

    // Consider the current module "explicit".
    explictlyImportedModules.insert(SF.getParentModule());
  }

  for (auto &Request: Lookup.RequestedCachedResults) {
    llvm::DenseSet<CodeCompletionCache::Key> ImportsSeen;
    auto handleImport = [&](ImportedModule Import) {
      ModuleDecl *TheModule = Import.importedModule;
      ImportPath::Access Path = Import.accessPath;
      if (TheModule->getFiles().empty())
        return;

      // Clang submodules are ignored and there's no lookup cost involved,
      // so just ignore them and don't put the empty results in the cache
      // because putting a lot of objects in the cache will push out
      // other lookups.
      if (isClangSubModule(TheModule))
        return;

      std::vector<std::string> AccessPath;
      for (auto Piece : Path) {
        AccessPath.push_back(std::string(Piece.Item));
      }

      StringRef ModuleFilename = TheModule->getModuleFilename();
      // ModuleFilename can be empty if something strange happened during
      // module loading, for example, the module file is corrupted.
      if (!ModuleFilename.empty()) {
        CodeCompletionCache::Key K{
            ModuleFilename.str(),
            std::string(TheModule->getName()),
            AccessPath,
            Request.NeedLeadingDot,
            SF.hasTestableOrPrivateImport(
                AccessLevel::Internal, TheModule,
                SourceFile::ImportQueryKind::TestableOnly),
            SF.hasTestableOrPrivateImport(
                AccessLevel::Internal, TheModule,
                SourceFile::ImportQueryKind::PrivateOnly),
            CompletionContext.getAddInitsToTopLevel(),
            CompletionContext.addCallWithNoDefaultArgs(),
            CompletionContext.getAnnotateResult()};

        using PairType = llvm::DenseSet<swift::ide::CodeCompletionCache::Key,
            llvm::DenseMapInfo<CodeCompletionCache::Key>>::iterator;
        std::pair<PairType, bool> Result = ImportsSeen.insert(K);
        if (!Result.second)
          return; // already handled.
        RequestedModules.push_back({std::move(K), TheModule,
          Request.OnlyTypes, Request.OnlyPrecedenceGroups});

        auto TheModuleName = TheModule->getName();
        if (Request.IncludeModuleQualifier &&
            (!Lookup.isHiddenModuleName(TheModuleName) ||
             explictlyImportedModules.contains(TheModule)) &&
            seenModuleNames.insert(TheModuleName).second)
          Lookup.addModuleName(TheModule);
      }
    };

    if (Request.TheModule) {
      // FIXME: actually check imports.
      for (auto Import : namelookup::getAllImports(Request.TheModule)) {
        handleImport(Import);
      }
    } else {
      // Add results from current module.
      Lookup.getToplevelCompletions(Request.OnlyTypes);

      // Add the qualifying module name
      auto curModule = SF.getParentModule();
      if (Request.IncludeModuleQualifier &&
          seenModuleNames.insert(curModule->getName()).second)
        Lookup.addModuleName(curModule);

      // Add results for all imported modules.
      SmallVector<ImportedModule, 4> Imports;
      SF.getImportedModules(
          Imports, {ModuleDecl::ImportFilterKind::Exported,
                    ModuleDecl::ImportFilterKind::Default,
                    ModuleDecl::ImportFilterKind::ImplementationOnly});

      for (auto Imported : Imports) {
        for (auto Import : namelookup::getAllImports(Imported.importedModule))
          handleImport(Import);
      }
    }
  }
  Lookup.RequestedCachedResults.clear();
  CompletionContext.typeContextKind = Lookup.typeContextKind();

  postProcessResults(CompletionContext.getResultSink().Results,
                     CompletionContext.CodeCompletionKind, DC,
                     /*Sink=*/nullptr);

  Consumer.handleResultsAndModules(CompletionContext, RequestedModules, DC);
}

void deliverUnresolvedMemberResults(
    ArrayRef<UnresolvedMemberTypeCheckCompletionCallback::ExprResult> Results,
    ArrayRef<Type> EnumPatternTypes, DeclContext *DC, SourceLoc DotLoc,
    ide::CodeCompletionContext &CompletionCtx,
    CodeCompletionConsumer &Consumer) {
  ASTContext &Ctx = DC->getASTContext();
  CompletionLookup Lookup(CompletionCtx.getResultSink(), Ctx, DC,
                          &CompletionCtx);

  assert(DotLoc.isValid());
  Lookup.setHaveDot(DotLoc);
  Lookup.shouldCheckForDuplicates(Results.size() + EnumPatternTypes.size() > 1);

  // Get the canonical versions of the top-level types
  SmallPtrSet<CanType, 4> originalTypes;
  for (auto &Result: Results)
    originalTypes.insert(Result.ExpectedTy->getCanonicalType());

  for (auto &Result: Results) {
    Lookup.setExpectedTypes({Result.ExpectedTy},
                            Result.IsImplicitSingleExpressionReturn,
                            /*expectsNonVoid*/true);
    Lookup.setIdealExpectedType(Result.ExpectedTy);

    // For optional types, also get members of the unwrapped type if it's not
    // already equivalent to one of the top-level types. Handling it via the top
    // level type and not here ensures we give the correct type relation
    // (identical, rather than convertible).
    if (Result.ExpectedTy->getOptionalObjectType()) {
      Type Unwrapped = Result.ExpectedTy->lookThroughAllOptionalTypes();
      if (originalTypes.insert(Unwrapped->getCanonicalType()).second)
        Lookup.getUnresolvedMemberCompletions(Unwrapped);
    }
    Lookup.getUnresolvedMemberCompletions(Result.ExpectedTy);
  }

  // Offer completions when interpreting the pattern match as an
  // EnumElementPattern.
  for (auto &Ty : EnumPatternTypes) {
    Lookup.setExpectedTypes({Ty}, /*IsImplicitSingleExpressionReturn=*/false,
                            /*expectsNonVoid=*/true);
    Lookup.setIdealExpectedType(Ty);

    // We can pattern match MyEnum against Optional<MyEnum>
    if (Ty->getOptionalObjectType()) {
      Type Unwrapped = Ty->lookThroughAllOptionalTypes();
      Lookup.getEnumElementPatternCompletions(Unwrapped);
    }

    Lookup.getEnumElementPatternCompletions(Ty);
  }

  deliverCompletionResults(CompletionCtx, Lookup, DC, Consumer);
}

void deliverKeyPathResults(
    ArrayRef<KeyPathTypeCheckCompletionCallback::Result> Results,
    DeclContext *DC, SourceLoc DotLoc,
    ide::CodeCompletionContext &CompletionCtx,
    CodeCompletionConsumer &Consumer) {
  ASTContext &Ctx = DC->getASTContext();
  CompletionLookup Lookup(CompletionCtx.getResultSink(), Ctx, DC,
                          &CompletionCtx);

  if (DotLoc.isValid()) {
    Lookup.setHaveDot(DotLoc);
  }
  Lookup.shouldCheckForDuplicates(Results.size() > 1);

  for (auto &Result : Results) {
    Lookup.setIsSwiftKeyPathExpr(Result.OnRoot);
    Lookup.getValueExprCompletions(Result.BaseType);
  }

  deliverCompletionResults(CompletionCtx, Lookup, DC, Consumer);
}

void deliverDotExprResults(
    ArrayRef<DotExprTypeCheckCompletionCallback::Result> Results,
    Expr *BaseExpr, DeclContext *DC, SourceLoc DotLoc, bool IsInSelector,
    ide::CodeCompletionContext &CompletionCtx,
    CodeCompletionConsumer &Consumer) {
  ASTContext &Ctx = DC->getASTContext();
  CompletionLookup Lookup(CompletionCtx.getResultSink(), Ctx, DC,
                          &CompletionCtx);

  if (DotLoc.isValid())
    Lookup.setHaveDot(DotLoc);

  Lookup.setIsSuperRefExpr(isa<SuperRefExpr>(BaseExpr));

  if (auto *DRE = dyn_cast<DeclRefExpr>(BaseExpr))
    Lookup.setIsSelfRefExpr(DRE->getDecl()->getName() == Ctx.Id_self);

  if (isa<BindOptionalExpr>(BaseExpr) || isa<ForceValueExpr>(BaseExpr))
    Lookup.setIsUnwrappedOptional(true);

  if (IsInSelector) {
    Lookup.includeInstanceMembers();
    Lookup.setPreferFunctionReferencesToCalls();
  }

  Lookup.shouldCheckForDuplicates(Results.size() > 1);
  for (auto &Result: Results) {
    Lookup.setIsStaticMetatype(Result.BaseIsStaticMetaType);
    Lookup.getPostfixKeywordCompletions(Result.BaseTy, BaseExpr);
    Lookup.setExpectedTypes(Result.ExpectedTypes,
                            Result.IsImplicitSingleExpressionReturn,
                            Result.ExpectsNonVoid);
    if (isDynamicLookup(Result.BaseTy))
      Lookup.setIsDynamicLookup();
    Lookup.getValueExprCompletions(Result.BaseTy, Result.BaseDecl);
  }

  deliverCompletionResults(CompletionCtx, Lookup, DC, Consumer);
}

bool CodeCompletionCallbacksImpl::trySolverCompletion(bool MaybeFuncBody) {
  assert(ParsedExpr || CurDeclContext);

  SourceLoc CompletionLoc = ParsedExpr
    ? ParsedExpr->getLoc()
    : CurDeclContext->getASTContext().SourceMgr.getCodeCompletionLoc();

  switch (Kind) {
  case CompletionKind::DotExpr: {
    assert(CodeCompleteTokenExpr);
    assert(CurDeclContext);

    DotExprTypeCheckCompletionCallback Lookup(CurDeclContext,
                                              CodeCompleteTokenExpr);
    llvm::SaveAndRestore<TypeCheckCompletionCallback*>
      CompletionCollector(Context.CompletionCallback, &Lookup);
    typeCheckContextAt(CurDeclContext, CompletionLoc);

    // This (hopefully) only happens in cases where the expression isn't
    // typechecked during normal compilation either (e.g. member completion in a
    // switch case where there control expression is invalid). Having normal
    // typechecking still resolve even these cases would be beneficial for
    // tooling in general though.
    if (!Lookup.gotCallback())
      Lookup.fallbackTypeCheck();

    addKeywords(CompletionContext.getResultSink(), MaybeFuncBody);

    Expr *CheckedBase = CodeCompleteTokenExpr->getBase();
    deliverDotExprResults(Lookup.getResults(), CheckedBase, CurDeclContext,
                          DotLoc, isInsideObjCSelector(), CompletionContext,
                          Consumer);
    return true;
  }
  case CompletionKind::UnresolvedMember: {
    assert(CodeCompleteTokenExpr);
    assert(CurDeclContext);

    UnresolvedMemberTypeCheckCompletionCallback Lookup(CodeCompleteTokenExpr);
    llvm::SaveAndRestore<TypeCheckCompletionCallback*>
      CompletionCollector(Context.CompletionCallback, &Lookup);
    typeCheckContextAt(CurDeclContext, CompletionLoc);

    if (!Lookup.gotCallback())
      Lookup.fallbackTypeCheck(CurDeclContext);

    addKeywords(CompletionContext.getResultSink(), MaybeFuncBody);
    deliverUnresolvedMemberResults(Lookup.getExprResults(),
                                   Lookup.getEnumPatternTypes(), CurDeclContext,
                                   DotLoc, CompletionContext, Consumer);
    return true;
  }
  case CompletionKind::KeyPathExprSwift: {
    assert(CurDeclContext);

    // CodeCompletionCallbacks::completeExprKeyPath takes a \c KeyPathExpr,
    // so we can safely cast the \c ParsedExpr back to a \c KeyPathExpr.
    auto KeyPath = cast<KeyPathExpr>(ParsedExpr);
    KeyPathTypeCheckCompletionCallback Lookup(KeyPath);
    llvm::SaveAndRestore<TypeCheckCompletionCallback *> CompletionCollector(
        Context.CompletionCallback, &Lookup);
    typeCheckContextAt(CurDeclContext, CompletionLoc);

    deliverKeyPathResults(Lookup.getResults(), CurDeclContext, DotLoc,
                          CompletionContext, Consumer);
    return true;
  }
  default:
    return false;
  }
}

// Undoes the single-expression closure/function body transformation on the
// given DeclContext and its parent contexts if they have a single expression
// body that contains the code completion location.
//
// FIXME: Remove this once all expression position completions are migrated
// to work via TypeCheckCompletionCallback.
static void undoSingleExpressionReturn(DeclContext *DC) {
  auto updateBody = [](BraceStmt *BS, ASTContext &Ctx) -> bool {
    ASTNode LastElem = BS->getLastElement();
    auto *RS = dyn_cast_or_null<ReturnStmt>(LastElem.dyn_cast<Stmt*>());

    if (!RS || !RS->isImplicit())
      return false;

    BS->setLastElement(RS->getResult());
    return true;
  };

  while (ClosureExpr *CE = dyn_cast_or_null<ClosureExpr>(DC)) {
    if (CE->hasSingleExpressionBody()) {
      if (updateBody(CE->getBody(), CE->getASTContext()))
        CE->setBody(CE->getBody(), false);
    }
    DC = DC->getParent();
  }
  if (FuncDecl *FD = dyn_cast_or_null<FuncDecl>(DC)) {
    if (FD->hasSingleExpressionBody()) {
      if (updateBody(FD->getBody(), FD->getASTContext()))
        FD->setHasSingleExpressionBody(false);
    }
  }
}

void CodeCompletionCallbacksImpl::doneParsing() {
  CompletionContext.CodeCompletionKind = Kind;

  if (Kind == CompletionKind::None) {
    return;
  }

  bool MaybeFuncBody = true;
  if (CurDeclContext) {
    auto *CD = CurDeclContext->getLocalContext();
    if (!CD || CD->getContextKind() == DeclContextKind::Initializer ||
        CD->getContextKind() == DeclContextKind::TopLevelCodeDecl)
      MaybeFuncBody = false;
  }

  if (auto *DC = dyn_cast_or_null<DeclContext>(ParsedDecl)) {
    if (DC->isChildContextOf(CurDeclContext))
      CurDeclContext = DC;
  }

  if (trySolverCompletion(MaybeFuncBody))
    return;

  undoSingleExpressionReturn(CurDeclContext);
  typeCheckContextAt(
      CurDeclContext,
      ParsedExpr
          ? ParsedExpr->getLoc()
          : CurDeclContext->getASTContext().SourceMgr.getCodeCompletionLoc());

  // Add keywords even if type checking fails completely.
  addKeywords(CompletionContext.getResultSink(), MaybeFuncBody);

  Optional<Type> ExprType;
  ConcreteDeclRef ReferencedDecl = nullptr;
  if (ParsedExpr) {
    if (auto *checkedExpr = findParsedExpr(CurDeclContext,
                                           ParsedExpr->getSourceRange())) {
      ParsedExpr = checkedExpr;
    }

    if (auto typechecked = typeCheckParsedExpr()) {
      ExprType = typechecked->first;
      ReferencedDecl = typechecked->second;
      ParsedExpr->setType(*ExprType);
    }

    if (!ExprType && Kind != CompletionKind::PostfixExprParen &&
        Kind != CompletionKind::CallArg &&
        Kind != CompletionKind::KeyPathExprObjC)
      return;
  }

  if (!ParsedTypeLoc.isNull() && !typecheckParsedType())
    return;

  CompletionLookup Lookup(CompletionContext.getResultSink(), P.Context,
                          CurDeclContext, &CompletionContext);
  if (ExprType) {
    Lookup.setIsStaticMetatype(ParsedExpr->isStaticallyDerivedMetatype());
  }
  if (auto *DRE = dyn_cast_or_null<DeclRefExpr>(ParsedExpr)) {
    Lookup.setIsSelfRefExpr(DRE->getDecl()->getName() == Context.Id_self);
  } else if (isa_and_nonnull<SuperRefExpr>(ParsedExpr)) {
    Lookup.setIsSuperRefExpr();
  }

  if (isInsideObjCSelector())
    Lookup.includeInstanceMembers();
  if (PreferFunctionReferencesToCalls)
    Lookup.setPreferFunctionReferencesToCalls();

  auto DoPostfixExprBeginning = [&] (){
    SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
    Lookup.getValueCompletionsInDeclContext(Loc);
    Lookup.getSelfTypeCompletionInDeclContext(Loc, /*isForDeclResult=*/false);
  };

  switch (Kind) {
  case CompletionKind::None:
  case CompletionKind::DotExpr:
  case CompletionKind::UnresolvedMember:
  case CompletionKind::KeyPathExprSwift:
    llvm_unreachable("should be already handled");
    return;

  case CompletionKind::StmtOrExpr:
  case CompletionKind::ForEachSequence:
  case CompletionKind::PostfixExprBeginning: {
    ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);
    Lookup.setExpectedTypes(ContextInfo.getPossibleTypes(),
                            ContextInfo.isImplicitSingleExpressionReturn());
    DoPostfixExprBeginning();
    break;
  }

  case CompletionKind::PostfixExpr: {
    Lookup.setHaveLeadingSpace(HasSpace);
    if (isDynamicLookup(*ExprType))
      Lookup.setIsDynamicLookup();
    Lookup.getValueExprCompletions(*ExprType, ReferencedDecl.getDecl());
    /// We set the type of ParsedExpr explicitly above. But we don't want an
    /// unresolved type in our AST when we type check again for operator
    /// completions. Remove the type of the ParsedExpr and see if we can come up
    /// with something more useful based on the the full sequence expression.
    if (ParsedExpr->getType()->is<UnresolvedType>()) {
      ParsedExpr->setType(nullptr);
    }
    Lookup.getOperatorCompletions(ParsedExpr, leadingSequenceExprs);
    Lookup.getPostfixKeywordCompletions(*ExprType, ParsedExpr);
    break;
  }

  case CompletionKind::PostfixExprParen: {
    Lookup.setHaveLParen(true);

    ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);

    if (ShouldCompleteCallPatternAfterParen) {
      ExprContextInfo ParentContextInfo(CurDeclContext, ParsedExpr);
      Lookup.setExpectedTypes(
          ParentContextInfo.getPossibleTypes(),
          ParentContextInfo.isImplicitSingleExpressionReturn());
      if (!ContextInfo.getPossibleCallees().empty()) {
        for (auto &typeAndDecl : ContextInfo.getPossibleCallees())
          Lookup.tryFunctionCallCompletions(typeAndDecl.Type, typeAndDecl.Decl,
                                            typeAndDecl.SemanticContext);
      } else if (ExprType && ((*ExprType)->is<AnyFunctionType>() ||
                              (*ExprType)->is<AnyMetatypeType>())) {
        Lookup.getValueExprCompletions(*ExprType, ReferencedDecl.getDecl());
      }
    } else {
      // Add argument labels, then fallthrough to get values.
      Lookup.addCallArgumentCompletionResults(ContextInfo.getPossibleParams());
    }

    if (!Lookup.FoundFunctionCalls ||
        (Lookup.FoundFunctionCalls &&
         Lookup.FoundFunctionsWithoutFirstKeyword)) {
      Lookup.setExpectedTypes(ContextInfo.getPossibleTypes(),
                              ContextInfo.isImplicitSingleExpressionReturn());
      Lookup.setHaveLParen(false);

      // Add any keywords that can be used in an argument expr position.
      addSuperKeyword(CompletionContext.getResultSink());
      addExprKeywords(CompletionContext.getResultSink(), CurDeclContext);

      DoPostfixExprBeginning();
    }
    break;
  }

  case CompletionKind::KeyPathExprObjC: {
    if (DotLoc.isValid())
      Lookup.setHaveDot(DotLoc);
    Lookup.setIsKeyPathExpr();
    Lookup.includeInstanceMembers();

    if (ExprType) {
      if (isDynamicLookup(*ExprType))
        Lookup.setIsDynamicLookup();

      Lookup.getValueExprCompletions(*ExprType, ReferencedDecl.getDecl());
    } else {
      SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
      Lookup.getValueCompletionsInDeclContext(Loc, KeyPathFilter,
                                              /*LiteralCompletions=*/false);
    }
    break;
  }

  case CompletionKind::TypeDeclResultBeginning:
  case CompletionKind::TypeSimpleBeginning: {
    auto Loc = Context.SourceMgr.getCodeCompletionLoc();
    Lookup.getTypeCompletionsInDeclContext(Loc);
    Lookup.getSelfTypeCompletionInDeclContext(
        Loc, Kind == CompletionKind::TypeDeclResultBeginning);
    break;
  }

  case CompletionKind::TypeIdentifierWithDot: {
    Lookup.setHaveDot(SourceLoc());
    Lookup.getTypeCompletions(ParsedTypeLoc.getType());
    break;
  }

  case CompletionKind::TypeIdentifierWithoutDot: {
    Lookup.getTypeCompletions(ParsedTypeLoc.getType());
    break;
  }

  case CompletionKind::CaseStmtBeginning: {
    ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);
    Lookup.setExpectedTypes(ContextInfo.getPossibleTypes(),
                            ContextInfo.isImplicitSingleExpressionReturn());
    Lookup.setIdealExpectedType(CodeCompleteTokenExpr->getType());
    Lookup.getUnresolvedMemberCompletions(ContextInfo.getPossibleTypes());
    DoPostfixExprBeginning();
    break;
  }

  case CompletionKind::NominalMemberBeginning: {
    CompletionOverrideLookup OverrideLookup(CompletionContext.getResultSink(),
                                            P.Context, CurDeclContext,
                                            ParsedKeywords, introducerLoc);
    OverrideLookup.getOverrideCompletions(SourceLoc());
    break;
  }

  case CompletionKind::AccessorBeginning: {
    if (isa<AccessorDecl>(ParsedDecl)) {
      ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);
      Lookup.setExpectedTypes(ContextInfo.getPossibleTypes(),
                              ContextInfo.isImplicitSingleExpressionReturn());
      DoPostfixExprBeginning();
    }
    break;
  }

  case CompletionKind::AttributeBegin: {
    Lookup.getAttributeDeclCompletions(IsInSil, AttTargetDK);

    // TypeName at attribute position after '@'.
    // - VarDecl: Property Wrappers.
    // - ParamDecl/VarDecl/FuncDecl: Function Builders.
    if (!AttTargetDK || *AttTargetDK == DeclKind::Var ||
        *AttTargetDK == DeclKind::Param || *AttTargetDK == DeclKind::Func)
      Lookup.getTypeCompletionsInDeclContext(
          P.Context.SourceMgr.getCodeCompletionLoc());
    break;
  }
  case CompletionKind::AttributeDeclParen: {
    Lookup.getAttributeDeclParamCompletions(AttrKind, AttrParamIndex);
    break;
  }
  case CompletionKind::PoundAvailablePlatform: {
    Lookup.getPoundAvailablePlatformCompletions();
    break;
  }
  case CompletionKind::Import: {
    if (DotLoc.isValid())
      Lookup.addSubModuleNames(SubModuleNameVisibilityPairs);
    else
      Lookup.addImportModuleNames();
    break;
  }
  case CompletionKind::CallArg: {
    ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);

    bool shouldPerformGlobalCompletion = true;

    if (ShouldCompleteCallPatternAfterParen &&
        !ContextInfo.getPossibleCallees().empty()) {
      Lookup.setHaveLParen(true);
      for (auto &typeAndDecl : ContextInfo.getPossibleCallees()) {
        auto apply = ContextInfo.getAnalyzedExpr();
        if (isa_and_nonnull<SubscriptExpr>(apply)) {
          Lookup.addSubscriptCallPattern(
              typeAndDecl.Type,
              dyn_cast_or_null<SubscriptDecl>(typeAndDecl.Decl),
              typeAndDecl.SemanticContext);
        } else {
          Lookup.addFunctionCallPattern(
              typeAndDecl.Type,
              dyn_cast_or_null<AbstractFunctionDecl>(typeAndDecl.Decl),
              typeAndDecl.SemanticContext);
        }
      }
      Lookup.setHaveLParen(false);

      shouldPerformGlobalCompletion =
          !Lookup.FoundFunctionCalls ||
          (Lookup.FoundFunctionCalls &&
           Lookup.FoundFunctionsWithoutFirstKeyword);
    } else if (!ContextInfo.getPossibleParams().empty()) {
      auto params = ContextInfo.getPossibleParams();
      Lookup.addCallArgumentCompletionResults(params);

      shouldPerformGlobalCompletion = !ContextInfo.getPossibleTypes().empty();
      // Fallback to global completion if the position is out of number. It's
      // better than suggest nothing.
      shouldPerformGlobalCompletion |= llvm::all_of(
          params, [](const PossibleParamInfo &P) { return !P.Param; });
    }

    if (shouldPerformGlobalCompletion) {
      Lookup.setExpectedTypes(ContextInfo.getPossibleTypes(),
                              ContextInfo.isImplicitSingleExpressionReturn());

      // Add any keywords that can be used in an argument expr position.
      addSuperKeyword(CompletionContext.getResultSink());
      addExprKeywords(CompletionContext.getResultSink(), CurDeclContext);

      DoPostfixExprBeginning();
    }
    break;
  }

  case CompletionKind::LabeledTrailingClosure: {
    ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);

    SmallVector<PossibleParamInfo, 2> params;
    // Only complete function type parameters
    llvm::copy_if(ContextInfo.getPossibleParams(), std::back_inserter(params),
                  [](const PossibleParamInfo &P) {
                    // nullptr indicates out of bounds.
                    if (!P.Param)
                      return true;
                    return P.Param->getPlainType()
                        ->lookThroughAllOptionalTypes()
                        ->is<AnyFunctionType>();
                  });

    bool allRequired = false;
    if (!params.empty()) {
      Lookup.addCallArgumentCompletionResults(
          params, /*isLabeledTrailingClosure=*/true);
      allRequired = llvm::all_of(
          params, [](const PossibleParamInfo &P) { return P.IsRequired; });
    }

    // If there're optional parameters, do global completion or member
    // completion depending on the completion is happening at the start of line.
    if (!allRequired) {
      if (IsAtStartOfLine) {
        //   foo() {}
        //   <HERE>

        auto &Sink = CompletionContext.getResultSink();
        if (isa<Initializer>(CurDeclContext))
          CurDeclContext = CurDeclContext->getParent();

        if (CurDeclContext->isTypeContext()) {
          // Override completion (CompletionKind::NominalMemberBeginning).
          addDeclKeywords(Sink, CurDeclContext,
                          Context.LangOpts.EnableExperimentalConcurrency,
                          Context.LangOpts.EnableExperimentalDistributed);
          addLetVarKeywords(Sink);
          SmallVector<StringRef, 0> ParsedKeywords;
          CompletionOverrideLookup OverrideLookup(Sink, Context, CurDeclContext,
                                                  ParsedKeywords, SourceLoc());
          OverrideLookup.getOverrideCompletions(SourceLoc());
        } else {
          // Global completion (CompletionKind::PostfixExprBeginning).
          addDeclKeywords(Sink, CurDeclContext,
                          Context.LangOpts.EnableExperimentalConcurrency,
                          Context.LangOpts.EnableExperimentalDistributed);
          addStmtKeywords(Sink, CurDeclContext, MaybeFuncBody);
          addSuperKeyword(Sink);
          addLetVarKeywords(Sink);
          addExprKeywords(Sink, CurDeclContext);
          addAnyTypeKeyword(Sink, Context.TheAnyType);
          DoPostfixExprBeginning();
        }
      } else {
        //   foo() {} <HERE>
        // Member completion.
        Expr *analyzedExpr = ContextInfo.getAnalyzedExpr();
        if (!analyzedExpr)
          break;

        // Only if the completion token is the last token in the call.
        if (analyzedExpr->getEndLoc() != CodeCompleteTokenExpr->getLoc())
          break;

        Type resultTy = analyzedExpr->getType();
        // If the call expression doesn't have a type, fallback to:
        if (!resultTy || resultTy->is<ErrorType>()) {
          // 1) Try to type check removing CodeCompletionExpr from the call.
          Expr *removedExpr = analyzedExpr;
          removeCodeCompletionExpr(CurDeclContext->getASTContext(),
                                   removedExpr);
          ConcreteDeclRef referencedDecl;
          auto optT = getTypeOfCompletionContextExpr(
              CurDeclContext->getASTContext(), CurDeclContext,
              CompletionTypeCheckKind::Normal, removedExpr, referencedDecl);
          if (optT) {
            resultTy = *optT;
            analyzedExpr->setType(resultTy);
          }
        }
        if (!resultTy || resultTy->is<ErrorType>()) {
          // 2) Infer it from the possible callee info.
          if (!ContextInfo.getPossibleCallees().empty()) {
            auto calleeInfo = ContextInfo.getPossibleCallees()[0];
            resultTy = calleeInfo.Type->getResult();
            analyzedExpr->setType(resultTy);
          }
        }
        if (!resultTy || resultTy->is<ErrorType>()) {
          // 3) Give up providing postfix completions.
          break;
        }

        auto &SM = CurDeclContext->getASTContext().SourceMgr;
        auto leadingChar =
            SM.extractText({SM.getCodeCompletionLoc().getAdvancedLoc(-1), 1});
        Lookup.setHaveLeadingSpace(leadingChar.find_first_of(" \t\f\v") !=
                                   StringRef::npos);

        if (isDynamicLookup(resultTy))
          Lookup.setIsDynamicLookup();
        Lookup.getValueExprCompletions(resultTy, /*VD=*/nullptr);
        Lookup.getOperatorCompletions(analyzedExpr, leadingSequenceExprs);
        Lookup.getPostfixKeywordCompletions(resultTy, analyzedExpr);
      }
    }
    break;
  }

  case CompletionKind::ReturnStmtExpr : {
    SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
    SmallVector<Type, 2> possibleReturnTypes;
    collectPossibleReturnTypesFromContext(CurDeclContext, possibleReturnTypes);
    Lookup.setExpectedTypes(possibleReturnTypes,
                            /*isImplicitSingleExpressionReturn*/ false);
    Lookup.getValueCompletionsInDeclContext(Loc);
    break;
  }

  case CompletionKind::YieldStmtExpr: {
    SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
    if (auto FD = dyn_cast<AccessorDecl>(CurDeclContext)) {
      if (FD->isCoroutine()) {
        // TODO: handle multi-value yields.
        Lookup.setExpectedTypes(FD->getStorage()->getValueInterfaceType(),
                                /*isImplicitSingleExpressionReturn*/ false);
      }
    }
    Lookup.getValueCompletionsInDeclContext(Loc);
    break;
  }

  case CompletionKind::AfterPoundExpr: {
    ExprContextInfo ContextInfo(CurDeclContext, CodeCompleteTokenExpr);
    Lookup.setExpectedTypes(ContextInfo.getPossibleTypes(),
                            ContextInfo.isImplicitSingleExpressionReturn());

    Lookup.addPoundAvailable(ParentStmtKind);
    Lookup.addPoundLiteralCompletions(/*needPound=*/false);
    Lookup.addObjCPoundKeywordCompletions(/*needPound=*/false);
    break;
  }

  case CompletionKind::AfterPoundDirective: {
    addPoundDirectives(CompletionContext.getResultSink());
    // FIXME: Add pound expressions (e.g. '#selector()') if it's at statements
    // position.
    break;
  }

  case CompletionKind::PlatformConditon: {
    addPlatformConditions(CompletionContext.getResultSink());
    addConditionalCompilationFlags(CurDeclContext->getASTContext(),
                                   CompletionContext.getResultSink());
    break;
  }

  case CompletionKind::GenericRequirement: {
    auto Loc = Context.SourceMgr.getCodeCompletionLoc();
    Lookup.getGenericRequirementCompletions(CurDeclContext, Loc);
    break;
  }
  case CompletionKind::PrecedenceGroup:
    Lookup.getPrecedenceGroupCompletions(SyntxKind);
    break;
  case CompletionKind::StmtLabel: {
    SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
    Lookup.getStmtLabelCompletions(Loc, ParentStmtKind == StmtKind::Continue);
    break;
  }
  case CompletionKind::TypeAttrBeginning: {
    Lookup.getTypeAttributeKeywordCompletions();

    // Type names at attribute position after '@'.
    Lookup.getTypeCompletionsInDeclContext(
      P.Context.SourceMgr.getCodeCompletionLoc());
    break;

  }
  case CompletionKind::AfterIfStmtElse:
  case CompletionKind::CaseStmtKeyword:
  case CompletionKind::EffectsSpecifier:
  case CompletionKind::ForEachPatternBeginning:
    // Handled earlier by keyword completions.
    break;
  }

  deliverCompletionResults(CompletionContext, Lookup, CurDeclContext, Consumer);
}

namespace {
class CodeCompletionCallbacksFactoryImpl
    : public CodeCompletionCallbacksFactory {
  CodeCompletionContext &CompletionContext;
  CodeCompletionConsumer &Consumer;

public:
  CodeCompletionCallbacksFactoryImpl(CodeCompletionContext &CompletionContext,
                                     CodeCompletionConsumer &Consumer)
      : CompletionContext(CompletionContext), Consumer(Consumer) {}

  CodeCompletionCallbacks *createCodeCompletionCallbacks(Parser &P) override {
    return new CodeCompletionCallbacksImpl(P, CompletionContext, Consumer);
  }
};
} // end anonymous namespace

CodeCompletionCallbacksFactory *
swift::ide::makeCodeCompletionCallbacksFactory(
    CodeCompletionContext &CompletionContext,
    CodeCompletionConsumer &Consumer) {
  return new CodeCompletionCallbacksFactoryImpl(CompletionContext, Consumer);
}

void swift::ide::lookupCodeCompletionResultsFromModule(
    CodeCompletionResultSink &targetSink, const ModuleDecl *module,
    ArrayRef<std::string> accessPath, bool needLeadingDot,
    const SourceFile *SF) {
  // Use the SourceFile as the decl context, to avoid decl context specific
  // behaviors.
  CompletionLookup Lookup(targetSink, module->getASTContext(), SF);
  Lookup.lookupExternalModuleDecls(module, accessPath, needLeadingDot);
}

static MutableArrayRef<CodeCompletionResult *>
copyCodeCompletionResults(CodeCompletionResultSink &targetSink,
                          CodeCompletionCache::Value &source, bool onlyTypes,
                          bool onlyPrecedenceGroups) {

  // We will be adding foreign results (from another sink) into TargetSink.
  // TargetSink should have an owning pointer to the allocator that keeps the
  // results alive.
  targetSink.ForeignAllocators.push_back(source.Allocator);
  auto startSize = targetSink.Results.size();

  std::function<bool(const ContextFreeCodeCompletionResult *)>
      shouldIncludeResult;
  if (onlyTypes) {
    shouldIncludeResult = [](const ContextFreeCodeCompletionResult *R) -> bool {
      if (R->getKind() != CodeCompletionResultKind::Declaration)
        return false;
      switch (R->getAssociatedDeclKind()) {
      case CodeCompletionDeclKind::Module:
      case CodeCompletionDeclKind::Class:
      case CodeCompletionDeclKind::Actor:
      case CodeCompletionDeclKind::Struct:
      case CodeCompletionDeclKind::Enum:
      case CodeCompletionDeclKind::Protocol:
      case CodeCompletionDeclKind::TypeAlias:
      case CodeCompletionDeclKind::AssociatedType:
      case CodeCompletionDeclKind::GenericTypeParam:
        return true;
      case CodeCompletionDeclKind::PrecedenceGroup:
      case CodeCompletionDeclKind::EnumElement:
      case CodeCompletionDeclKind::Constructor:
      case CodeCompletionDeclKind::Destructor:
      case CodeCompletionDeclKind::Subscript:
      case CodeCompletionDeclKind::StaticMethod:
      case CodeCompletionDeclKind::InstanceMethod:
      case CodeCompletionDeclKind::PrefixOperatorFunction:
      case CodeCompletionDeclKind::PostfixOperatorFunction:
      case CodeCompletionDeclKind::InfixOperatorFunction:
      case CodeCompletionDeclKind::FreeFunction:
      case CodeCompletionDeclKind::StaticVar:
      case CodeCompletionDeclKind::InstanceVar:
      case CodeCompletionDeclKind::LocalVar:
      case CodeCompletionDeclKind::GlobalVar:
        return false;
      }

      llvm_unreachable("Unhandled CodeCompletionDeclKind in switch.");
    };
  } else if (onlyPrecedenceGroups) {
    shouldIncludeResult = [](const ContextFreeCodeCompletionResult *R) -> bool {
      return R->getAssociatedDeclKind() ==
             CodeCompletionDeclKind::PrecedenceGroup;
    };
  } else {
    shouldIncludeResult = [](const ContextFreeCodeCompletionResult *R) -> bool {
      return true;
    };
  }
  for (auto contextFreeResult : source.Results) {
    if (!shouldIncludeResult(contextFreeResult)) {
      continue;
    }
    auto contextualResult = new (*targetSink.Allocator) CodeCompletionResult(
        *contextFreeResult, SemanticContextKind::OtherModule,
        CodeCompletionFlair(),
        /*numBytesToErase=*/0, /*TypeContext=*/nullptr, /*DC=*/nullptr,
        ContextualNotRecommendedReason::None,
        CodeCompletionDiagnosticSeverity::None, /*DiagnosticMessage=*/"");
    targetSink.Results.push_back(contextualResult);
  }

  return llvm::makeMutableArrayRef(targetSink.Results.data() + startSize,
                                   targetSink.Results.size() - startSize);
}

void SimpleCachingCodeCompletionConsumer::handleResultsAndModules(
    CodeCompletionContext &context,
    ArrayRef<RequestedCachedModule> requestedModules,
    DeclContext *DC) {

  // Use the current SourceFile as the DeclContext so that we can use it to
  // perform qualified lookup, and to get the correct visibility for
  // @testable imports. Also it cannot use 'DC' since it would apply decl
  // context changes to cached results.
  const SourceFile *SF = DC->getParentSourceFile();

  for (auto &R : requestedModules) {
    // FIXME(thread-safety): lock the whole AST context.  We might load a
    // module.
    llvm::Optional<CodeCompletionCache::ValueRefCntPtr> V =
        context.Cache.get(R.Key);
    if (!V.hasValue()) {
      // No cached results found. Fill the cache.
      V = context.Cache.createValue();
      // Temporary sink in which we gather the result. The cache value retains
      // the sink's allocator.
      CodeCompletionResultSink Sink;
      Sink.annotateResult = context.getAnnotateResult();
      Sink.addInitsToTopLevel = context.getAddInitsToTopLevel();
      Sink.enableCallPatternHeuristics = context.getCallPatternHeuristics();
      Sink.includeObjectLiterals = context.includeObjectLiterals();
      Sink.addCallWithNoDefaultArgs = context.addCallWithNoDefaultArgs();
      lookupCodeCompletionResultsFromModule(Sink, R.TheModule, R.Key.AccessPath,
                                            R.Key.ResultsHaveLeadingDot, SF);
      (*V)->Allocator = Sink.Allocator;
      auto &CachedResults = (*V)->Results;
      CachedResults.reserve(Sink.Results.size());
      // Instead of copying the context free results out of the sink's allocator
      // retain the sink's entire allocator (which also includes the contextual
      // properities) and simply store pointers to the context free results that
      // back the contextual results.
      for (auto Result : Sink.Results) {
        CachedResults.push_back(Result->getContextFreeResultPtr());
      }
      context.Cache.set(R.Key, *V);
    }
    assert(V.hasValue());
    auto newItems = copyCodeCompletionResults(
        context.getResultSink(), **V, R.OnlyTypes, R.OnlyPrecedenceGroups);
    postProcessResults(newItems, context.CodeCompletionKind, DC,
                       &context.getResultSink());
  }

  handleResults(context);
}

//===----------------------------------------------------------------------===//
// ImportDepth
//===----------------------------------------------------------------------===//

ImportDepth::ImportDepth(ASTContext &context,
                         const FrontendOptions &frontendOptions) {
  llvm::DenseSet<ModuleDecl *> seen;
  std::deque<std::pair<ModuleDecl *, uint8_t>> worklist;

  StringRef mainModule = frontendOptions.ModuleName;
  auto *main = context.getLoadedModule(context.getIdentifier(mainModule));
  assert(main && "missing main module");
  worklist.emplace_back(main, uint8_t(0));

  // Imports from -import-name such as Playground auxiliary sources are treated
  // specially by applying import depth 0.
  llvm::StringSet<> auxImports;
  for (const auto &pair : frontendOptions.getImplicitImportModuleNames())
    auxImports.insert(pair.first);

  // Private imports from this module.
  // FIXME: only the private imports from the current source file.
  // FIXME: ImportFilterKind::ShadowedByCrossImportOverlay?
  SmallVector<ImportedModule, 16> mainImports;
  main->getImportedModules(mainImports,
                           {ModuleDecl::ImportFilterKind::Default,
                            ModuleDecl::ImportFilterKind::ImplementationOnly});
  for (auto &import : mainImports) {
    uint8_t depth = 1;
    if (auxImports.count(import.importedModule->getName().str()))
      depth = 0;
    worklist.emplace_back(import.importedModule, depth);
  }

  // Fill depths with BFS over module imports.
  while (!worklist.empty()) {
    ModuleDecl *module;
    uint8_t depth;
    std::tie(module, depth) = worklist.front();
    worklist.pop_front();

    if (!seen.insert(module).second)
      continue;

    // Insert new module:depth mapping.
    const clang::Module *CM = module->findUnderlyingClangModule();
    if (CM) {
      depths[CM->getFullModuleName()] = depth;
    } else {
      depths[module->getName().str()] = depth;
    }

    // Add imports to the worklist.
    SmallVector<ImportedModule, 16> imports;
    module->getImportedModules(imports);
    for (auto &import : imports) {
      uint8_t next = std::max(depth, uint8_t(depth + 1)); // unsigned wrap

      // Implicitly imported sub-modules get the same depth as their parent.
      if (const clang::Module *CMI =
              import.importedModule->findUnderlyingClangModule())
        if (CM && CMI->isSubModuleOf(CM))
          next = depth;
      worklist.emplace_back(import.importedModule, next);
    }
  }
}

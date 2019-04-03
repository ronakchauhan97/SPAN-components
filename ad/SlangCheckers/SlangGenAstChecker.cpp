//===----------------------------------------------------------------------===//
//  MIT License.
//  Copyright (c) 2019 The SLANG Authors.
//
//  Author: Ronak Chauhan (r.chauhan@somaiya.edu)
//  Author: Anshuman Dhuliya [AD] (dhuliya@cse.iitb.ac.in)
//
//  If SlangGenAstChecker class name is added or changed, then also edit,
//  ../../../../include/clang/StaticAnalyzer/Checkers/Checkers.td
//  if this checker is named `SlangGenAst` (in Checkers.td) then it can be used as follows,
//
//      clang --analyze -Xanalyzer -analyzer-checker=debug.slanggen test.c |& tee mylog
//
//  which generates the file `test.c.spanir`.
//===----------------------------------------------------------------------===//

#include "ClangSACheckers.h"
#include "clang/AST/Decl.h" //AD
#include "clang/AST/Expr.h" //AD
#include "clang/AST/Stmt.h" //AD
#include "clang/AST/Type.h" //AD
#include "clang/Analysis/CFG.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/AnalysisManager.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h" //AD
#include <fstream>                    //AD
#include <iomanip>                    //AD for std::fixed
#include <sstream>                    //AD
#include <string>                     //AD
#include <unordered_map>              //AD
#include <utility>                    //AD
#include <vector>                     //AD

#include "SlangUtil.h"

using namespace slang;
using namespace clang;
using namespace ento;

typedef std::vector<const Stmt *> StmtVector;

// non-breaking space
#define NBSP1 " "
#define NBSP2 NBSP1 NBSP1
#define NBSP4 NBSP2 NBSP2
#define NBSP6 NBSP2 NBSP4
#define NBSP8 NBSP4 NBSP4
#define NBSP10 NBSP4 NBSP6
#define NBSP12 NBSP6 NBSP6

#define VAR_NAME_PREFIX "v:"
#define FUNC_NAME_PREFIX "f:"

#define DONT_PRINT "DONT_PRINT"
#define NULL_STMT "NULL_STMT"

#define LABEL_PREFIX "instr.LabelI(\""
#define LABEL_SUFFIX "\")"

// Generate the SPAN IR from Clang AST.
namespace {
// the numbering 0,1,2 is important.
enum EdgeLabel { FalseEdge = 0, TrueEdge = 1, UnCondEdge = 2 };
enum SlangRecordKind { Struct = 0, Union = 1 };

class SlangExpr {
public:
  std::string expr;
  bool compound;
  std::string locStr;
  QualType qualType;
  bool nonTmpVar;
  uint64_t varId;

  SlangExpr() {
    expr = "";
    compound = false;
    locStr = "";
    qualType = QualType();
    nonTmpVar = true;
    varId = 0;
  };

  std::string toString() {
    std::stringstream ss;
    ss << "SlangExpr:\n";
    ss << "  Expr     : " << expr << "\n";
    ss << "  ExprType : " << qualType.getAsString() << "\n";
    ss << "  NonTmpVar: " << (nonTmpVar ? "true" : "false") << "\n";
    ss << "  VarId    : " << varId << "\n";

    return ss.str();
  }
};

class SlangVar {
public:
  uint64_t id;
  // variable name: e.g. a variable 'x' in main function, is "v:main:x".
  std::string name;
  std::string typeStr;

  SlangVar() {}

  SlangVar(uint64_t id, std::string name) {
    // specially for anonymous field names (needed in member expressions)
    this->id = id;
    this->name = name;
    this->typeStr = DONT_PRINT;
  }

  std::string convertToString() {
    std::stringstream ss;
    ss << "\"" << name << "\": " << typeStr << ",";
    return ss.str();
  }

  void setLocalVarName(std::string varName, std::string funcName) {
    name = VAR_NAME_PREFIX;
    name += funcName + ":" + varName;
  }

  void setGlobalVarName(std::string varName) {
    name = VAR_NAME_PREFIX;
    name += varName;
  }
}; // class SlangVar

// holds info of a single function
class SlangFunc {
public:
  std::string name;     // e.g. 'main'
  std::string fullName; // e.g. 'f:main'
  std::string retType;
  std::vector<std::string> paramNames;
  bool variadic;

  uint32_t tmpVarCount;
  const Stmt *lastDeclStmt;

  std::vector<std::string> spanStmts;

  SlangFunc() {
    variadic = false;
    paramNames = std::vector<std::string>{};
    tmpVarCount = 0;
  }
}; // class SlangFunc

class SlangRecordField {
public:
  bool anonymous;
  std::string name;
  std::string typeStr;
  QualType type;

  SlangRecordField() : anonymous{false}, name{""}, typeStr{""}, type{QualType()} {}

  std::string getName() const { return name; }

  std::string toString() {
    std::stringstream ss;
    ss << "("
       << "\"" << name << "\"";
    ss << ", " << typeStr << ")";
    return ss.str();
  }

  void clear() {
    anonymous = false;
    name = "";
    typeStr = "";
    type = QualType();
  }
}; // class SlangRecordField

// holds a struct or a union record
class SlangRecord {
public:
  SlangRecordKind recordKind; // Struct, or Union
  bool anonymous;
  std::string name;
  std::vector<SlangRecordField> fields;
  std::string locStr;
  int32_t nextAnonymousFieldId;

  SlangRecord() {
    recordKind = Struct; // Struct, or Union
    anonymous = false;
    name = "";
    nextAnonymousFieldId = 0;
  }

  std::string getNextAnonymousFieldIdStr() {
    std::stringstream ss;
    nextAnonymousFieldId += 1;
    ss << nextAnonymousFieldId;
    return ss.str();
  }

  std::vector<SlangRecordField> getFields() const { return fields; }

  std::string toString() {
    std::stringstream ss;
    ss << NBSP6;
    ss << ((recordKind == Struct) ? "types.Struct(\n" : "types.Union(\n");

    ss << NBSP8 << "name = ";
    ss << "\"" << name << "\""
       << ",\n";

    std::string suffix = ",\n";
    ss << NBSP8 << "fields = [\n";
    for (auto field : fields) {
      ss << NBSP10 << field.toString() << suffix;
    }
    ss << NBSP8 << "],\n";

    ss << NBSP8 << "loc = " << locStr << ",\n";
    ss << NBSP6 << ")"; // close types.*(...

    return ss.str();
  }

  std::string toShortString() {
    std::stringstream ss;

    if (recordKind == Struct) {
      ss << "types.Struct";
    } else {
      ss << "types.Union";
    }
    ss << "(\"" << name << "\")";

    return ss.str();
  }
}; // class SlangRecord

// holds details of the entire translation unit
class SlangTranslationUnit {
public:
  std::string fileName; // the current translation unit file name
  SlangFunc *currFunc;  // the current function being translated

  uint64_t trueLabelCount;
  uint64_t falseLabelCount;
  uint64_t startConditionLabelCount;
  uint64_t endConditionLabelCount;

  uint64_t nextTrueLabelCount() { return ++trueLabelCount; }

  uint64_t nextFalseLabelCount() { return ++falseLabelCount; }

  uint64_t nextStartConditionLabelCount() { return ++startConditionLabelCount; }

  uint64_t nextEndConditionLabelCount() { return ++endConditionLabelCount; }

  // to uniquely name anonymous records (see getNextRecordId())
  int32_t recordId;

  // maps a unique variable id to its SlangVar.
  std::unordered_map<uint64_t, SlangVar> varMap;
  // map of var-name to a count:
  // used in case two local variables have same name (blocks)
  std::unordered_map<std::string, int32_t> varNameMap;
  // contains functions
  std::unordered_map<uint64_t, SlangFunc> funcMap;
  // contains structs
  std::unordered_map<uint64_t, SlangRecord> recordMap;

  // tracks variables that become dirty in an expression
  std::unordered_map<uint64_t, SlangExpr> dirtyVars;

  SlangTranslationUnit()
      : fileName{}, currFunc{nullptr}, recordId{0}, varMap{}, varNameMap{}, funcMap{}, dirtyVars{} {
    trueLabelCount = 0;
    falseLabelCount = 0;
    endConditionLabelCount = 0;
  }

  // clear the buffer for the next function.
  void clear() {
    varMap.clear();
    dirtyVars.clear();
    varNameMap.clear();
  } // clear()

  void addStmt(std::string spanStmt) { currFunc->spanStmts.push_back(spanStmt); }

  void pushBackFuncParams(std::string paramName) {
    SLANG_TRACE("AddingParam: " << paramName << " to func " << currFunc->name)
    currFunc->paramNames.push_back(paramName);
  }

  void setFuncReturnType(std::string &retType) { currFunc->retType = retType; }

  void setVariadicness(bool variadic) { currFunc->variadic = variadic; }

  std::string getCurrFuncName() {
    return currFunc->name; // not fullName
  }

  SlangVar &getVar(uint64_t varAddr) {
    // FIXME: there is no check
    return varMap[varAddr];
  }

  void setLastDeclStmtTo(const Stmt *declStmt) { currFunc->lastDeclStmt = declStmt; }

  const Stmt *getLastDeclStmt() const { return currFunc->lastDeclStmt; }

  bool isNewVar(uint64_t varAddr) { return varMap.find(varAddr) == varMap.end(); }

  uint32_t nextTmpId() {
    currFunc->tmpVarCount += 1;
    return currFunc->tmpVarCount;
  }

  void addVar(uint64_t varId, SlangVar &slangVar) { varMap[varId] = slangVar; }

  bool isRecordPresent(uint64_t recordAddr) {
    return !(recordMap.find(recordAddr) == recordMap.end());
  }

  void addRecord(uint64_t recordAddr, SlangRecord slangRecord) {
    recordMap[recordAddr] = slangRecord;
  }

  SlangRecord &getRecord(uint64_t recordAddr) { return recordMap[recordAddr]; }

  int32_t getNextRecordId() {
    recordId += 1;
    return recordId;
  }

  std::string getNextRecordIdStr() {
    std::stringstream ss;
    ss << getNextRecordId();
    return ss.str();
  }

  // BOUND START: dirtyVars_logic

  void setDirtyVar(uint64_t varId, SlangExpr slangExpr) {
    // Clear the value for varId to an empty SlangExpr.
    // This forces the creation of a new tmp var,
    // whenever getTmpVarForDirtyVar() is called.
    dirtyVars[varId] = slangExpr;
  }

  // If this function is called dirtyVar dict should already have the entry.
  SlangExpr getTmpVarForDirtyVar(uint64_t varId) { return dirtyVars[varId]; }

  bool isDirtyVar(uint64_t varId) { return !(dirtyVars.find(varId) == dirtyVars.end()); }

  void clearDirtyVars() { dirtyVars.clear(); }

  // BOUND END  : dirtyVars_logic

  std::string convertFuncName(std::string funcName) {
    std::stringstream ss;
    ss << FUNC_NAME_PREFIX << funcName;
    return ss.str();
  }

  std::string convertVarExpr(uint64_t varAddr) {
    // if here, var should already be in varMap
    std::stringstream ss;

    auto slangVar = varMap[varAddr];
    ss << slangVar.name;

    return ss.str();
  }

  // BOUND START: dump_routines (to SPAN Strings)

  // dump entire span ir module for the translation unit.
  void dumpSlangIr() {
    std::stringstream ss;

    dumpHeader(ss);
    dumpVariables(ss);
    dumpObjs(ss);
    dumpFooter(ss);

    // TODO: print the content to a file.
    std::string fileName = this->fileName + ".spanir";
    Util::writeToFile(fileName, ss.str());
    llvm::errs() << ss.str();
  } // dumpSlangIr()

  void dumpHeader(std::stringstream &ss) {
    ss << "\n";
    ss << "# START: A_SPAN_translation_unit.\n";
    ss << "\n";
    ss << "# eval() the contents of this file.\n";
    ss << "# Keep the following imports in effect when calling eval.\n";
    ss << "\n";
    ss << "# import span.ir.types as types\n";
    ss << "# import span.ir.expr as expr\n";
    ss << "# import span.ir.instr as instr\n";
    ss << "# import span.ir.obj as obj\n";
    ss << "# import span.ir.tunit as irTUnit\n";
    ss << "# from span.ir.types import Loc\n";
    ss << "\n";
    ss << "# An instance of span.ir.tunit.TUnit class.\n";
    ss << "irTUnit.TUnit(\n";
    ss << NBSP2 << "name = \"" << fileName << "\",\n";
    ss << NBSP2 << "description = \"Auto-Translated from Clang AST.\",\n";
  } // dumpHeader()

  void dumpFooter(std::stringstream &ss) {
    ss << ") # irTUnit.TUnit() ends\n";
    ss << "\n# END  : A_SPAN_translation_unit.\n";
  } // dumpFooter()

  void dumpVariables(std::stringstream &ss) {
    ss << "\n";
    ss << NBSP2 << "allVars = {\n";
    for (const auto &var : varMap) {
      if (var.second.typeStr == DONT_PRINT)
        continue;
      ss << NBSP4;
      ss << "\"" << var.second.name << "\": " << var.second.typeStr << ",\n";
    }
    ss << NBSP2 << "}, # end allVars dict\n\n";
  } // dumpVariables()

  void dumpObjs(std::stringstream &ss) {
    ss << NBSP2 << "allObjs = {\n";
    dumpRecords(ss);
    dumpFunctions(ss);
    ss << NBSP2 << "}, # end allObjs dict\n";
  }

  void dumpRecords(std::stringstream &ss) {
    for (auto slangRecord : recordMap) {
      ss << NBSP4;
      ss << "\"" << slangRecord.second.name << "\":\n";
      ss << slangRecord.second.toString();
      ss << ",\n\n";
    }
    ss << "\n";
  }

  void dumpFunctions(std::stringstream &ss) {
    std::string prefix;
    for (auto slangFunc : funcMap) {
      ss << NBSP4; // indent
      ss << "\"" << slangFunc.second.fullName << "\":\n";
      ss << NBSP6 << "obj.Func(\n";

      // fields
      ss << NBSP8 << "name = "
         << "\"" << slangFunc.second.fullName << "\",\n";
      ss << NBSP8 << "paramNames = [";
      prefix = "";
      for (std::string &paramName : slangFunc.second.paramNames) {
        ss << prefix << "\"" << paramName << "\"";
        if (prefix.size() == 0) {
          prefix = ", ";
        }
      }
      ss << "],\n";
      ss << NBSP8 << "variadic = " << (slangFunc.second.variadic ? "True" : "False") << ",\n";

      ss << NBSP8 << "returnType = " << slangFunc.second.retType << ",\n";

      // field: basicBlocks
      ss << "\n";
      ss << NBSP8 << "# Note: -1 is always start/entry BB. (REQUIRED)\n";
      ss << NBSP8 << "# Note: 0 is always end/exit BB (REQUIRED)\n";
      ss << NBSP8 << "instrSeq = [\n";
      for (auto insn : slangFunc.second.spanStmts) {
        ss << NBSP12 << insn << ",\n";
      }
      ss << NBSP8 << "], # instrSeq end.\n";

      // close this function object
      ss << NBSP6 << "), # " << slangFunc.second.fullName << "() end. \n\n";
    }
  } // dumpFunctions()

  // BOUND END  : dump_routines (to SPAN Strings)

}; // class SlangTranslationUnit

class SlangGenAstChecker : public Checker<check::ASTCodeBody, check::EndOfTranslationUnit> {

  // static_members initialized
  static SlangTranslationUnit stu;
  static const FunctionDecl *FD; // funcDecl

public:
  // BOUND START: top_level_routines

  // mainentry, main entry point. Invokes top level Function and Cfg handlers.
  // It is invoked once for each source translation unit function.
  void checkASTCodeBody(const Decl *D, AnalysisManager &mgr, BugReporter &BR) const {
    SLANG_EVENT("BOUND START: SLANG_Generated_Output.\n")

    // SLANG_DEBUG("slang_add_nums: " << slang_add_nums(1,2) << "only\n"; // lib testing
    if (stu.fileName.size() == 0) {
      stu.fileName = D->getASTContext().getSourceManager().getFilename(D->getLocStart()).str();
    }

    FD = dyn_cast<FunctionDecl>(D);
    handleFuncNameAndType(FD);
    stu.currFunc = &stu.funcMap[(uint64_t)FD];
    SLANG_DEBUG(stu.currFunc->name)
    handleFunctionBody(FD);
  } // checkASTCodeBody()

  // invoked when the whole translation unit has been processed
  void checkEndOfTranslationUnit(const TranslationUnitDecl *TU, AnalysisManager &Mgr,
                                 BugReporter &BR) const {
    stu.dumpSlangIr();
    SLANG_EVENT("Translation Unit Ended.\n")
    SLANG_EVENT("BOUND END  : SLANG_Generated_Output.\n")
  } // checkEndOfTranslationUnit()

  // BOUND END  : top_level_routines

  // BOUND START: handling_routines

  void handleFunctionBody(const FunctionDecl *funcDecl) const {
    const Stmt *body = funcDecl->getBody();
    if (body) {
      convertStmt(body);
    } else {
      SLANG_ERROR("No body for function: " << funcDecl->getNameAsString())
    }
  }

  // records the function details
  void handleFuncNameAndType(const FunctionDecl *funcDecl) const {
    if (stu.funcMap.find((uint64_t)funcDecl) == stu.funcMap.end()) {
      // if here, function not already present. Add its details.
      SlangFunc slangFunc{};
      slangFunc.name = funcDecl->getNameInfo().getAsString();
      slangFunc.fullName = stu.convertFuncName(slangFunc.name);
      SLANG_DEBUG("AddingFunction: " << slangFunc.name)

      // STEP 1.2: Get function parameters.
      // if (funcDecl->doesThisDeclarationHaveABody()) { //& !funcDecl->hasPrototype())
      for (unsigned i = 0, e = funcDecl->getNumParams(); i != e; ++i) {
        const ParmVarDecl *paramVarDecl = funcDecl->getParamDecl(i);
        handleVariable(paramVarDecl, slangFunc.name); // adds the var too
        slangFunc.paramNames.push_back(stu.getVar((uint64_t)paramVarDecl).name);
      }
      slangFunc.variadic = funcDecl->isVariadic();

      // STEP 1.3: Get function return type.
      slangFunc.retType = convertClangType(funcDecl->getReturnType());

      // STEP 2: Copy the function to the map.
      stu.funcMap[(uint64_t)funcDecl] = slangFunc;
    }
  } // handleFunction()

  // record the variable name and type
  void handleVariable(const ValueDecl *valueDecl, std::string funcName) const {
    uint64_t varAddr = (uint64_t)valueDecl;
    std::string varName;
    if (stu.isNewVar(varAddr)) {
      // seeing the variable for the first time.
      SlangVar slangVar{};
      slangVar.id = varAddr;
      const VarDecl *varDecl = dyn_cast<VarDecl>(valueDecl);

      if (varDecl) {
        varName = valueDecl->getNameAsString();
        if (varName == "") {
          // used only to name anonymous function parameters
          varName = "p." + Util::getNextUniqueIdStr();
        }

        if (varDecl->hasLocalStorage()) {
          slangVar.setLocalVarName(varName, funcName);
        } else if (varDecl->hasGlobalStorage()) {
          slangVar.setGlobalVarName(varName);
        } else if (varDecl->hasExternalStorage()) {
          SLANG_ERROR("External Storage Not Handled.")
        } else {
          SLANG_ERROR("Unknown variable storage.")
        }

        // check if it has a initialization body
        if (varDecl->hasInit()) {
          // yes it has, so initialize it
          SlangExpr slangExpr = convertStmt(varDecl->getInit());
          std::string locStr = getLocationString(valueDecl);
          std::stringstream ss;
          ss << "instr.AssignI(";
          ss << "expr.VarE(\"" << slangVar.name << "\"";
          ss << ", " << locStr << ")"; // close expr.VarE(...
          ss << ", " << slangExpr.expr;
          ss << ", " << locStr << ")"; // close instr.AssignI(...
          stu.addStmt(ss.str());
        }
      } else {
        SLANG_ERROR("ValueDecl not a VarDecl!")
      }

      slangVar.typeStr = convertClangType(valueDecl->getType());
      stu.addVar(slangVar.id, slangVar);
      SLANG_DEBUG("NEW_VAR: " << slangVar.convertToString())

    } else {
      SLANG_DEBUG("SEEN_VAR: " << stu.getVar(varAddr).convertToString())
    }
  } // handleVariable()

  void handleDeclStmt(const DeclStmt *declStmt) const {
    // assumes there is only single decl inside (the likely case).
    stu.setLastDeclStmtTo(declStmt);
    SLANG_DEBUG("Set last DeclStmt to DeclStmt at " << (uint64_t)(declStmt));

    std::stringstream ss;
    std::string locStr = getLocationString(declStmt);

    for (auto it = declStmt->decl_begin(); it != declStmt->decl_end(); ++it) {
      if (isa<VarDecl>(*it)) {
        handleVariable(cast<ValueDecl>(*it), stu.currFunc->name);
      }
    }
  } // handleDeclStmt()

  // BOUND END  : handling_routines

  // BOUND START: conversion_routines

  // stmtconversion
  SlangExpr convertStmt(const Stmt *stmt) const {
    SlangExpr slangExpr;

    SLANG_DEBUG("ConvertingStmt : " << stmt->getStmtClassName() << "\n")
    stmt->dump();

    if (!stmt) {
      return slangExpr;
    }

    switch (stmt->getStmtClass()) {
    case Stmt::LabelStmtClass:
      return convertLabel(cast<LabelStmt>(stmt));

    case Stmt::IfStmtClass:
      return convertIfStmt(cast<IfStmt>(stmt));

    case Stmt::WhileStmtClass:
      return convertWhileStmt(cast<WhileStmt>(stmt));

    case Stmt::UnaryOperatorClass:
      return convertUnaryOperator(cast<UnaryOperator>(stmt));

    case Stmt::BinaryOperatorClass:
      return convertBinaryOperator(cast<BinaryOperator>(stmt));

    case Stmt::CompoundStmtClass:
      return convertCompoundStmt(cast<CompoundStmt>(stmt));

    case Stmt::DeclStmtClass:
      handleDeclStmt(cast<DeclStmt>(stmt));
      break;

    case Stmt::DeclRefExprClass:
      return convertDeclRefExpr(cast<DeclRefExpr>(stmt));

    case Stmt::IntegerLiteralClass:
      return convertIntegerLiteral(cast<IntegerLiteral>(stmt));

    case Stmt::FloatingLiteralClass:
      return convertFloatingLiteral(cast<FloatingLiteral>(stmt));

    case Stmt::StringLiteralClass:
      return convertStringLiteral(cast<StringLiteral>(stmt));

    case Stmt::ImplicitCastExprClass:
      return convertImplicitCastExpr(cast<ImplicitCastExpr>(stmt));

    default:
      SLANG_ERROR("Unhandled_Stmt: " << stmt->getStmtClassName())
    }

    return slangExpr;
  } // convertStmt()

  SlangExpr convertIfStmt(const IfStmt *ifStmt) const {
    std::stringstream ss;

    ss << "StartOfCondition : " << stu.nextStartConditionLabelCount();
    std::string startConditionLabel = ss.str();
    ss.str("");

    ss << "True : " << stu.nextTrueLabelCount();
    std::string trueLabel = ss.str();
    ss.str("");

    ss << "False : " << stu.nextFalseLabelCount();
    std::string falseLabel = ss.str();
    ss.str("");

    ss << "EndOfCondition : " << stu.nextEndConditionLabelCount();
    std::string endConditionLabel = ss.str();
    ss.str("");

    stu.addStmt(LABEL_PREFIX + startConditionLabel + LABEL_SUFFIX);

    const Stmt *condition = ifStmt->getCond();
    SlangExpr conditionExpr = convertStmt(condition);
    std::string locStr = getLocationString(condition);
    SlangExpr tempExpr = genTmpVariable(conditionExpr.qualType, locStr);

    ss << "instr.AssignI(" << tempExpr.expr << ", " << conditionExpr.expr << ")";
    stu.addStmt(ss.str());
    ss.str("");

    ss << "instr.CondI(" << tempExpr.expr << ", \"" << trueLabel << "\""
       << ", "
       << "\"" << falseLabel
       << "\""
          ")";
    stu.addStmt(ss.str());
    ss.str("");

    stu.addStmt(LABEL_PREFIX + trueLabel + LABEL_SUFFIX);
    const Stmt *thenBody = ifStmt->getThen();
    if (thenBody)
      convertStmt(thenBody);

    stu.addStmt(LABEL_PREFIX + falseLabel + LABEL_SUFFIX);
    const Stmt *elseBody = ifStmt->getElse();
    if (elseBody)
      convertStmt(elseBody);

    stu.addStmt(LABEL_PREFIX + endConditionLabel + LABEL_SUFFIX);
    return SlangExpr{}; // return empty expression

  } // convertIfStmt()

  SlangExpr convertWhileStmt(const WhileStmt *whileStmt) const {
    std::stringstream ss;

    ss << "StartOfCondition : " << stu.nextStartConditionLabelCount();
    std::string startConditionLabel = ss.str();
    ss.str("");

    ss << "True : " << stu.nextTrueLabelCount();
    std::string trueLabel = ss.str();
    ss.str("");

    ss << "False : " << stu.nextFalseLabelCount();
    std::string falseLabel = ss.str();
    ss.str("");

    ss << "EndOfCondition : " << stu.nextEndConditionLabelCount();
    std::string endConditionLabel = ss.str();
    ss.str("");

    stu.addStmt(LABEL_PREFIX + startConditionLabel + LABEL_SUFFIX);

    const Stmt *condition = whileStmt->getCond();
    SlangExpr conditionExpr = convertStmt(condition);
    std::string locStr = getLocationString(condition);
    SlangExpr tempExpr = genTmpVariable(conditionExpr.qualType, locStr);

    ss << "instr.AssignI(" << tempExpr.expr << ", " << conditionExpr.expr << ")";
    stu.addStmt(ss.str());
    ss.str("");

    ss << "instr.CondI(" << tempExpr.expr << ", \"" << trueLabel << "\""
       << ", "
       << "\"" << falseLabel
       << "\""
          ")";
    stu.addStmt(ss.str());
    ss.str("");

    stu.addStmt(LABEL_PREFIX + trueLabel + LABEL_SUFFIX);
    const Stmt *thenBody = whileStmt->getBody();
    if (thenBody)
      convertStmt(thenBody);

    // while has no else block but we keep the label for appropriate jumps
    stu.addStmt(LABEL_PREFIX + falseLabel + LABEL_SUFFIX);
    stu.addStmt(LABEL_PREFIX + endConditionLabel + LABEL_SUFFIX);
    return SlangExpr{}; // return empty expression

  } // convertWhileStmt()

  SlangExpr convertImplicitCastExpr(const ImplicitCastExpr *stmt) const {
    // only one child is expected
    auto it = stmt->child_begin();
    return convertStmt(*it);
  }

  SlangExpr convertIntegerLiteral(const IntegerLiteral *il) const {
    std::stringstream ss;
    std::string suffix = ""; // helps make int appear float

    std::string locStr = getLocationString(il);

    // check if int is implicitly casted to floating
    const auto &parents = FD->getASTContext().getParents(*il);
    if (!parents.empty()) {
      const Stmt *stmt1 = parents[0].get<Stmt>();
      if (stmt1) {
        switch (stmt1->getStmtClass()) {
        default:
          break;
        case Stmt::ImplicitCastExprClass: {
          const ImplicitCastExpr *ice = cast<ImplicitCastExpr>(stmt1);
          switch (ice->getCastKind()) {
          default:
            break;
          case CastKind::CK_IntegralToFloating:
            suffix = ".0";
            break;
          }
        }
        }
      }
    }

    bool is_signed = il->getType()->isSignedIntegerType();
    ss << "expr.LitE(" << il->getValue().toString(10, is_signed);
    ss << suffix;
    ss << ", " << locStr << ")";
    SLANG_TRACE(ss.str())

    SlangExpr slangExpr;
    slangExpr.expr = ss.str();
    slangExpr.qualType = il->getType();

    return slangExpr;
  } // convertIntegerLiteral()

  SlangExpr convertFloatingLiteral(const FloatingLiteral *fl) const {
    std::stringstream ss;
    bool toInt = false;

    std::string locStr = getLocationString(fl);

    // check if float is implicitly casted to int
    const auto &parents = FD->getASTContext().getParents(*fl);
    if (!parents.empty()) {
      const Stmt *stmt1 = parents[0].get<Stmt>();
      if (stmt1) {
        switch (stmt1->getStmtClass()) {
        default:
          break;
        case Stmt::ImplicitCastExprClass: {
          const ImplicitCastExpr *ice = cast<ImplicitCastExpr>(stmt1);
          switch (ice->getCastKind()) {
          default:
            break;
          case CastKind::CK_FloatingToIntegral:
            toInt = true;
            break;
          }
        }
        }
      }
    }

    ss << "expr.LitE(";
    if (toInt) {
      ss << (int64_t)fl->getValue().convertToDouble();
    } else {
      ss << std::fixed << fl->getValue().convertToDouble();
    }
    ss << ", " << locStr << ")";
    SLANG_TRACE(ss.str())

    SlangExpr slangExpr;
    slangExpr.expr = ss.str();
    slangExpr.qualType = fl->getType();

    return slangExpr;
  } // convertFloatingLiteral()

  SlangExpr convertStringLiteral(const StringLiteral *sl) const {
    SlangExpr slangExpr;
    std::stringstream ss;

    std::string locStr = getLocationString(sl);

    ss << "expr.LitE(\"\"\"" << sl->getBytes().str() << "\"\"\"";
    ss << ", " << locStr << ")";
    slangExpr.expr = ss.str();
    slangExpr.locStr = locStr;

    return slangExpr;
  } // convertStringLiteral()

  SlangExpr convertVarDecl(const VarDecl *varDecl, std::string &locStr) const {
    std::stringstream ss;
    SlangExpr slangExpr;

    ss << "expr.VarE(\"" << stu.convertVarExpr((uint64_t)varDecl) << "\"";
    ss << ", " << locStr << ")";
    slangExpr.expr = ss.str();
    slangExpr.qualType = varDecl->getType();
    slangExpr.varId = (uint64_t)varDecl;

    return slangExpr;
  } // convertVarDecl()

  SlangExpr convertEnumConst(const EnumConstantDecl *ecd, std::string &locStr) const {
    SlangExpr slangExpr;

    std::stringstream ss;
    ss << "expr.LitE(" << (ecd->getInitVal()).toString(10);
    ss << ", " << locStr << ")";

    slangExpr.expr = ss.str();
    slangExpr.locStr = locStr;
    slangExpr.qualType = ecd->getType();

    return slangExpr;
  }

  SlangExpr convertDeclRefExpr(const DeclRefExpr *dre) const {
    SlangExpr slangExpr;
    std::stringstream ss;

    std::string locStr = getLocationString(dre);

    const ValueDecl *valueDecl = dre->getDecl();
    if (isa<VarDecl>(valueDecl)) {
      auto varDecl = cast<VarDecl>(valueDecl);
      slangExpr = convertVarDecl(varDecl, locStr);
      slangExpr.locStr = getLocationString(dre);
      return slangExpr;

    } else if (isa<EnumConstantDecl>(valueDecl)) {
      auto ecd = cast<EnumConstantDecl>(valueDecl);
      return convertEnumConst(ecd, locStr);

    } else if (isa<FunctionDecl>(valueDecl)) {
      auto funcDecl = cast<FunctionDecl>(valueDecl);
      std::string funcName = funcDecl->getNameInfo().getAsString();
      ss << "expr.FuncE(\"" << stu.convertFuncName(funcName) << "\"";
      ss << ", " << locStr << ")";
      slangExpr.expr = ss.str();
      slangExpr.qualType = funcDecl->getType();
      slangExpr.locStr = locStr;
      return slangExpr;

    } else {
      SLANG_ERROR("Not_a_VarDecl.")
      slangExpr.expr = "ERROR:convertDeclRefExpr";
      return slangExpr;
    }
  } // convertDeclRefExpr()

  SlangExpr convertUnaryOperator(const UnaryOperator *unOp) const {
    SlangExpr slangExpr;

    std::string locStr = getLocationString(unOp);

    return slangExpr;
  } // convertUnaryOperator()

  SlangExpr convertBinaryOperator(const BinaryOperator *binOp) const {
    SlangExpr slangExpr;

    std::string locStr = getLocationString(binOp);

    if (binOp->isAssignmentOp()) {
      return convertAssignmentOp(binOp);
    } else if (binOp->isCompoundAssignmentOp()) {
      return slangExpr;
      // return convertCompoundAssignmentOp(binOp);
    }

    std::string op;
    switch (binOp->getOpcode()) {
    case BO_Add:
      op = "op.BO_ADD";
      break;

    case BO_Sub:
      op = "op.BO_SUB";
      break;

    case BO_Mul:
      op = "op.BO_MUL";
      break;

    case BO_Div:
      op = "op.BO_DIV";
      break;

    case BO_Rem:
      op = "op.BO_MOD";
      break;

    case BO_LT:
      op = "op.BO_LT";
      break;

    case BO_LE:
      op = "op.BO_LE";
      break;

    case BO_EQ:
      op = "op.BO_EQ";
      break;

    case BO_NE:
      op = "op.BO_NE";
      break;

    case BO_GE:
      op = "op.BO_GE";
      break;

    case BO_GT:
      op = "op.BO_GT";
      break;

    case BO_Or:
      op = "op.BO_BIT_OR";
      break;
    case BO_And:
      op = "op.BO_BIT_AND";
      break;

    case BO_Xor:
      op = "op.BO_BIT_XOR";
      break;

    case BO_LOr:
      op = "op.BO_LOGICAL_OR";
      break;

    case BO_LAnd:
      op = "op.BO_LOGICAL_AND";
      break;

    default:
      op = "ERROR:binOp";
      break;
    }

    auto it = binOp->child_begin();
    const Stmt *leftOprStmt = *it;
    ++it;
    const Stmt *rightOprStmt = *it;

    SlangExpr rightOprExpr = convertStmt(rightOprStmt);
    SlangExpr leftOprExpr = convertStmt(leftOprStmt);

    if (rightOprExpr.compound) {
      rightOprExpr = convertToTmpAssignment(rightOprExpr);
    }

    if (leftOprExpr.compound) {
      leftOprExpr = convertToTmpAssignment(leftOprExpr);
    }

    std::stringstream ss;
    ss << "expr.BinaryE(" << leftOprExpr.expr;
    ss << ", " << op;
    ss << ", " << rightOprExpr.expr;
    ss << ", " << locStr << ")"; // close expr.BinaryE(...

    slangExpr.expr = ss.str();
    slangExpr.qualType = binOp->getType();
    slangExpr.locStr = locStr;
    slangExpr.compound = true;

    return slangExpr;
  } // convertBinaryOperator()

  // stores the given expression into a tmp variable
  SlangExpr convertToTmpAssignment(const SlangExpr slangExpr) const {
    SlangExpr tmpExpr = genTmpVariable(slangExpr.qualType, slangExpr.locStr);
    std::stringstream ss;

    ss << "instr.AssignI(" << tmpExpr.expr << ", " << slangExpr.expr;
    ss << ", " << slangExpr.locStr << ")"; // close instr.AssignI(...
    stu.addStmt(ss.str());

    return tmpExpr;
  } // convertToTmpAssignment()

  SlangExpr convertAssignmentOp(const BinaryOperator *binOp) const {
    SlangExpr lhsExpr, rhsExpr;
    std::stringstream ss;

    std::string locStr = getLocationString(binOp);

    auto it = binOp->child_begin();
    const Stmt *lhs = *it;
    const Stmt *rhs = *(++it);

    rhsExpr = convertStmt(rhs);
    lhsExpr = convertStmt(lhs);

    if (lhsExpr.compound && rhsExpr.compound) {
      rhsExpr = convertToTmpAssignment(rhsExpr);
    }

    ss << "instr.AssignI(" << lhsExpr.expr << ", " << rhsExpr.expr;
    ss << ", " << locStr << ")"; // close instr.AssignI(...
    stu.addStmt(ss.str());

    return lhsExpr;
  } // convertAssignmentOp()

  SlangExpr convertCompoundStmt(const CompoundStmt *compoundStmt) const {
    SlangExpr slangExpr;

    for (auto it = compoundStmt->body_begin(); it != compoundStmt->body_end(); ++it) {
      // don't care about the return value
      convertStmt(*it);
    }

    return slangExpr;
  } // convertCompoundStmt()

  SlangExpr convertLabel(const LabelStmt *labelStmt) const {
    SlangExpr slangExpr;
    std::stringstream ss;

    std::string locStr = getLocationString(labelStmt);

    ss << "instr.LabelI(\"" << labelStmt->getName() << "\"";
    ss << ", " << locStr << ")"; // close instr.LabelI(...
    stu.addStmt(ss.str());

    for (auto it = labelStmt->child_begin(); it != labelStmt->child_end(); ++it) {
      convertStmt(*it);
    }

    return slangExpr;
  } // convertLabel()

  // BOUND START: type_conversion_routines

  // converts clang type to span ir types
  std::string convertClangType(QualType qt) const {
    std::stringstream ss;

    if (qt.isNull()) {
      return "types.Int32"; // the default type
    }

    qt = getCleanedQualType(qt);

    const Type *type = qt.getTypePtr();

    if (type->isBuiltinType()) {
      return convertClangBuiltinType(qt);

    } else if (type->isEnumeralType()) {
      ss << "types.Int32";

    } else if (type->isFunctionPointerType()) {
      // should be before ->isPointerType() check below
      return convertFunctionPointerType(qt);

    } else if (type->isPointerType()) {
      ss << "types.Ptr(to=";
      QualType pqt = type->getPointeeType();
      ss << convertClangType(pqt);
      ss << ")";

    } else if (type->isRecordType()) {
      if (type->isStructureType()) {
        return convertClangRecordType(qt.getTypePtr()->getAsStructureType()->getDecl());
      } else if (type->isUnionType()) {
        return convertClangRecordType(qt.getTypePtr()->getAsUnionType()->getDecl());
      } else {
        ss << "ERROR:RecordType";
      }

    } else if (type->isArrayType()) {
      return convertClangArrayType(qt);

    } else {
      ss << "UnknownType.";
    }

    return ss.str();
  } // convertClangType()

  std::string convertClangBuiltinType(QualType qt) const {
    std::stringstream ss;

    const Type *type = qt.getTypePtr();

    if (type->isSignedIntegerType()) {
      if (type->isCharType()) {
        ss << "types.Int8";
      } else if (type->isChar16Type()) {
        ss << "types.Int16";
      } else if (type->isIntegerType()) {
        ss << "types.Int32";
      } else {
        ss << "UnknownSignedIntType.";
      }

    } else if (type->isUnsignedIntegerType()) {
      if (type->isCharType()) {
        ss << "types.UInt8";
      } else if (type->isChar16Type()) {
        ss << "types.UInt16";
      } else if (type->isIntegerType()) {
        ss << "types.UInt32";
      } else {
        ss << "UnknownUnsignedIntType.";
      }

    } else if (type->isFloatingType()) {
      ss << "types.Float32";
    } else if (type->isRealFloatingType()) {
      ss << "types.Float64"; // FIXME: is realfloat a double?

    } else if (type->isVoidType()) {
      ss << "types.Void";
    } else {
      ss << "UnknownBuiltinType.";
    }

    return ss.str();
  } // convertClangBuiltinType()

  std::string convertClangRecordType(const RecordDecl *recordDecl) const {
    // a hack1 for anonymous decls (it works!) see test 000193.c and its AST!!
    static const RecordDecl *lastAnonymousRecordDecl = nullptr;

    if (recordDecl == nullptr) {
      // default to the last anonymous record decl
      return convertClangRecordType(lastAnonymousRecordDecl);
    }

    if (stu.isRecordPresent((uint64_t)recordDecl)) {
      return stu.getRecord((uint64_t)recordDecl).toShortString();
    }

    std::string namePrefix;
    SlangRecord slangRecord;

    if (recordDecl->isStruct()) {
      namePrefix = "s:";
      slangRecord.recordKind = Struct;
    } else if (recordDecl->isUnion()) {
      namePrefix = "u:";
      slangRecord.recordKind = Union;
    }

    if (recordDecl->getNameAsString() == "") {
      slangRecord.anonymous = true;
      slangRecord.name = namePrefix + stu.getNextRecordIdStr();
    } else {
      slangRecord.anonymous = false;
      slangRecord.name = namePrefix + recordDecl->getNameAsString();
    }

    slangRecord.locStr = getLocationString(recordDecl);

    stu.addRecord((uint64_t)recordDecl, slangRecord);                  // IMPORTANT
    SlangRecord &newSlangRecord = stu.getRecord((uint64_t)recordDecl); // IMPORTANT

    SlangRecordField slangRecordField;

    for (auto it = recordDecl->decls_begin(); it != recordDecl->decls_end(); ++it) {
      (*it)->dump();
      if (isa<RecordDecl>(*it)) {
        convertClangRecordType(cast<RecordDecl>(*it));
      } else if (isa<FieldDecl>(*it)) {
        const FieldDecl *fieldDecl = cast<FieldDecl>(*it);

        slangRecordField.clear();

        if (fieldDecl->getNameAsString() == "") {
          slangRecordField.name = newSlangRecord.getNextAnonymousFieldIdStr() + "a";
          slangRecordField.anonymous = true;
        } else {
          slangRecordField.name = fieldDecl->getNameAsString();
          slangRecordField.anonymous = false;
        }

        slangRecordField.type = fieldDecl->getType();
        if (slangRecordField.anonymous) {
          auto slangVar = SlangVar((uint64_t)fieldDecl, slangRecordField.name);
          stu.addVar((uint64_t)fieldDecl, slangVar);
          slangRecordField.typeStr = convertClangRecordType(nullptr);
        } else {
          slangRecordField.typeStr = convertClangType(slangRecordField.type);
        }

        newSlangRecord.fields.push_back(slangRecordField);
      }
    }

    // store for later use (part-of-hack1))
    lastAnonymousRecordDecl = recordDecl;

    // no need to add newSlangRecord, its a reference to its entry in the stu.recordMap
    return newSlangRecord.toShortString();
  } // convertClangRecordType()

  std::string convertClangArrayType(QualType qt) const {
    std::stringstream ss;

    const Type *type = qt.getTypePtr();
    const ArrayType *arrayType = type->getAsArrayTypeUnsafe();

    if (isa<ConstantArrayType>(arrayType)) {
      ss << "types.ConstSizeArray(of=";
      ss << convertClangType(arrayType->getElementType());
      ss << ", ";
      auto constArrType = cast<ConstantArrayType>(arrayType);
      ss << "size=" << constArrType->getSize().toString(10, true);
      ss << ")";

    } else if (isa<VariableArrayType>(arrayType)) {
      ss << "types.VarArray(of=";
      ss << convertClangType(arrayType->getElementType());
      ss << ")";

    } else if (isa<IncompleteArrayType>(arrayType)) {
      ss << "types.IncompleteArray(of=";
      ss << convertClangType(arrayType->getElementType());
      ss << ")";

    } else {
      ss << "UnknownArrayType";
    }

    SLANG_DEBUG(ss.str())
    return ss.str();
  } // convertClangArrayType()

  std::string convertFunctionPointerType(QualType qt) const {
    std::stringstream ss;

    const Type *type = qt.getTypePtr();

    ss << "types.Ptr(to=";
    const Type *funcType = type->getPointeeType().getTypePtr();
    funcType = funcType->getUnqualifiedDesugaredType();
    if (isa<FunctionProtoType>(funcType)) {
      auto funcProtoType = cast<FunctionProtoType>(funcType);
      ss << "types.FuncSig(returnType=";
      ss << convertClangType(funcProtoType->getReturnType());
      ss << ", "
         << "paramTypes=[";
      std::string prefix = "";
      for (auto qType : funcProtoType->getParamTypes()) {
        ss << prefix << convertClangType(qType);
        if (prefix == "")
          prefix = ", ";
      }
      ss << "]";
      if (funcProtoType->isVariadic()) {
        ss << ", variadic=True";
      }
      ss << ")"; // close types.FuncSig(...
      ss << ")"; // close types.Ptr(...

    } else if (isa<FunctionNoProtoType>(funcType)) {
      ss << "types.FuncSig(returnType=types.Int32)";
      ss << ")"; // close types.Ptr(...

    } else if (isa<FunctionType>(funcType)) {
      ss << "FuncType";

    } else {
      ss << "UnknownFunctionPtrType";
    }

    return ss.str();
  } // convertFunctionPointerType()

  // BOUND END  : type_conversion_routines
  // BOUND END  : conversion_routines

  // BOUND START: helper_routines

  SlangExpr genTmpVariable(QualType qt, std::string locStr) const {
    std::stringstream ss;
    SlangExpr slangExpr{};

    // STEP 1: Populate a SlangVar object with unique name.
    SlangVar slangVar{};
    slangVar.id = stu.nextTmpId();
    ss << "t." << slangVar.id;
    slangVar.setLocalVarName(ss.str(), stu.getCurrFuncName());
    slangVar.typeStr = convertClangType(qt);

    // STEP 2: Add to the var map.
    // FIXME: The var's 'id' here should be small enough to not interfere with uint64_t addresses.
    stu.addVar(slangVar.id, slangVar);

    // STEP 3: generate var expression.
    ss.str(""); // empty the stream
    ss << "expr.VarE(\"" << slangVar.name << "\"";
    ss << ", " << locStr << ")";

    slangExpr.expr = ss.str();
    slangExpr.locStr = locStr;
    slangExpr.qualType = qt;
    slangExpr.nonTmpVar = false;

    return slangExpr;
  } // genTmpVariable()

  std::string getLocationString(const Stmt *stmt) const {
    std::stringstream ss;
    uint32_t line = 0;
    uint32_t col = 0;

    ss << "Loc(";
    line = FD->getASTContext().getSourceManager().getExpansionLineNumber(stmt->getLocStart());
    ss << line << ",";
    col = FD->getASTContext().getSourceManager().getExpansionColumnNumber(stmt->getLocStart());
    ss << col << ")";

    return ss.str();
  }

  std::string getLocationString(const RecordDecl *recordDecl) const {
    std::stringstream ss;
    uint32_t line = 0;
    uint32_t col = 0;

    ss << "Loc(";
    line = FD->getASTContext().getSourceManager().getExpansionLineNumber(recordDecl->getLocStart());
    ss << line << ",";
    col =
        FD->getASTContext().getSourceManager().getExpansionColumnNumber(recordDecl->getLocStart());
    ss << col << ")";

    return ss.str();
  }

  std::string getLocationString(const ValueDecl *valueDecl) const {
    std::stringstream ss;
    uint32_t line = 0;
    uint32_t col = 0;

    ss << "Loc(";
    line = FD->getASTContext().getSourceManager().getExpansionLineNumber(valueDecl->getLocStart());
    ss << line << ",";
    col = FD->getASTContext().getSourceManager().getExpansionColumnNumber(valueDecl->getLocStart());
    ss << col << ")";

    return ss.str();
  }

  // Remove qualifiers and typedefs
  QualType getCleanedQualType(QualType qt) const {
    if (qt.isNull())
      return qt;
    qt = qt.getCanonicalType();
    qt.removeLocalConst();
    qt.removeLocalRestrict();
    qt.removeLocalVolatile();
    return qt;
  }

  // BOUND END  : helper_routines
};
} // anonymous namespace

// static_members initialized
SlangTranslationUnit SlangGenAstChecker::stu = SlangTranslationUnit();
const FunctionDecl *SlangGenAstChecker::FD = nullptr;

// Register the Checker
void ento::registerSlangGenAstChecker(CheckerManager &mgr) {
  mgr.registerChecker<SlangGenAstChecker>();
}

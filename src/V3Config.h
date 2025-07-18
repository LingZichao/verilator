// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Configuration Files
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2010-2025 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#ifndef VERILATOR_V3CONFIG_H_
#define VERILATOR_V3CONFIG_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3Ast.h"
#include "V3Error.h"
#include "V3FileLine.h"
#include "V3Mutex.h"

//######################################################################

class V3Config final {
public:
    static void addCaseFull(const string& file, int lineno);
    static void addCaseParallel(const string& file, int lineno);
    static void addCoverageBlockOff(const string& file, int lineno);
    static void addCoverageBlockOff(const string& module, const string& blockname);
    static void addHierWorkers(FileLine* fl, const string& model, int workers);
    static void addIgnore(V3ErrorCode code, bool on, const string& filename, int min, int max);
    static void addIgnoreMatch(V3ErrorCode code, const string& filename, const string& contents,
                               const string& match);
    static void addInline(FileLine* fl, const string& module, const string& ftask, bool on);
    static void addModulePragma(const string& module, VPragmaType pragma);
    static void addProfileData(FileLine* fl, const string& hierDpi, uint64_t cost);
    static void addProfileData(FileLine* fl, const string& model, const string& key,
                               uint64_t cost);
    static void addScopeTraceOn(bool on, const string& scope, int levels);
    static void addVarAttr(FileLine* fl, const string& module, const string& ftask,
                           const string& signal, VAttrType type, AstSenTree* nodep);

    static void applyCase(AstCase* nodep);
    static void applyCoverageBlock(AstNodeModule* modulep, AstBegin* nodep);
    static void applyFTask(AstNodeModule* modulep, AstNodeFTask* ftaskp);
    static void applyIgnores(FileLine* filelinep);
    static void applyModule(AstNodeModule* modulep);
    static void applyVarAttr(AstNodeModule* modulep, AstNodeFTask* ftaskp, AstVar* varp);

    static int getHierWorkers(const string& model);
    static FileLine* getHierWorkersFileLine(const string& model);
    static uint64_t getProfileData(const string& hierDpi);
    static uint64_t getProfileData(const string& model, const string& key);
    static FileLine* getProfileDataFileLine();
    static bool getScopeTraceOn(const string& scope);

    static void contentsPushText(const string& text);

    static bool containsMTaskProfileData();

    static bool waive(FileLine* filelinep, V3ErrorCode code, const string& message);
};

#endif  // Guard

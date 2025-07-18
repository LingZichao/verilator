// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Hierarchical Verilation for large designs
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2025 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
// Hierarchical Verilation is useful for large designs.
// It reduces
//   - time and memory for Verilation
//   - compilation time especially when a hierarchical block is used many times
//
// Hierarchical Verilation internally uses --lib-create for each
// hierarchical block.  Upper modules read the wrapper from --lib-create
// instead of the Verilog design.
//
// Hierarchical Verilation runs as the following step
// 1) Find modules marked by /*verilator hier_block*/ metacomment
// 2) Generate ${prefix}_hier.mk to create protected-lib for hierarchical blocks and
//    final Verilation to process the top module, that refers wrappers
// 3) Call child Verilator process via ${prefix}_hier.mk
//
// There are 3 kinds of Verilator run.
// a) To create ${prefix}_hier.mk (--hierarchical)
// b) To --lib-create on each hierarchical block (--hierarchical-child)
// c) To load wrappers and Verilate the top module (... what primary flags?)
//
// Then user can build Verilated module as usual.
//
// Here is more detailed internal process.
// 1) Parser adds VPragmaType::HIER_BLOCK of AstPragma to modules
//    that are marked with /*verilator hier_block*/ metacomment in Verilator run a).
// 2) If module type parameters are present, V3Config marks hier param modules
// (marked with hier_params verilator config pragma) as modp->hierParams(true).
// This is done in run b), de-parametrized modules are mapped with their params one-to-one.
// 3) AstModule with HIER_BLOCK pragma is marked modp->hierBlock(true)
//    in V3LinkResolve.cpp during run a).
// 4) In V3LinkCells.cpp, the following things are done during run b) and c).
//    4-1) Delete the upper modules of the hierarchical block because the top module in run b) is
//         hierarchical block, not the top module of run c).
//    4-2) If the top module of the run b) or c) instantiates other hierarchical blocks that is
//         parameterized,
//         module and task names are renamed to the original name to be compatible with the
//         hier module to be called.
//
//         Parameterized modules have unique name by V3Param.cpp. The unique name contains '__' and
//         Verilator encodes '__' when loading such symbols.
// 5) In V3LinkDot.cpp,
//    5-1) Dotted access across hierarchical block boundary is checked. Currently hierarchical
//    block references are not supported.
//    5-2) If present, parameters in hier params module replace parameter values of de-parametrized
//    module in run b).
// 6) In V3Dead.cpp, some parameters of parameterized modules are protected not to be deleted even
//    if the parameter is not referred. This protection is necessary to match step 6) below.
// 7) In V3Param.cpp, use --lib-create wrapper of the parameterized module made in b) and c).
//    If a hierarchical block is a parameterized module and instantiated in multiple locations,
//    all parameters must exactly match.
// 8) In V3HierBlock.cpp, relationships among hierarchical blocks are checked in run a).
//    (which block uses other blocks..)
// 9) In V3EmitMk.cpp, ${prefix}_hier.mk is created in run a).
//
// There are three hidden command options:
//   --hierarchical-child is added to Verilator run b).
//   --hierarchical-block module_name,mangled_name,name0,value0,name1,value1,...
//       module_name  :The original modulename
//       mangled_name :Mangled name of parameterized modules (named in V3Param.cpp).
//                     Same as module_name for non-parameterized hierarchical block.
//       name         :The name of the parameter
//       value        :Overridden value of the parameter
//
//       Used for b) and c).
//       These options are repeated for all instantiated hierarchical blocks.
//   --hierarchical-params-file filename
//      filename    :Name of a hierarchical parameters file
//
//      Added in a), used for b).
//      Each de-parametrized module version has exactly one hier params file specified.

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3HierBlock.h"

#include "V3Config.h"
#include "V3EmitV.h"
#include "V3File.h"
#include "V3Os.h"
#include "V3Stats.h"
#include "V3String.h"

#include <memory>
#include <sstream>
#include <utility>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

static string V3HierCommandArgsFilename(const string& prefix, bool forCMake) {
    return v3Global.opt.makeDir() + "/" + prefix
           + (forCMake ? "__hierCMakeArgs.f" : "__hierMkArgs.f");
}

static string V3HierParametersFileName(const string& prefix) {
    return v3Global.opt.makeDir() + "/" + prefix + "__hierParameters.v";
}

static void V3HierWriteCommonInputs(const V3HierBlock* hblockp, std::ostream* of, bool forCMake) {
    string topModuleFile;
    if (hblockp) topModuleFile = hblockp->vFileIfNecessary();
    if (!forCMake) {
        if (!topModuleFile.empty()) *of << topModuleFile << "\n";
        const V3StringList& vFiles = v3Global.opt.vFiles();
        for (const string& i : vFiles) *of << i << "\n";
    }
    const V3StringSet& libraryFiles = v3Global.opt.libraryFiles();
    for (const string& i : libraryFiles) {
        if (V3Os::filenameRealPath(i) != topModuleFile) *of << "-v " << i << "\n";
    }
}

//######################################################################

V3HierBlock::StrGParams V3HierBlock::stringifyParams(const V3HierBlockParams::GParams& gparams,
                                                     bool forGOption) {
    StrGParams strParams;
    for (const auto& gparam : gparams) {
        if (const AstConst* const constp = VN_CAST(gparam->valuep(), Const)) {
            string s;
            // Only constant parameter needs to be set to -G because already checked in
            // V3Param.cpp. See also ParamVisitor::checkSupportedParam() in the file.
            if (constp->isDouble()) {
                // 64 bit width of hex can be expressed with 16 chars.
                // 32 chars must be long enough for hexadecimal floating point
                // considering prefix of '0x', '.', and 'P'.
                std::vector<char> hexFpStr(32, '\0');
                const int len = VL_SNPRINTF(hexFpStr.data(), hexFpStr.size(), "%a",
                                            constp->num().toDouble());
                UASSERT_OBJ(0 < len && static_cast<size_t>(len) < hexFpStr.size(), constp,
                            " is not properly converted to string");
                s = hexFpStr.data();
            } else if (constp->isString()) {
                s = constp->num().toString();
                if (!forGOption) s = VString::quoteBackslash(s);
                s = VString::quoteStringLiteralForShell(s);
            } else {  // Either signed or unsigned integer.
                s = constp->num().ascii(true, true);
                s = VString::quoteAny(s, '\'', '\\');
            }
            strParams.emplace_back(gparam->name(), s);
        }
    }
    return strParams;
}

V3HierBlock::~V3HierBlock() {
    UASSERT(m_children.empty(), "at least one module must be a leaf");
    for (const auto& hierblockp : m_children) {
        const bool deleted = hierblockp->m_children.erase(this);
        UASSERT_OBJ(deleted, m_modp, " is not registered");
    }
}

V3StringList V3HierBlock::commandArgs(bool forCMake) const {
    V3StringList opts;
    const string prefix = hierPrefix();
    if (!forCMake) {
        opts.push_back(" --prefix " + prefix);
        opts.push_back(" --mod-prefix " + prefix);
        opts.push_back(" --top-module " + modp()->name());
    }
    opts.push_back(" --lib-create " + modp()->name());  // possibly mangled name
    if (v3Global.opt.protectKeyProvided())
        opts.push_back(" --protect-key " + v3Global.opt.protectKeyDefaulted());
    opts.push_back(" --hierarchical-child " + cvtToStr(v3Global.opt.threads()));

    const StrGParams gparamsStr = stringifyParams(params().gparams(), true);
    for (const StrGParam& param : gparamsStr) {
        const string name = param.first;
        const string value = param.second;
        opts.push_back("-G" + name + "=" + value + "");
    }
    if (!params().gTypeParams().empty())
        opts.push_back(" --hierarchical-params-file " + typeParametersFilename());

    const int blockThreads = V3Config::getHierWorkers(m_modp->origName());
    if (blockThreads > 1) {
        if (hasParent()) {
            V3Config::getHierWorkersFileLine(m_modp->origName())
                ->v3warn(E_UNSUPPORTED, "Specifying workers for nested hierarchical blocks");
        } else {
            if (v3Global.opt.threads() < blockThreads) {
                m_modp->v3error("Hierarchical blocks cannot be scheduled on more threads than in "
                                "thread pool, threads = "
                                << v3Global.opt.threads()
                                << " hierarchical block threads = " << blockThreads);
            }

            opts.push_back(" --threads " + std::to_string(blockThreads));
        }
    }

    return opts;
}

V3StringList V3HierBlock::hierBlockArgs() const {
    V3StringList opts;
    const StrGParams gparamsStr = stringifyParams(params().gparams(), false);
    opts.push_back("--hierarchical-block ");
    string s = modp()->origName();  // origName
    s += "," + modp()->name();  // mangledName
    for (StrGParams::const_iterator paramIt = gparamsStr.begin(); paramIt != gparamsStr.end();
         ++paramIt) {
        s += "," + paramIt->first;
        s += "," + paramIt->second;
    }
    opts.back() += s;
    return opts;
}

string V3HierBlock::hierPrefix() const { return "V" + modp()->name(); }

string V3HierBlock::hierSomeFilename(bool withDir, const char* prefix, const char* suffix) const {
    string s;
    if (withDir) s = hierPrefix() + '/';
    s += prefix + modp()->name() + suffix;
    return s;
}

string V3HierBlock::hierWrapperFilename(bool withDir) const {
    return hierSomeFilename(withDir, "", ".sv");
}

string V3HierBlock::hierMkFilename(bool withDir) const {
    return hierSomeFilename(withDir, "V", ".mk");
}

string V3HierBlock::hierLibFilename(bool withDir) const {
    return hierSomeFilename(withDir, "lib", ".a");
}

string V3HierBlock::hierGeneratedFilenames(bool withDir) const {
    return hierWrapperFilename(withDir) + ' ' + hierMkFilename(withDir);
}

string V3HierBlock::vFileIfNecessary() const {
    string filename = V3Os::filenameRealPath(m_modp->fileline()->filename());
    for (const string& v : v3Global.opt.vFiles()) {
        // Already listed in vFiles, so no need to add the file.
        if (filename == V3Os::filenameRealPath(v)) return "";
    }
    return filename;
}

void V3HierBlock::writeCommandArgsFile(bool forCMake) const {
    const std::unique_ptr<std::ofstream> of{V3File::new_ofstream(commandArgsFilename(forCMake))};
    *of << "--cc\n";

    if (!forCMake) {
        for (const auto& hierblockp : m_children) {
            *of << v3Global.opt.makeDir() << "/" << hierblockp->hierWrapperFilename(true) << "\n";
        }
        *of << "-Mdir " << v3Global.opt.makeDir() << "/" << hierPrefix() << " \n";
    }
    V3HierWriteCommonInputs(this, of.get(), forCMake);
    const V3StringList& commandOpts = commandArgs(false);
    for (const string& opt : commandOpts) *of << opt << "\n";
    *of << hierBlockArgs().front() << "\n";
    for (const auto& hierblockp : m_children) *of << hierblockp->hierBlockArgs().front() << "\n";
    *of << v3Global.opt.allArgsStringForHierBlock(false) << "\n";
}

string V3HierBlock::commandArgsFilename(bool forCMake) const {
    return V3HierCommandArgsFilename(hierPrefix(), forCMake);
}

string V3HierBlock::typeParametersFilename() const {
    return V3HierParametersFileName(hierPrefix());
}

void V3HierBlock::writeParametersFile() const {
    if (m_params.gTypeParams().empty()) return;

    VHashSha256 hash{"type params"};
    const string moduleName = "Vhsh" + hash.digestSymbol();
    const std::unique_ptr<std::ofstream> of{V3File::new_ofstream(typeParametersFilename())};
    *of << "module " << moduleName << ";\n";
    for (AstParamTypeDType* const gparam : m_params.gTypeParams()) {
        AstTypedef* tdefp
            = new AstTypedef(new FileLine{FileLine::builtInFilename()}, gparam->name(), nullptr,
                             VFlagChildDType{}, gparam->skipRefp()->cloneTreePure(true));
        V3EmitV::verilogForTree(tdefp, *of);
        VL_DO_DANGLING(tdefp->deleteTree(), tdefp);
    }
    *of << "endmodule\n\n";
    *of << "`verilator_config\n";
    *of << "hier_params -module \"" << moduleName << "\"\n";
}

//######################################################################
// Collect how hierarchical blocks are used
class HierBlockUsageCollectVisitor final : public VNVisitorConst {
    // NODE STATE
    // AstNode::user1()            -> bool. Processed
    const VNUser1InUse m_inuser1;

    // STATE
    using ModuleSet = std::unordered_set<const AstModule*>;
    V3HierBlockPlan* const m_planp;
    AstModule* m_modp = nullptr;  // The current module
    AstModule* m_hierBlockp = nullptr;  // The nearest parent module that is a hierarchical block
    ModuleSet m_referred;  // Modules that have hier_block pragma
    V3HierBlockParams m_params;

    void visit(AstModule* nodep) override {
        // Don't visit twice
        if (nodep->user1SetOnce()) return;
        UINFO(5, "Checking " << nodep->prettyNameQ() << " from "
                             << (m_hierBlockp ? m_hierBlockp->prettyNameQ() : "null"s));
        VL_RESTORER(m_modp);
        AstModule* const prevHierBlockp = m_hierBlockp;
        ModuleSet prevReferred;
        V3HierBlockParams previousParams;
        m_modp = nodep;
        if (nodep->hierBlock()) {
            m_hierBlockp = nodep;
            prevReferred.swap(m_referred);
        }
        previousParams.swap(m_params);

        iterateChildrenConst(nodep);

        if (nodep->hierBlock()) {
            m_planp->add(nodep, m_params);
            for (const AstModule* modp : m_referred) m_planp->registerUsage(nodep, modp);
            m_hierBlockp = prevHierBlockp;
            m_referred = prevReferred;
        }
        m_params.swap(previousParams);
    }
    void visit(AstCell* nodep) override {
        // Visit used module here to know that the module is hier_block or not.
        // This visitor behaves almost depth first search
        if (AstModule* const modp = VN_CAST(nodep->modp(), Module)) {
            iterateConst(modp);
            m_referred.insert(modp);
        }
        // Nothing to do for interface because hierarchical block does not exist
        // beyond interface.
    }
    void visit(AstVar* nodep) override {
        if (m_modp && m_modp->hierBlock() && nodep->isIfaceRef() && !nodep->isIfaceParent()) {
            nodep->v3error("Modport cannot be used at the hierarchical block boundary");
        }
        if (nodep->isGParam() && nodep->overriddenParam()) m_params.add(nodep);
    }

    void visit(AstParamTypeDType* nodep) override { m_params.add(nodep); }

    void visit(AstNodeExpr*) override {}  // Accelerate
    void visit(AstConstPool*) override {}  // Accelerate
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    HierBlockUsageCollectVisitor(V3HierBlockPlan* planp, AstNetlist* netlist)
        : m_planp{planp} {
        iterateChildrenConst(netlist);
    }
};

//######################################################################

void V3HierBlockPlan::add(const AstNodeModule* modp, const V3HierBlockParams& params) {
    const auto pair = m_blocks.emplace(modp, nullptr);
    if (pair.second) {
        V3HierBlock* hblockp = new V3HierBlock{modp, params};
        UINFO(3, "Add " << modp->prettyNameQ() << " with " << params.gparams().size()
                        << " parameters and " << params.gTypeParams().size()
                        << " type parameters");
        pair.first->second = hblockp;
    }
}

void V3HierBlockPlan::registerUsage(const AstNodeModule* parentp, const AstNodeModule* childp) {
    const iterator parent = m_blocks.find(parentp);
    UASSERT_OBJ(parent != m_blocks.end(), parentp, "must be added");
    const iterator child = m_blocks.find(childp);
    if (child != m_blocks.end()) {
        UINFO(3, "Found usage relation " << parentp->prettyNameQ() << " uses "
                                         << childp->prettyNameQ());
        parent->second->addChild(child->second);
        child->second->addParent(parent->second);
    }
}

void V3HierBlockPlan::createPlan(AstNetlist* nodep) {
    // When processing a hierarchical block, no need to create a plan anymore.
    if (v3Global.opt.hierChild()) return;

    AstNodeModule* const modp = nodep->topModulep();
    if (modp->hierBlock()) {
        modp->v3warn(HIERBLOCK,
                     "Top module illegally marked hierarchical block, ignoring marking\n"
                         + modp->warnMore()
                         + "... Suggest remove verilator hier_block on this module");
        modp->hierBlock(false);
    }

    std::unique_ptr<V3HierBlockPlan> planp(new V3HierBlockPlan);
    { HierBlockUsageCollectVisitor{planp.get(), nodep}; }

    V3Stats::addStat("HierBlock, Hierarchical blocks", planp->m_blocks.size());

    // No hierarchical block is found, nothing to do.
    if (planp->empty()) return;

    v3Global.hierPlanp(planp.release());
}

V3HierBlockPlan::HierVector V3HierBlockPlan::hierBlocksSorted() const {
    using ChildrenMap
        = std::unordered_map<const V3HierBlock*, std::unordered_set<const V3HierBlock*>>;
    ChildrenMap childrenOfHierBlock;

    HierVector sorted;
    for (const_iterator it = begin(); it != end(); ++it) {
        if (!it->second->hasChild()) {  // No children, already leaf
            sorted.push_back(it->second);
        } else {
            ChildrenMap::value_type::second_type& childrenSet
                = childrenOfHierBlock[it->second];  // insert
            const V3HierBlock::HierBlockSet& c = it->second->children();
            childrenSet.insert(c.begin(), c.end());
        }
    }

    // Use index instead of iterator because new elements will be added in this loop
    for (size_t i = 0; i < sorted.size(); ++i) {
        // This hblockp is already leaf.
        const V3HierBlock* hblockp = sorted[i];
        const V3HierBlock::HierBlockSet& p = hblockp->parents();
        for (V3HierBlock::HierBlockSet::const_iterator it = p.begin(); it != p.end(); ++it) {
            // Delete hblockp from parents. If a parent does not have a child anymore, then it is
            // a leaf too.
            const auto parentIt = childrenOfHierBlock.find(*it);
            UASSERT_OBJ(parentIt != childrenOfHierBlock.end(), (*it)->modp(), "must be included");
            const V3HierBlock::HierBlockSet::size_type erased = parentIt->second.erase(hblockp);
            UASSERT_OBJ(erased == 1, hblockp->modp(),
                        " must be a child of " << parentIt->first->modp());
            if (parentIt->second.empty()) {  // Now parentIt is leaf
                sorted.push_back(parentIt->first);
                childrenOfHierBlock.erase(parentIt);
            }
        }
    }
    return sorted;
}

void V3HierBlockPlan::writeCommandArgsFiles(bool forCMake) const {
    for (const_iterator it = begin(); it != end(); ++it) {
        it->second->writeCommandArgsFile(forCMake);
    }
    // For the top module
    const std::unique_ptr<std::ofstream> of{
        V3File::new_ofstream(topCommandArgsFilename(forCMake))};
    if (!forCMake) {
        // Load wrappers first not to be overwritten by the original HDL
        for (const_iterator it = begin(); it != end(); ++it) {
            *of << it->second->hierWrapperFilename(true) << "\n";
        }
    }
    V3HierWriteCommonInputs(nullptr, of.get(), forCMake);
    if (!forCMake) {
        const V3StringSet& cppFiles = v3Global.opt.cppFiles();
        for (const string& i : cppFiles) *of << i << "\n";
        *of << "--top-module " << v3Global.rootp()->topModulep()->name() << "\n";
        *of << "--prefix " << v3Global.opt.prefix() << "\n";
        *of << "-Mdir " << v3Global.opt.makeDir() << "\n";
        *of << "--mod-prefix " << v3Global.opt.modPrefix() << "\n";
    }
    for (const_iterator it = begin(); it != end(); ++it) {
        *of << it->second->hierBlockArgs().front() << "\n";
    }

    if (!v3Global.opt.libCreate().empty()) {
        *of << "--lib-create " << v3Global.opt.libCreate() << "\n";
    }
    if (v3Global.opt.protectKeyProvided()) {
        *of << "--protect-key " << v3Global.opt.protectKeyDefaulted() << "\n";
    }
    *of << "--threads " << cvtToStr(v3Global.opt.threads()) << "\n";
    *of << (v3Global.opt.systemC() ? "--sc" : "--cc") << "\n";
    *of << v3Global.opt.allArgsStringForHierBlock(true) << "\n";
}

string V3HierBlockPlan::topCommandArgsFilename(bool forCMake) {
    return V3HierCommandArgsFilename(v3Global.opt.prefix(), forCMake);
}

void V3HierBlockPlan::writeParametersFiles() const {
    for (const auto& block : *this) block.second->writeParametersFile();
}

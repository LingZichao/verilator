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

#include "V3PchAstMT.h"

#include "V3Config.h"

#include "V3String.h"

#include <memory>
#include <set>
#include <unordered_map>

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################
// Resolve wildcards in files, modules, ftasks or variables

// Template for a class that serves as a map for entities that can be specified
// as wildcards and are accessed by a resolved name. It rebuilds a name lookup
// cache of resolved entities. Entities stored in this container need an update
// function that takes a reference of this type to join multiple entities into one.
template <typename T>
class V3ConfigWildcardResolver final {
    mutable V3Mutex m_mutex;  // protects members
    // Pattern strings (wildcard, or simple name) to entities
    std::map<const std::string, T> m_mapPatterns VL_GUARDED_BY(m_mutex);
    // Resolved strings to converged entities - nullptr, iff none of the patterns applies
    std::map<const std::string, std::unique_ptr<T>> m_mapResolved VL_GUARDED_BY(m_mutex);

public:
    V3ConfigWildcardResolver() = default;
    ~V3ConfigWildcardResolver() = default;

    /// Update into maps from other
    void update(const V3ConfigWildcardResolver& other) VL_MT_SAFE_EXCLUDES(m_mutex)
        VL_EXCLUDES(other.m_mutex) {
        V3LockGuard lock{m_mutex};
        V3LockGuard otherLock{other.m_mutex};
        // Clear the resolved cache, as 'other' might add new patterns that need to be applied as
        // well.
        m_mapResolved.clear();
        for (const auto& itr : other.m_mapPatterns) m_mapPatterns[itr.first].update(itr.second);
    }

    // Access and create a pattern entry
    T& at(const string& name) VL_MT_SAFE_EXCLUDES(m_mutex) {
        V3LockGuard lock{m_mutex};
        // We might be adding a new entry under this, so clear the cache.
        m_mapResolved.clear();
        return m_mapPatterns[name];
    }
    // Access an entity and resolve patterns that match it
    T* resolve(const string& name) VL_MT_SAFE_EXCLUDES(m_mutex) {
        V3LockGuard lock{m_mutex};
        // Lookup if it was resolved before, typically not
        const auto pair = m_mapResolved.emplace(name, nullptr);
        std::unique_ptr<T>& entryr = pair.first->second;
        // Resolve entry when first requested, cache the result
        if (pair.second) {
            // Update the entity with all matches in the patterns
            for (const auto& patEnt : m_mapPatterns) {
                if (VString::wildmatch(name, patEnt.first)) {
                    if (!entryr) entryr.reset(new T{});
                    entryr->update(patEnt.second);
                }
            }
        }
        return entryr.get();
    }
};

// Only public_flat_rw has the sensitity tree
class V3ConfigVarAttr final {
public:
    VAttrType m_type;  // Type of attribute
    AstSenTree* m_sentreep;  // Sensitivity tree for public_flat_rw
    explicit V3ConfigVarAttr(VAttrType type)
        : m_type{type}
        , m_sentreep{nullptr} {}
    V3ConfigVarAttr(VAttrType type, AstSenTree* sentreep)
        : m_type{type}
        , m_sentreep{sentreep} {}
};

// Overload vector with the required update function and to apply all entries
class V3ConfigVar final : public std::vector<V3ConfigVarAttr> {
public:
    // Update from other by copying all attributes
    void update(const V3ConfigVar& node) {
        reserve(size() + node.size());
        insert(end(), node.begin(), node.end());
    }
    // Apply all attributes to the variable
    void apply(AstVar* varp) {
        for (const_iterator it = begin(); it != end(); ++it) {
            AstNode* const newp = new AstAttrOf{varp->fileline(), it->m_type};
            varp->addAttrsp(newp);
            if (it->m_type == VAttrType::VAR_PUBLIC_FLAT_RW && it->m_sentreep) {
                newp->addNext(new AstAlwaysPublic{varp->fileline(), it->m_sentreep, nullptr});
            }
        }
    }
};

using V3ConfigVarResolver = V3ConfigWildcardResolver<V3ConfigVar>;

//======================================================================

class WildcardContents final {
    // Not mutex protected, current calling from V3Config::waive is protected by error's mutex
    // MEMBERS
    std::map<const std::string, bool> m_mapPatterns;  // Pattern match results
    std::deque<string> m_lines;  // Source text lines

    // METHODS
    static WildcardContents& s() {  // Singleton
        static WildcardContents s_s;
        return s_s;
    }
    void clearCacheImp() { m_mapPatterns.clear(); }
    void pushTextImp(const string& text) {
        // Similar code in VFileContent::pushText()
        // Any leftover text is stored on largest line (might be "")
        const string leftover = m_lines.back() + text;
        m_lines.pop_back();

        // Insert line-by-line
        string::size_type line_start = 0;
        while (true) {
            const string::size_type line_end = leftover.find('\n', line_start);
            if (line_end != string::npos) {
                const string oneline(leftover, line_start, line_end - line_start + 1);
                if (oneline.size() > 1) m_lines.push_back(oneline);  // Keeps newline
                UINFO(9, "Push[+" << (m_lines.size() - 1) << "]: " << oneline);
                line_start = line_end + 1;
            } else {
                break;
            }
        }
        // Keep leftover for next time
        m_lines.emplace_back(string(leftover, line_start));  // Might be ""
        clearCacheImp();
    }

    bool resolveUncachedImp(const string& name) {
        for (const string& i : m_lines) {
            if (VString::wildmatch(i, name)) return true;
        }
        return false;
    }
    bool resolveCachedImp(const string& name) {
        // Lookup if it was resolved before, typically is
        const auto pair = m_mapPatterns.emplace(name, false);
        bool& entryr = pair.first->second;
        // Resolve entry when first requested, cache the result
        if (pair.second) entryr = resolveUncachedImp(name);
        return entryr;
    }

public:
    WildcardContents() {
        m_lines.emplace_back("");  // start with no leftover
    }
    ~WildcardContents() = default;
    // Return true iff name in parsed contents
    static bool resolve(const string& name) { return s().resolveCachedImp(name); }
    // Add arbitrary text (need not be line-by-line)
    static void pushText(const string& text) { s().pushTextImp(text); }
};

//######################################################################
// Function or task: Have variables and properties

class V3ConfigFTask final {
    V3ConfigVarResolver m_vars;  // Variables in function/task
    bool m_isolate = false;  // Isolate function return
    bool m_noinline = false;  // Don't inline function/task
    bool m_public = false;  // Public function/task

public:
    V3ConfigFTask() = default;
    void update(const V3ConfigFTask& f) {
        // Don't overwrite true with false
        if (f.m_isolate) m_isolate = true;
        if (f.m_noinline) m_noinline = true;
        if (f.m_public) m_public = true;
        m_vars.update(f.m_vars);
    }

    V3ConfigVarResolver& vars() { return m_vars; }

    void setIsolate(bool set) { m_isolate = set; }
    void setNoInline(bool set) { m_noinline = set; }
    void setPublic(bool set) { m_public = set; }

    void apply(AstNodeFTask* ftaskp) const {
        if (m_noinline)
            ftaskp->addStmtsp(new AstPragma{ftaskp->fileline(), VPragmaType::NO_INLINE_TASK});
        if (m_public)
            ftaskp->addStmtsp(new AstPragma{ftaskp->fileline(), VPragmaType::PUBLIC_TASK});
        // Only functions can have isolate (return value)
        if (VN_IS(ftaskp, Func)) ftaskp->attrIsolateAssign(m_isolate);
    }
};

using V3ConfigFTaskResolver = V3ConfigWildcardResolver<V3ConfigFTask>;

//######################################################################
// Modules have tasks, variables, named blocks and properties

class V3ConfigModule final {
    V3ConfigFTaskResolver m_tasks;  // Functions/tasks in module
    V3ConfigVarResolver m_vars;  // Variables in module
    std::unordered_set<std::string> m_coverageOffBlocks;  // List of block names for coverage_off
    std::set<VPragmaType> m_modPragmas;  // List of Pragmas for modules
    bool m_inline = false;  // Whether to force the inline
    bool m_inlineValue = false;  // The inline value (on/off)

public:
    V3ConfigModule() = default;

    void update(const V3ConfigModule& m) {
        m_tasks.update(m.m_tasks);
        m_vars.update(m.m_vars);
        for (const string& i : m.m_coverageOffBlocks) m_coverageOffBlocks.insert(i);
        if (!m_inline) {
            m_inline = m.m_inline;
            m_inlineValue = m.m_inlineValue;
        }
        for (auto it = m.m_modPragmas.cbegin(); it != m.m_modPragmas.cend(); ++it) {
            m_modPragmas.insert(*it);
        }
    }

    V3ConfigFTaskResolver& ftasks() { return m_tasks; }
    V3ConfigVarResolver& vars() { return m_vars; }

    void addCoverageBlockOff(const string& name) { m_coverageOffBlocks.insert(name); }
    void setInline(bool set) {
        m_inline = true;
        m_inlineValue = set;
    }
    void addModulePragma(VPragmaType pragma) { m_modPragmas.insert(pragma); }

    void apply(AstNodeModule* modp) {
        if (m_inline) {
            const VPragmaType type
                = m_inlineValue ? VPragmaType::INLINE_MODULE : VPragmaType::NO_INLINE_MODULE;
            AstNode* const nodep = new AstPragma{modp->fileline(), type};
            modp->addStmtsp(nodep);
        }
        for (const auto& itr : m_modPragmas) {
            // Catch hier param modules to mark their attributes before they are
            // flagged dead in LinkDot.
            if (itr == VPragmaType::HIER_PARAMS) modp->hierParams(true);
            AstNode* const nodep = new AstPragma{modp->fileline(), itr};
            modp->addStmtsp(nodep);
        }
    }

    void applyBlock(AstNodeBlock* nodep) {
        const VPragmaType pragma = VPragmaType::COVERAGE_BLOCK_OFF;
        if (!nodep->unnamed()) {
            for (const string& i : m_coverageOffBlocks) {
                if (VString::wildmatch(nodep->name(), i)) {
                    nodep->addStmtsp(new AstPragma{nodep->fileline(), pragma});
                }
            }
        }
    }
};

using V3ConfigModuleResolver = V3ConfigWildcardResolver<V3ConfigModule>;

//######################################################################
// Files have:
//  - Line ignores (lint/coverage/tracing on/off)
//  - Line attributes: Attributes attached to lines

// lint/coverage/tracing on/off
class V3ConfigIgnoresLine final {
public:
    const int m_lineno;  // Line number to make change at
    const V3ErrorCode m_code;  // Error code
    const bool m_on;  // True to enable message
    V3ConfigIgnoresLine(V3ErrorCode code, int lineno, bool on)
        : m_lineno{lineno}
        , m_code{code}
        , m_on{on} {}
    ~V3ConfigIgnoresLine() = default;
    bool operator<(const V3ConfigIgnoresLine& rh) const {
        if (m_lineno < rh.m_lineno) return true;
        if (m_lineno > rh.m_lineno) return false;
        if (m_code < rh.m_code) return true;
        if (m_code > rh.m_code) return false;
        // Always turn "on" before "off" so that overlapping lines will end
        // up finally with the error "off"
        return (m_on > rh.m_on);
    }
};
std::ostream& operator<<(std::ostream& os, const V3ConfigIgnoresLine& rhs) {
    return os << rhs.m_lineno << ", " << rhs.m_code << ", " << rhs.m_on;
}

// Some attributes are attached to entities of the occur on a fileline
// and multiple attributes can be attached to a line
using V3ConfigLineAttribute = std::bitset<VPragmaType::ENUM_SIZE>;

class WaiverSetting final {
public:
    V3ErrorCode m_code;  // Error code
    string m_contents;  // --contents regexp
    string m_match;  // --match regexp
    WaiverSetting(V3ErrorCode code, const string& contents, const string& match)
        : m_code{code}
        , m_contents{contents}
        , m_match{match} {}
    ~WaiverSetting() = default;
    WaiverSetting& operator=(const WaiverSetting& rhs) {
        m_code = rhs.m_code;
        m_contents = rhs.m_contents;
        m_match = rhs.m_match;
        return *this;
    }
};

// File entity
class V3ConfigFile final {
    using LineAttrMap = std::map<int, V3ConfigLineAttribute>;  // Map line->bitset of attributes
    using IgnLines = std::multiset<V3ConfigIgnoresLine>;  // list of {line,code,on}
    using Waivers = std::vector<WaiverSetting>;  // List of {code,wildcard string}

    LineAttrMap m_lineAttrs;  // Attributes to line mapping
    IgnLines m_ignLines;  // Ignore line settings
    Waivers m_waivers;  // Waive messages

    struct {
        int lineno;  // Last line number
        IgnLines::const_iterator it;  // Point with next linenumber > current line number
    } m_lastIgnore;  // Last ignore line run

    // Match a given line and attribute to the map, line 0 is any
    bool lineMatch(int lineno, VPragmaType type) {
        if (m_lineAttrs.find(0) != m_lineAttrs.end() && m_lineAttrs[0][type]) return true;
        if (m_lineAttrs.find(lineno) == m_lineAttrs.end()) return false;
        return m_lineAttrs[lineno][type];
    }

public:
    V3ConfigFile() {
        m_lastIgnore.lineno = -1;
        m_lastIgnore.it = m_ignLines.begin();
    }
    void update(const V3ConfigFile& file) {
        // Copy in all Attributes
        for (const auto& itr : file.m_lineAttrs) m_lineAttrs[itr.first] |= itr.second;
        // Copy in all ignores
        for (const auto& ignLine : file.m_ignLines) m_ignLines.insert(ignLine);
        // Update the iterator after the list has changed
        m_lastIgnore.it = m_ignLines.begin();
        m_waivers.reserve(m_waivers.size() + file.m_waivers.size());
        m_waivers.insert(m_waivers.end(), file.m_waivers.begin(), file.m_waivers.end());
    }
    void addLineAttribute(int lineno, VPragmaType attr) { m_lineAttrs[lineno].set(attr); }
    void addIgnore(V3ErrorCode code, int lineno, bool on) {
        m_ignLines.insert(V3ConfigIgnoresLine{code, lineno, on});
        m_lastIgnore.it = m_ignLines.begin();
    }
    void addIgnoreMatch(V3ErrorCode code, const string& contents, const string& match) {
        // Since Verilator 5.031 the error message compared has context, so
        // allow old rules to still match using a final '*'
        string newMatch = match;
        if (newMatch.empty() || newMatch.back() != '*') newMatch += '*';
        m_waivers.emplace_back(WaiverSetting{code, contents, newMatch});
    }

    void applyBlock(AstNodeBlock* nodep) {
        // Apply to block at this line
        const VPragmaType pragma = VPragmaType::COVERAGE_BLOCK_OFF;
        if (lineMatch(nodep->fileline()->lineno(), pragma)) {
            nodep->addStmtsp(new AstPragma{nodep->fileline(), pragma});
        }
    }
    void applyCase(AstCase* nodep) {
        // Apply to this case at this line
        const int lineno = nodep->fileline()->lineno();
        if (lineMatch(lineno, VPragmaType::FULL_CASE)) nodep->fullPragma(true);
        if (lineMatch(lineno, VPragmaType::PARALLEL_CASE)) nodep->parallelPragma(true);
    }
    void applyIgnores(FileLine* filelinep) {
        // HOT routine, called each parsed token line of this filename
        if (m_lastIgnore.lineno != filelinep->lineno()) {
            // UINFO(9, "   ApplyIgnores for " << filelinep->ascii());
            // Process all on/offs for lines up to and including the current line
            const int curlineno = filelinep->lastLineno();
            for (; m_lastIgnore.it != m_ignLines.end(); ++m_lastIgnore.it) {
                if (m_lastIgnore.it->m_lineno > curlineno) break;
                // UINFO(9, "     Hit " << *m_lastIgnore.it);
                filelinep->warnOn(m_lastIgnore.it->m_code, m_lastIgnore.it->m_on);
            }
            if (false && debug() >= 9) {
                for (IgnLines::const_iterator it = m_lastIgnore.it; it != m_ignLines.end(); ++it) {
                    UINFO(9, "     NXT " << *it);
                }
            }
            m_lastIgnore.lineno = filelinep->lastLineno();
        }
    }
    bool waive(V3ErrorCode code, const string& match) {
        if (code.hardError()) return false;
        for (const auto& itr : m_waivers) {
            if ((code.isUnder(itr.m_code) || (itr.m_code == V3ErrorCode::I_LINT))
                && VString::wildmatch(match, itr.m_match)
                && WildcardContents::resolve(itr.m_contents)) {
                return true;
            }
        }
        return false;
    }
};

using V3ConfigFileResolver = V3ConfigWildcardResolver<V3ConfigFile>;

//######################################################################
// ScopeTrace tracking

class V3ConfigScopeTraceEntry final {
public:
    const string m_scope;  // Scope or regexp to match
    const bool m_on = false;  // True to enable message
    int m_levels = 0;  // # levels, 0 = all, 1 = only this, ...
    // CONSTRUCTORS
    V3ConfigScopeTraceEntry(const string& scope, bool on, int levels)
        : m_scope{scope}
        , m_on{on}
        , m_levels{levels} {}
    ~V3ConfigScopeTraceEntry() = default;
    bool operator<(const V3ConfigScopeTraceEntry& other) const {
        if (m_on < other.m_on) return true;
        if (m_on > other.m_on) return false;
        if (m_levels < other.m_levels) return true;
        if (m_levels > other.m_levels) return false;
        return m_scope < other.m_scope;
    }
};

// Tracks what matches are known to hit against V3ConfigScopeTraceEntries
class V3ConfigScopeTraceEntryMatch final {
public:
    const V3ConfigScopeTraceEntry* m_entryp;
    const string m_scopepart;
    V3ConfigScopeTraceEntryMatch(const V3ConfigScopeTraceEntry* entryp, const string& scopepart)
        : m_entryp{entryp}
        , m_scopepart{scopepart} {}
    bool operator<(const V3ConfigScopeTraceEntryMatch& other) const {
        if (m_entryp < other.m_entryp) return true;
        if (m_entryp > other.m_entryp) return false;
        return m_scopepart < other.m_scopepart;
    }
};

class V3ConfigScopeTraceResolver final {
    std::vector<V3ConfigScopeTraceEntry> m_entries;  // User specified on/offs and levels
    std::map<V3ConfigScopeTraceEntryMatch, bool> m_matchCache;  // Matching entries for speed

public:
    void addScopeTraceOn(bool on, const string& scope, int levels) {
        UINFO(9, "addScopeTraceOn " << on << " '" << scope << "' "
                                    << " levels=" << levels);
        m_entries.emplace_back(V3ConfigScopeTraceEntry{scope, on, levels});
        m_matchCache.clear();
    }

    bool getEntryMatch(const V3ConfigScopeTraceEntry* entp, const string& scopepart) {
        // Return if a entry matches the scopepart, with memoization
        const V3ConfigScopeTraceEntryMatch key{entp, scopepart};
        const auto pair = m_matchCache.emplace(key, false);
        if (pair.second) pair.first->second = VString::wildmatch(scopepart, entp->m_scope);
        return pair.first->second;
    }

    bool getScopeTraceOn(const string& scope) {
        // Apply in the order the user provided them, so they can choose on/off preferencing
        int maxLevel = 1;
        for (const auto& ch : scope) {
            if (ch == '.') ++maxLevel;
        }
        UINFO(9, "getScopeTraceOn " << scope << " maxLevel=" << maxLevel);

        bool enabled = true;
        for (const auto& ent : m_entries) {
            // We apply shortest match first for each rule component
            // (Otherwise the levels would be useless as "--scope top* --levels 1" would
            // always match at every scopepart, and we wouldn't know how to count levels)
            int partLevel = 1;
            for (string::size_type partEnd = 0; true;) {
                partEnd = scope.find('.', partEnd + 1);
                if (partEnd == string::npos) partEnd = scope.length();
                const std::string scopepart = scope.substr(0, partEnd);
                if (getEntryMatch(&ent, scopepart)) {
                    const bool levelMatch
                        = !ent.m_levels || (ent.m_levels >= maxLevel - partLevel);
                    if (levelMatch) enabled = ent.m_on;
                    UINFO(9, "getScopeTraceOn-part " << scope << " enabled=" << enabled
                                                     << " @ lev=" << partLevel
                                                     << (levelMatch ? "[match]" : "[miss]")
                                                     << " from scopepart=" << scopepart);
                    break;
                }
                if (partEnd == scope.length()) break;
                ++partLevel;
            }
        }
        return enabled;
    }
};

//######################################################################
// Resolve modules and files in the design

class V3ConfigResolverHierWorkerEntry final {
    const int m_workers;
    FileLine* const m_flp;

public:
    explicit V3ConfigResolverHierWorkerEntry(int workers, FileLine* flp)
        : m_workers{workers}
        , m_flp{flp} {}
    int workers() const { return m_workers; }
    FileLine* flp() const { return m_flp; }
};

class V3ConfigResolver final {
    enum ProfileDataMode : uint8_t { NONE = 0, MTASK = 1, HIER_DPI = 2 };
    V3ConfigModuleResolver m_modules;  // Access to module names (with wildcards)
    V3ConfigFileResolver m_files;  // Access to file names (with wildcards)
    V3ConfigScopeTraceResolver m_scopeTraces;  // Regexp to trace enables
    std::unordered_map<string, std::unordered_map<string, uint64_t>>
        m_profileData;  // Access to profile_data records
    uint8_t m_mode = NONE;
    std::unordered_map<string, V3ConfigResolverHierWorkerEntry> m_hierWorkers;
    FileLine* m_profileFileLine = nullptr;

    V3ConfigResolver() = default;
    ~V3ConfigResolver() = default;

public:
    static V3ConfigResolver& s() {
        static V3ConfigResolver s_singleton;
        return s_singleton;
    }
    V3ConfigModuleResolver& modules() { return m_modules; }
    V3ConfigFileResolver& files() { return m_files; }
    V3ConfigScopeTraceResolver& scopeTraces() { return m_scopeTraces; }

    void addProfileData(FileLine* fl, const string& hierDpi, uint64_t cost) {
        // Empty key for hierarchical DPI wrapper costs.
        addProfileData(fl, hierDpi, "", cost, HIER_DPI);
    }
    void addProfileData(FileLine* fl, const string& model, const string& key, uint64_t cost,
                        ProfileDataMode mode = MTASK) {
        if (!m_profileFileLine) m_profileFileLine = fl;
        if (cost == 0) cost = 1;  // Cost 0 means delete (or no data)
        m_profileData[model][key] += cost;
        m_mode |= mode;
    }
    bool containsMTaskProfileData() const { return m_mode & MTASK; }
    uint64_t getProfileData(const string& hierDpi) const {
        // Empty key for hierarchical DPI wrapper costs.
        return getProfileData(hierDpi, "");
    }
    void addHierWorkers(FileLine* flp, const string& model, int workers) {
        m_hierWorkers.emplace(std::piecewise_construct, std::forward_as_tuple(model),
                              std::forward_as_tuple(workers, flp));
    }
    int getHierWorkers(const string& model) const {
        const auto mit = m_hierWorkers.find(model);
        // Assign a single worker if no specified.
        return mit != m_hierWorkers.cend() ? mit->second.workers() : 0;
    }
    FileLine* getHierWorkersFileLine(const string& model) const {
        const auto mit = m_hierWorkers.find(model);
        return mit != m_hierWorkers.cend() ? mit->second.flp() : v3Global.rootp()->fileline();
    }
    uint64_t getProfileData(const string& model, const string& key) const {
        const auto mit = m_profileData.find(model);
        if (mit == m_profileData.cend()) return 0;
        const auto it = mit->second.find(key);
        if (it == mit->second.cend()) return 0;
        return it->second;
    }
    FileLine* getProfileDataFileLine() const { return m_profileFileLine; }  // Maybe null
};

//######################################################################
// V3Config

void V3Config::addCaseFull(const string& filename, int lineno) {
    V3ConfigFile& file = V3ConfigResolver::s().files().at(filename);
    file.addLineAttribute(lineno, VPragmaType::FULL_CASE);
}

void V3Config::addCaseParallel(const string& filename, int lineno) {
    V3ConfigFile& file = V3ConfigResolver::s().files().at(filename);
    file.addLineAttribute(lineno, VPragmaType::PARALLEL_CASE);
}

void V3Config::addCoverageBlockOff(const string& filename, int lineno) {
    V3ConfigFile& file = V3ConfigResolver::s().files().at(filename);
    file.addLineAttribute(lineno, VPragmaType::COVERAGE_BLOCK_OFF);
}

void V3Config::addCoverageBlockOff(const string& module, const string& blockname) {
    V3ConfigResolver::s().modules().at(module).addCoverageBlockOff(blockname);
}

void V3Config::addHierWorkers(FileLine* fl, const string& model, int workers) {
    V3ConfigResolver::s().addHierWorkers(fl, model, workers);
}

void V3Config::addIgnore(V3ErrorCode code, bool on, const string& filename, int min, int max) {
    if (filename == "*") {
        FileLine::globalWarnOff(code, !on);
    } else {
        V3ConfigResolver::s().files().at(filename).addIgnore(code, min, on);
        if (max) V3ConfigResolver::s().files().at(filename).addIgnore(code, max, !on);
    }
}

void V3Config::addIgnoreMatch(V3ErrorCode code, const string& filename, const string& contents,
                              const string& match) {
    V3ConfigResolver::s().files().at(filename).addIgnoreMatch(code, contents, match);
}

void V3Config::addInline(FileLine* fl, const string& module, const string& ftask, bool on) {
    if (ftask.empty()) {
        V3ConfigResolver::s().modules().at(module).setInline(on);
    } else {
        if (!on) {
            fl->v3error("Unsupported: no_inline for tasks");
        } else {
            V3ConfigResolver::s().modules().at(module).ftasks().at(ftask).setNoInline(on);
        }
    }
}

void V3Config::addModulePragma(const string& module, VPragmaType pragma) {
    V3ConfigResolver::s().modules().at(module).addModulePragma(pragma);
}

void V3Config::addProfileData(FileLine* fl, const string& hierDpi, uint64_t cost) {
    V3ConfigResolver::s().addProfileData(fl, hierDpi, cost);
}

void V3Config::addProfileData(FileLine* fl, const string& model, const string& key,
                              uint64_t cost) {
    V3ConfigResolver::s().addProfileData(fl, model, key, cost);
}

void V3Config::addScopeTraceOn(bool on, const string& scope, int levels) {
    V3ConfigResolver::s().scopeTraces().addScopeTraceOn(on, scope, levels);
}

void V3Config::addVarAttr(FileLine* fl, const string& module, const string& ftask,
                          const string& var, VAttrType attr, AstSenTree* sensep) {
    // Semantics: sensep only if public_flat_rw
    if ((attr != VAttrType::VAR_PUBLIC_FLAT_RW) && sensep) {
        sensep->v3error("sensitivity not expected for attribute");
        return;
    }
    // Semantics: Most of the attributes operate on signals
    if (var.empty()) {
        if (attr == VAttrType::VAR_ISOLATE_ASSIGNMENTS) {
            if (ftask.empty()) {
                fl->v3error("isolate_assignments only applies to signals or functions/tasks");
            } else {
                V3ConfigResolver::s().modules().at(module).ftasks().at(ftask).setIsolate(true);
            }
        } else if (attr == VAttrType::VAR_PUBLIC) {
            if (ftask.empty()) {
                // public module, this is the only exception from var here
                V3ConfigResolver::s().modules().at(module).addModulePragma(
                    VPragmaType::PUBLIC_MODULE);
            } else {
                V3ConfigResolver::s().modules().at(module).ftasks().at(ftask).setPublic(true);
            }
        } else {
            fl->v3error("missing -var");
        }
    } else {
        if (attr == VAttrType::VAR_FORCEABLE) {
            if (module.empty()) {
                fl->v3error("forceable missing -module");
            } else if (!ftask.empty()) {
                fl->v3error("Signals inside functions/tasks cannot be marked forceable");
            } else {
                V3ConfigResolver::s().modules().at(module).vars().at(var).push_back(
                    V3ConfigVarAttr{attr});
            }
        } else {
            V3ConfigModule& mod = V3ConfigResolver::s().modules().at(module);
            if (ftask.empty()) {
                mod.vars().at(var).push_back(V3ConfigVarAttr{attr, sensep});
            } else {
                mod.ftasks().at(ftask).vars().at(var).push_back(V3ConfigVarAttr{attr, sensep});
            }
        }
    }
}

void V3Config::applyCase(AstCase* nodep) {
    const string& filename = nodep->fileline()->filename();
    V3ConfigFile* filep = V3ConfigResolver::s().files().resolve(filename);
    if (filep) filep->applyCase(nodep);
}

void V3Config::applyCoverageBlock(AstNodeModule* modulep, AstBegin* nodep) {
    const string& filename = nodep->fileline()->filename();
    V3ConfigFile* filep = V3ConfigResolver::s().files().resolve(filename);
    if (filep) filep->applyBlock(nodep);
    const string& modname = modulep->name();
    V3ConfigModule* modp = V3ConfigResolver::s().modules().resolve(modname);
    if (modp) modp->applyBlock(nodep);
}

void V3Config::applyIgnores(FileLine* filelinep) {
    const string& filename = filelinep->filename();
    V3ConfigFile* filep = V3ConfigResolver::s().files().resolve(filename);
    if (filep) filep->applyIgnores(filelinep);
}

void V3Config::applyModule(AstNodeModule* modulep) {
    const string& modname = modulep->origName();
    V3ConfigModule* modp = V3ConfigResolver::s().modules().resolve(modname);
    if (modp) modp->apply(modulep);
}

void V3Config::applyFTask(AstNodeModule* modulep, AstNodeFTask* ftaskp) {
    const string& modname = modulep->name();
    V3ConfigModule* modp = V3ConfigResolver::s().modules().resolve(modname);
    if (!modp) return;
    const V3ConfigFTask* const ftp = modp->ftasks().resolve(ftaskp->name());
    if (ftp) ftp->apply(ftaskp);
}

void V3Config::applyVarAttr(AstNodeModule* modulep, AstNodeFTask* ftaskp, AstVar* varp) {
    V3ConfigVar* vp;
    V3ConfigModule* modp = V3ConfigResolver::s().modules().resolve(modulep->name());
    if (!modp) return;
    if (ftaskp) {
        V3ConfigFTask* ftp = modp->ftasks().resolve(ftaskp->name());
        if (!ftp) return;
        vp = ftp->vars().resolve(varp->name());
    } else {
        vp = modp->vars().resolve(varp->name());
    }
    if (vp) vp->apply(varp);
}

int V3Config::getHierWorkers(const string& model) {
    return V3ConfigResolver::s().getHierWorkers(model);
}
FileLine* V3Config::getHierWorkersFileLine(const string& model) {
    return V3ConfigResolver::s().getHierWorkersFileLine(model);
}
uint64_t V3Config::getProfileData(const string& hierDpi) {
    return V3ConfigResolver::s().getProfileData(hierDpi);
}
uint64_t V3Config::getProfileData(const string& model, const string& key) {
    return V3ConfigResolver::s().getProfileData(model, key);
}
FileLine* V3Config::getProfileDataFileLine() {
    return V3ConfigResolver::s().getProfileDataFileLine();
}
bool V3Config::getScopeTraceOn(const string& scope) {
    return V3ConfigResolver::s().scopeTraces().getScopeTraceOn(scope);
}

void V3Config::contentsPushText(const string& text) { return WildcardContents::pushText(text); }

bool V3Config::containsMTaskProfileData() {
    return V3ConfigResolver::s().containsMTaskProfileData();
}

bool V3Config::waive(FileLine* filelinep, V3ErrorCode code, const string& message) {
    V3ConfigFile* filep = V3ConfigResolver::s().files().resolve(filelinep->filename());
    if (!filep) return false;
    return filep->waive(code, message);
}

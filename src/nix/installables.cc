#include "command.hh"
#include "attr-path.hh"
#include "common-eval-args.hh"
#include "derivations.hh"
#include "eval-inline.hh"
#include "eval.hh"
#include "get-drvs.hh"
#include "store-api.hh"
#include "shared.hh"
#include "primops/flake.hh"

#include <regex>

namespace nix {

SourceExprCommand::SourceExprCommand()
{
    mkFlag()
        .shortName('f')
        .longName("file")
        .label("file")
        .description("evaluate FILE rather than use the default installation source")
        .dest(&file);
}

Value * SourceExprCommand::getSourceExpr(EvalState & state)
{
    if (vSourceExpr) return vSourceExpr;

    vSourceExpr = state.allocValue();

    if (file)
        state.evalFile(lookupFileArg(state, *file), *vSourceExpr);
    else {
        // FIXME: remove "impure" hack, call some non-user-accessible
        // variant of getFlake instead.
        auto fun = state.parseExprFromString(
             "builtins.mapAttrs (flakeName: flakeInfo:"
             "  (getFlake (\"impure:\" + flakeInfo.uri)).${flakeName}.provides.packages or {})", "/");
        auto vFun = state.allocValue();
        state.eval(fun, *vFun);
        auto vRegistry = makeFlakeRegistryValue(state);
        mkApp(*vSourceExpr, *vFun, *vRegistry);
    }

    return vSourceExpr;
}

ref<EvalState> SourceExprCommand::getEvalState()
{
    if (!evalState)
        evalState = std::make_shared<EvalState>(searchPath, getStore());
    return ref<EvalState>(evalState);
}

Buildable Installable::toBuildable()
{
    auto buildables = toBuildables();
    if (buildables.size() != 1)
        throw Error("installable '%s' evaluates to %d derivations, where only one is expected", what(), buildables.size());
    return std::move(buildables[0]);
}

struct InstallableStorePath : Installable
{
    Path storePath;

    InstallableStorePath(const Path & storePath) : storePath(storePath) { }

    std::string what() override { return storePath; }

    Buildables toBuildables() override
    {
        return {{isDerivation(storePath) ? storePath : "", {{"out", storePath}}}};
    }
};

struct InstallableValue : Installable
{
    SourceExprCommand & cmd;

    InstallableValue(SourceExprCommand & cmd) : cmd(cmd) { }

    Buildables toBuildables() override
    {
        auto state = cmd.getEvalState();

        auto v = toValue(*state);

        Bindings & autoArgs = *cmd.getAutoArgs(*state);

        DrvInfos drvs;
        getDerivations(*state, *v, "", autoArgs, drvs, false);

        Buildables res;

        PathSet drvPaths;

        for (auto & drv : drvs) {
            Buildable b{drv.queryDrvPath()};
            drvPaths.insert(b.drvPath);

            auto outputName = drv.queryOutputName();
            if (outputName == "")
                throw Error("derivation '%s' lacks an 'outputName' attribute", b.drvPath);

            b.outputs.emplace(outputName, drv.queryOutPath());

            res.push_back(std::move(b));
        }

        // Hack to recognize .all: if all drvs have the same drvPath,
        // merge the buildables.
        if (drvPaths.size() == 1) {
            Buildable b{*drvPaths.begin()};
            for (auto & b2 : res)
                b.outputs.insert(b2.outputs.begin(), b2.outputs.end());
            return {b};
        } else
            return res;
    }
};

struct InstallableExpr : InstallableValue
{
    std::string text;

    InstallableExpr(SourceExprCommand & cmd, const std::string & text)
         : InstallableValue(cmd), text(text) { }

    std::string what() override { return text; }

    Value * toValue(EvalState & state) override
    {
        auto v = state.allocValue();
        state.eval(state.parseExprFromString(text, absPath(".")), *v);
        return v;
    }
};

struct InstallableAttrPath : InstallableValue
{
    std::string attrPath;

    InstallableAttrPath(SourceExprCommand & cmd, const std::string & attrPath)
        : InstallableValue(cmd), attrPath(attrPath)
    { }

    std::string what() override { return attrPath; }

    Value * toValue(EvalState & state) override
    {
        auto source = cmd.getSourceExpr(state);

        Bindings & autoArgs = *cmd.getAutoArgs(state);

        Value * v = findAlongAttrPath(state, attrPath, autoArgs, *source);
        state.forceValue(*v);

        return v;
    }
};

struct InstallableFlake : InstallableValue
{
    FlakeRef flakeRef;
    std::string attrPath;

    InstallableFlake(SourceExprCommand & cmd, FlakeRef && flakeRef, const std::string & attrPath)
        : InstallableValue(cmd), flakeRef(flakeRef), attrPath(attrPath)
    { }

    std::string what() override { return flakeRef.to_string() + ":" + attrPath; }

    Value * toValue(EvalState & state) override
    {
        auto vTemp = state.allocValue();
        auto vFlake = *makeFlakeValue(state, flakeRef, true, *vTemp);

        auto vProvides = (*vFlake.attrs->get(state.symbols.create("provides")))->value;

        state.forceValue(*vProvides);

        auto emptyArgs = state.allocBindings(0);

        if (auto aPackages = *vProvides->attrs->get(state.symbols.create("packages"))) {
            try {
                auto * v = findAlongAttrPath(state, attrPath, *emptyArgs, *aPackages->value);
                state.forceValue(*v);
                return v;
            } catch (AttrPathNotFound & e) {
            }
        }

        auto * v = findAlongAttrPath(state, attrPath, *emptyArgs, *vProvides);
        state.forceValue(*v);
        return v;
    }
};

// FIXME: extend
std::string attrRegex = R"([A-Za-z_][A-Za-z0-9-_+]*)";
static std::regex attrPathRegex(fmt(R"(%1%(\.%1%)*)", attrRegex));

static std::vector<std::shared_ptr<Installable>> parseInstallables(
    SourceExprCommand & cmd, ref<Store> store, std::vector<std::string> ss, bool useDefaultInstallables)
{
    std::vector<std::shared_ptr<Installable>> result;

    if (ss.empty() && useDefaultInstallables) {
        if (cmd.file == "")
            cmd.file = ".";
        ss = {""};
    }

    for (auto & s : ss) {

        if (s.compare(0, 1, "(") == 0)
            result.push_back(std::make_shared<InstallableExpr>(cmd, s));

        /*
        else if (s.find('/') != std::string::npos) {

            auto path = store->toStorePath(store->followLinksToStore(s));

            if (store->isStorePath(path))
                result.push_back(std::make_shared<InstallableStorePath>(path));
        }
        */

        else {
            auto colon = s.rfind(':');
            if (colon != std::string::npos) {
                auto flakeRef = std::string(s, 0, colon);
                auto attrPath = std::string(s, colon + 1);
                result.push_back(std::make_shared<InstallableFlake>(cmd, FlakeRef(flakeRef), attrPath));
            } else {
                result.push_back(std::make_shared<InstallableFlake>(cmd, FlakeRef("nixpkgs"), s));
            }
        }

        /*
        else if (s == "" || std::regex_match(s, attrPathRegex))
            result.push_back(std::make_shared<InstallableAttrPath>(cmd, s));

        else
            throw UsageError("don't know what to do with argument '%s'", s);
        */
    }

    return result;
}

std::shared_ptr<Installable> parseInstallable(
    SourceExprCommand & cmd, ref<Store> store, const std::string & installable,
    bool useDefaultInstallables)
{
    auto installables = parseInstallables(cmd, store, {installable}, false);
    assert(installables.size() == 1);
    return installables.front();
}

Buildables build(ref<Store> store, RealiseMode mode,
    std::vector<std::shared_ptr<Installable>> installables)
{
    if (mode != Build)
        settings.readOnlyMode = true;

    Buildables buildables;

    PathSet pathsToBuild;

    for (auto & i : installables) {
        for (auto & b : i->toBuildables()) {
            if (b.drvPath != "") {
                StringSet outputNames;
                for (auto & output : b.outputs)
                    outputNames.insert(output.first);
                pathsToBuild.insert(
                    b.drvPath + "!" + concatStringsSep(",", outputNames));
            } else
                for (auto & output : b.outputs)
                    pathsToBuild.insert(output.second);
            buildables.push_back(std::move(b));
        }
    }

    if (mode == DryRun)
        printMissing(store, pathsToBuild, lvlError);
    else if (mode == Build)
        store->buildPaths(pathsToBuild);

    return buildables;
}

PathSet toStorePaths(ref<Store> store, RealiseMode mode,
    std::vector<std::shared_ptr<Installable>> installables)
{
    PathSet outPaths;

    for (auto & b : build(store, mode, installables))
        for (auto & output : b.outputs)
            outPaths.insert(output.second);

    return outPaths;
}

Path toStorePath(ref<Store> store, RealiseMode mode,
    std::shared_ptr<Installable> installable)
{
    auto paths = toStorePaths(store, mode, {installable});

    if (paths.size() != 1)
            throw Error("argument '%s' should evaluate to one store path", installable->what());

    return *paths.begin();
}

PathSet toDerivations(ref<Store> store,
    std::vector<std::shared_ptr<Installable>> installables, bool useDeriver)
{
    PathSet drvPaths;

    for (auto & i : installables)
        for (auto & b : i->toBuildables()) {
            if (b.drvPath.empty()) {
                if (!useDeriver)
                    throw Error("argument '%s' did not evaluate to a derivation", i->what());
                for (auto & output : b.outputs) {
                    auto derivers = store->queryValidDerivers(output.second);
                    if (derivers.empty())
                        throw Error("'%s' does not have a known deriver", i->what());
                    // FIXME: use all derivers?
                    drvPaths.insert(*derivers.begin());
                }
            } else
                drvPaths.insert(b.drvPath);
        }

    return drvPaths;
}

void InstallablesCommand::prepare()
{
    installables = parseInstallables(*this, getStore(), _installables, useDefaultInstallables());
}

void InstallableCommand::prepare()
{
    installable = parseInstallable(*this, getStore(), _installable, false);
}

}

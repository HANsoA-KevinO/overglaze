// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include "lab_game_profile.hpp"
#include "lab_installation.hpp"  // Loader: how a game loads us and where the payload lives
#include "lab_game_reasons.hpp"   // Reason / Check / Outcome and the one code table
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace lab::games {
// The manager's disk rule, in one place. The in-game thresholds (the host's
// startup check in core/src/installation.cpp and the bridge's in
// runtime/src/nr_live_core.hpp) are compiled into the game side and are
// deliberately separate from it.
inline constexpr std::uint64_t kDataDiskReserveBytes=30ULL<<30;   // install: keep free on the Lab data disk
inline constexpr std::uint64_t kGameDiskMinimumBytes=512ULL<<20;  // install: free on the game's disk
// Uninstall writes only a recovery copy of the files it removes, so it needs
// that copy's size plus this margin -- not the 30 GiB install reserve.
inline constexpr std::uint64_t kRecoveryCopyMarginBytes=64ULL<<20;
inline constexpr std::uint64_t recovery_space_needed(std::uint64_t copy_bytes) noexcept {return copy_bytes+kRecoveryCopyMarginBytes;}
// Storage retention: recovery copies the manager made under this rule, newest
// first, kept per game. Copies made before the rule existed are not in the
// ledger and are never touched by it; they stay until the owner decides what
// to do with them.
inline constexpr std::size_t kRecoveryCopiesKept=2;

// One adapter package: app/adapters/<name>/package.json plus its payload files.
// Every game shares the one published host build; the package pins the game
// (executable + graphics modules) and carries the V3 facts the host consumes.
struct Policy {
    std::string name, profile, executable, title, route="sl-rr", consent, config_sha256, checker_sha256;
    // Which track's published host this package carries. Absent in a package
    // means "research": every package that existed when the field was added was
    // research-track, and defaulting the other way would silently promote one.
    std::string track="research";
    // Where this package's own files go and what the loader is called. Absent
    // from a manifest means the original root dxgi.dll layout.
    Loader loader;
    std::filesystem::path game_root, directory; // game_root empty: match by executable name only
    std::map<std::string,std::string> pins, payload; // pins: exe + game modules; payload: dxgi/bridge/model
    std::vector<std::string> accepted_hosts, accepted_bridges; // current payload plus adopted earlier revisions
    // The risks the preflight recorded in the manifest found ("anti-tamper",
    // "anticheat"), and the anti-cheat names. Facts shown to the user, never a
    // refusal; a manifest whose verdict still says denuvo-blocked or
    // anticheat-blocked (written before risks replaced those refusals) counts too.
    std::vector<std::string> risks, anticheat;
    bool anti_tamper()const{return std::find(risks.begin(),risks.end(),"anti-tamper")!=risks.end();}
    profiles::Facts facts;
    // A package written before the rename (schema dlsslab-adapter-package-v1). `loader` already names the NEW identity (what a refresh
    // writes); legacy_basename / legacy_subdir say where its installed files are.
    // It can be recognised, uninstalled and refreshed, never installed as it is.
    bool legacy=false;
    std::string legacy_basename;
    std::filesystem::path legacy_subdir;
};
// Read-only. A malformed package is skipped and named in notes; nothing is fixed.
std::vector<Policy> load_packages(const std::filesystem::path& lab_root,std::vector<std::string>* notes=nullptr);
json policy_json(const Policy&);

struct Discovery {std::vector<std::filesystem::path> executables;bool complete=true;std::string notice;};
Discovery discover(const std::filesystem::path& input);
struct Entry {std::string id,title;std::filesystem::path exe;};

// Read-only preflight of one executable's directory. Never executes the
// target, loads a game DLL, or writes. Module presence is navigation, not
// compatibility; the absence of anti-cheat markers proves nothing.
struct Preflight {
    std::string store="unknown", route="none", verdict="unknown", executable_sha256, executable;
    bool executable_readable=false, pe_valid=false, pe_x64=false, denuvo_suspected=false, scan_complete=true, modules_signed=true;
    // anticheat_markers: the files found (relative paths); anticheat: what their
    // names point to ("Easy Anti-Cheat", "BattlEye", ...), or the file name
    // itself when the name points to no known product.
    std::vector<std::string> sections, anticheat_markers, anticheat, loader_conflicts, notes;
    // Anti-tamper (Denuvo sections) and anti-cheat (marker files) are risks the
    // user is told about and accepts at install, not verdicts: "anti-tamper",
    // "anticheat", in that order; empty when the checks found neither.
    std::vector<std::string> risks() const;
    json modules=json::object(); // name -> {sha256, version, signature}
    // Set when an installed OS package covers the EXE; executable_sha256 is
    // then "package:<full name>" whether or not the EXE is readable.
    json package_identity=nullptr;
    // Every check three-state: an unreadable EXE leaves the
    // Denuvo and PE checks "unknown", a truncated marker scan leaves the
    // anti-cheat check "unknown" -- never "pass". The verdict ladder above is
    // unchanged by this; what changes is that "not checked" is no longer
    // reported as "not found".
    std::vector<Check> checks;
    json to_json() const;
};
Preflight preflight(const std::filesystem::path& exe);
// The store a game directory belongs to. An installed OS package identity means
// Xbox app / GDK whatever the directory looks like: an Unreal GDK title keeps
// MicrosoftGame.config three levels above its EXE, where the marker walk does
// not reach. Values: steam | epic | gdk | unknown.
std::string store_kind(const std::filesystem::path& game_directory,bool has_package_identity);

// What inspect() reports. `state` is the backend state
// the page and the CLI have always shown; the three axes, the update parts,
// the health check, the reasons and the checks say the same thing as data.
// The backend composes no sentence: every word comes from the code table
// (lab_game_reasons.hpp).
struct Status {
    Entry entry;std::string state,host_hash,package,route;
    // Which published host this game's package takes: controller or research.
    std::string track;
    bool installed=false,can_install=false,can_uninstall=false,can_make_package=false,can_refresh_package=false,running=false;
    // Installed and something newer exists: the package no longer matches the
    // installed files (needs-update), or the package itself is behind the
    // published host, or only the proxy differs (update-available).
    bool update_available=false;
    json preflight=nullptr;
    // ---- axes
    // supported | unsupported | unknown. "experimental" (a package that was
    // never confirmed in the game) is reserved until the shipped compatibility
    // list exists; nothing produces it yet.
    std::string compatibility="unknown";
    // not-installed | installed | update-available | needs-update | game-changed |
    // incomplete | modified | external | research-managed | other-copy | unknown |
    // legacy (installed by the pre-rename build; migrate)
    std::string install_state="not-installed";
    // The loader strategy that applies: the transaction's for an installed game,
    // else the package's; empty when there is neither.
    std::string load_mode;
    struct Update {
        bool host=false,bridge=false,config=false,proxy=false; // installed file != package payload
        bool package_behind_published=false;                  // package payload != published host of its track
    } update;
    // What the in-game host would decide at start-up, checked here first:
    // ok | needs-update (the manifest no longer names the installed
    // files: "需要更新才能使用") | game-changed | unknown | not-applicable.
    std::string health="not-applicable";
    bool can_update=false,can_repin=false;
    // Risks the checks found, as facts the user is told about -- never a
    // refusal: "anti-tamper" (Denuvo sections in the EXE) and "anticheat"
    // (anti-cheat files in the game folder), with the anti-cheat names. Every
    // install, update and re-adaptation needs the user's risk acknowledgement,
    // whatever the checks found: not finding them proves nothing.
    std::vector<std::string> risks,anticheat;
    // steam | epic | gdk | unknown: decides how a late-loading game is started.
    std::string store="unknown";
    // A late-loading package (load_mode late_d3d12): how the player starts the
    // game, as text to paste. launch_via "steam": launch_command is the Steam
    // launch option; "watch": the watch command, for every other store. Both
    // empty for any other load mode.
    std::string launch_via,launch_command;
    std::vector<Reason> reasons; // the first one is the primary reason
    std::vector<Check> checks;   // in the order inspect() made them
    // Why an action the state would offer is not possible, keyed by action
    // (update, repin, ...). Absent: possible, or not part of this state.
    std::map<std::string,Reason> refusals;
};
// The legacy "detail" text is rendered from `reasons` through the code table;
// it is kept in the JSON for people reading the CLI output (no program reads it).
json status_json(const Status&);
std::string describe(const Status&);
// What the manager itself keeps under data/settings/plugin-manager/<id>/
// (read-only count). "managed" recovery copies are the ones in the retention
// ledger; the rest predate the rule and are kept until the owner decides what
// to do with them.
struct StorageUsage {
    struct Game {
        std::string id;
        std::uint64_t staging_directories=0,staging_bytes=0;
        std::uint64_t recovery_copies=0,recovery_bytes=0,managed_recovery_copies=0,managed_recovery_bytes=0;
        std::uint64_t other_bytes=0; // transaction, receipts, ledger
        bool complete=true;          // false: an entry could not be read, or the walk hit its bound
    };
    std::vector<Game> games;
    std::uint64_t total_bytes=0,data_disk_available=0,data_disk_reserve=kDataDiskReserveBytes;
    // _models/<sha256>/nvngx_dlssnr.dll: the one shared model copy that recovery copies refer to.
    std::uint64_t shared_models=0,shared_model_bytes=0;
    bool complete=true;
};
json storage_json(const StorageUsage&,const std::vector<Entry>& titles={});
// The user-supplied NR model (host contract V4). This project never
// includes or redistributes nvngx_dlssnr.dll; it looks for the user's copy in
// <program root>\models\ (app\models in the Lab layout) and uses it only when
// its SHA-256 is a reviewed version (lab_model_versions.hpp).
struct ModelStatus {
    std::filesystem::path path;
    bool present=false,known=false;
    std::string sha256,label,error;
};
json model_json(const ModelStatus&);
// Installer checks read only the selected application root and existing records.
// They never construct Manager, create directories or modify a game.
struct AppMaintenanceStatus {
    std::filesystem::path root;
    std::string operation,message;
    bool allowed=true;
    json blockers=json::array();
};
AppMaintenanceStatus app_maintenance_check(const std::filesystem::path&,const std::string& operation);
json app_maintenance_json(const AppMaintenanceStatus&);
struct PackageOptions {
    // How the game should load us. Default is the original root dxgi.dll proxy.
    Loader loader;
    std::string name,title;unsigned viewport=0;
    bool linear_depth=false,native_evaluate_host_rebind=false,binding_preservation=false,allow_unsigned_modules=false;
    float default_exposure_stops=0.f;
    // What the viewer's "生成适配包" asks for: the controller's root
    // proxy -- a thin dxgi.dll in the game directory and the controller host in
    // overglaze\ -- which every game working today uses. The default above still
    // means the research root layout, which the viewer must never produce.
    static PackageOptions controller_root_proxy(){
        PackageOptions o;o.loader.strategy="root_proxy_d3d12";o.loader.basename="overglaze_controller.dll";o.loader.subdir="overglaze";return o;}
    // Late loading: only overglaze\ in the game, nothing in its root; the
    // controller arrives after the game has started (Steam launch option or watch).
    static PackageOptions controller_late(){
        PackageOptions o;o.loader.strategy="late_d3d12";o.loader.basename="overglaze_controller.dll";o.loader.subdir="overglaze";return o;}
    // What the viewer makes for one game: the root proxy, except for a game
    // whose EXE carries Denuvo, which loads late. RE9 crashes at start with ANY
    // dxgi.dll in its root, even a pure forwarder, so the viewer never gives
    // such a game one. The command line still chooses freely.
    static PackageOptions for_viewer(bool denuvo){return denuvo?controller_late():controller_root_proxy();}
};
// How the player starts a late-loading game, built from this program's own
// root. Text to paste; nothing is written anywhere.
std::string steam_launch_option(const std::filesystem::path& lab_root); // "<root>\app\overglaze_launch.exe" %command%
std::string watch_command(const std::filesystem::path& lab_root);       // "<root>\app\overglaze_games.exe" watch
// Progress of a write operation. Stage names are the
// transaction's real steps, listed by operation_stages(); a nested operation
// (the uninstall and install inside an update or a repin) reports its own
// stages with `parent` set. status: start | done | skipped | failed.
struct ProgressEvent {
    std::string operation,stage,status,parent;
    unsigned index=0,count=0; // 1-based position of `stage` in operation_stages(operation)
    json params=json::object();
};
using Progress=std::function<void(const ProgressEvent&)>;
const std::vector<std::string>& operation_stages(std::string_view operation); // install | uninstall | update | repin
json progress_json(const ProgressEvent&);
// A write operation that stopped part-way. `install_after` says truthfully
// where the game was left (not-installed after an update whose install failed,
// for instance); `reason` renders through the code table. what() keeps the
// underlying message.
struct OperationError : std::runtime_error {
    std::string operation,stage,install_after;Reason reason;
    OperationError(std::string op,std::string stage_name,std::string after,Reason why,const std::string& message)
        :std::runtime_error(message),operation(std::move(op)),stage(std::move(stage_name)),install_after(std::move(after)),reason(std::move(why)){}
};
json operation_error_json(const OperationError&);
// Production: packages from <lab_root>/app/adapters and the published host in
// <lab_root>/app/plugin. Tests inject policies and a synthetic model hash.
class Manager {
public:
    explicit Manager(std::filesystem::path lab_root,std::optional<std::vector<Policy>> policies=std::nullopt,std::string model_sha256={},bool run_checker=true);
    // Anti-tamper and anti-cheat are risks, not refusals. What the user
    // acknowledges -- the viewer's install / update dialog, the CLI's
    // --accept-risk -- is passed to install(), update(), repin() and migrate()
    // as risk_accepted, for that one operation, and recorded in its
    // transaction. Overglaze never hides from, patches, spoofs, debugs or dumps
    // any protection, with or without it, and no identity or state check is
    // relaxed for it.
    // (There is no observation-only override for an "unsupported-route"
    // verdict: no preflight verdict produces that route.)
    std::vector<Entry> list() const;
    Entry add(const std::filesystem::path& exe);
    void forget(const std::string& id); // registry only; refuses installed/pending
    Status inspect(const Entry&) const;
    Preflight preflight(const Entry&) const;
    // Writes app/adapters/<name>/ from the published host. Refuses an existing
    // package directory. No game writes, no game launch.
    Policy make_package(const std::string& id,const PackageOptions&);
    // Re-copies a package's payload from the published host and rewrites its
    // config (version 4) and manifest. An empty strategy keeps the package's own loader.
    // Passing one changes how this game is loaded (root proxy vs injection).
    // Refused while the package's game has Lab files installed: a refreshed
    // package no longer accepts them and the host would refuse at start-up
    // update() refreshes and reinstalls in one operation instead.
    Policy refresh_package(const std::string& name,const std::string& strategy={});
    // Read-only dry run of install(): the files it would write and from where,
    // their sizes and hashes, the space checks, the stages. Writes nothing.
    json plan_install(const std::string& id) const;
    // risk_accepted is the user's own acknowledgement (dialog button or CLI
    // flag) that online / anti-cheat / anti-tamper games may not start, may be
    // kicked or penalised; consent is the text recorded in the receipt, never
    // an authorization read from a file. Both are required for every game.
    void install(const std::string& id,bool risk_accepted,const std::string& consent,const Progress& progress={});
    void uninstall(const std::string& id,bool confirmed,const Progress& progress={});
    // One user operation: when the package is behind the published host of
    // its track, re-run the refresh gates first, then uninstall, refresh the
    // package and install; otherwise uninstall and install the package's
    // current payload. Same consent rules as install(). Only when
    // update_available. If anything fails after the uninstall the game is left
    // not installed, and the OperationError says so.
    void update(const std::string& id,bool risk_accepted,const std::string& consent,const Progress& progress={});
    // Re-adapt a game whose EXE or modules changed, for a
    // controller-track package only: full preflight again (identity, loader
    // conflicts, module signatures; Denuvo and anti-cheat as risks), uninstall the old files if any, move the old package
    // into app/adapters-retired/ (never deleted), generate the new package from
    // the old one's facts and the published host, install. A research-track
    // package, or one whose profile is a compiled review row, is refused.
    // Returns where the old package now lives under app/adapters-retired/.
    std::filesystem::path repin(const std::string& id,bool risk_accepted,const std::string& consent,bool allow_unsigned_modules=false,const Progress& progress={});
    // A game installed by the pre-rename build (install state "legacy"): the
    // same operation as update() -- uninstall by recorded hashes, rewrite the
    // package under the new names, install -- refused for any other game.
    void migrate(const std::string& id,bool risk_accepted,const std::string& consent,const Progress& progress={});
    ModelStatus model_status() const;
    // User-selected file, reviewed SHA only. Existing different content is
    // refused; a matching model is reused. No DLL is loaded by this operation.
    ModelStatus import_model(const std::filesystem::path&);
    std::filesystem::path models_dir() const;  // <root>\app\models
    std::filesystem::path model_file() const;  // models_dir()\nvngx_dlssnr.dll
    void import_known_installations(); // registry only, no game writes
    // Read-only, bounded walk of the manager's own store. Never deletes.
    StorageUsage storage_usage() const;
    const std::vector<Policy>& policies()const{return policies_;}
    const std::vector<std::string>& package_notes()const{return package_notes_;}
    const std::filesystem::path& root()const{return root_;}
private:
    std::filesystem::path root_,store_;std::vector<Policy> policies_;std::vector<std::string> package_notes_;std::string model_sha256_;bool run_checker_=true;
    Entry find(const std::string&) const;
    const Policy* match(const std::filesystem::path& exe) const;
    // full=false: only what uninstall needs (no package payload, published host,
    // preflight or repin checks).
    Status inspect_impl(const Entry&,bool full) const;
    void install_impl(const std::string& id,bool risk_accepted,const std::string& consent,const Progress&,const std::string& parent);
    void uninstall_impl(const std::string& id,bool confirmed,const Progress&,const std::string& parent);
    // Why a refresh of this package would be refused now; empty code: it would not.
    Reason refresh_refusal(const Policy&,const Preflight&) const;
    // Why repin is not possible for this package; empty code: eligible. Runs the
    // preflight (stored in *pre) only when the package itself qualifies.
    Reason repin_refusal(const Policy&,const std::filesystem::path& exe,bool allow_unsigned,Preflight* pre) const;
    // Moves app/adapters/<name> to app/adapters-retired/<name>-repin-<UTC>; never deletes.
    std::filesystem::path retire_package(const Policy&);
    // Which track's published host to compare against: "controller" reads
    // app/plugin, "research" reads app/research/host.
    std::map<std::string,std::string> published_host(const std::string& track) const;
    Policy write_package(const std::filesystem::path& exe,const std::string& name,const profiles::Facts& facts,const std::string& title,const Preflight& pre,json notes,bool replace,const std::string& consent,const std::string& track,const Loader& loader);
};
}

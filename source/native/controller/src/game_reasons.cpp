// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_game_reasons.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <unordered_map>

namespace lab::games {
namespace {
// Every word the game manager shows about a game comes from here. Texts follow
// the existing page's wording; renamed terms (适配包 -> 适配配置 and so on)
// belong to a new front end, not to this table.
const std::vector<CodeText> kTable{
    // ---- state labels
    {"label.available","可安装"},
    {"label.installed","已安装 · 最新"},
    {"label.update-available","有更新"},
    {"label.needs-update","需更新"},
    {"label.other-copy","由另一份 Lab 安装"},
    {"label.external","已安装（外部方式）"},
    {"label.research-managed","研究版 · 只读"},
    {"label.game-changed","游戏已更新 · 需适配"},
    {"label.incomplete","安装未完成"},
    {"label.modified","文件被外部修改"},
    {"label.blocked","无法处理"},
    {"label.no-dlss","不支持：游戏没有 DLSS"},
    {"label.unsupported-store","商店包未识别"},
    {"label.loader-conflict","加载器冲突"},
    {"label.running","游戏运行中"},
    {"label.legacy","旧版 DLSS Lab · 需迁移"},
    // ---- buttons
    {"action.install","安装插件"},
    {"action.update","更新插件"},
    {"action.migrate","迁移到新版"},
    {"action.uninstall","卸载插件"},
    {"action.cleanup","清理未完成安装"},
    {"action.make-package","生成适配包"},
    {"action.refresh-package","刷新适配包"},
    {"action.repin","重新适配"},
    {"action.forget","从列表移除"},
    {"action.open-folder","打开游戏目录"},
    // ---- three-state outcomes
    {"outcome.pass","通过"},
    {"outcome.fail","未通过"},
    {"outcome.unknown","无法检查"},
    // ---- parameter values
    {"parts.host","宿主"},
    {"parts.bridge","桥"},
    {"parts.config","配置"},
    {"parts.proxy","转发器"},
    {"route.sl-rr","Streamline · 光线重建"},
    {"route.sl-sr","Streamline · 超分"},
    {"route.ngx-rr","引擎直连 · 光线重建"},
    {"route.ngx-sr","引擎直连 · 超分"},
    {"route.none","未发现 DLSS"},
    {"store.steam","Steam"},
    {"store.epic","Epic"},
    {"store.gdk","Xbox app（GDK）"},
    {"store.unknown","未识别的商店"},
    {"load-mode.root_dxgi_minimal","根目录 dxgi.dll（研究版）"},
    {"load-mode.root_proxy_d3d12","随游戏启动加载"},
    {"load-mode.root_proxy_on_insert","首次按 Insert 加载"},
    {"load-mode.late_d3d12","通过启动器接入（晚加载）"},
    {"load-mode.reframework_plugin","REFramework 插件"},
    {"operation.install","安装"},
    {"operation.update","更新"},
    {"operation.uninstall","卸载"},
    {"operation.repin","重新适配"},
    {"verdict.sl-rr-ready","可支持"},
    {"verdict.sl-sr-ready","可支持"},
    {"verdict.ngx-rr-ready","可支持"},
    {"verdict.ngx-sr-ready","可支持"},
    {"verdict.loader-conflict","目录里有其他加载器或 ReShade"},
    {"verdict.no-dlss","没有 DLSS 模块"},
    {"verdict.unsupported-store","无法识别商店包"},
    {"verdict.identity","EXE 身份与适配包不同"},
    // ---- check names
    {"check.identity","游戏身份"},
    {"check.pe","EXE 映像（x64 PE）"},
    {"check.denuvo","Denuvo 反篡改"},
    {"check.anticheat","反作弊标记"},
    {"check.loader-conflicts","其他加载器 / ReShade"},
    {"check.modules","DLSS 模块读取"},
    {"check.module-signatures","Streamline 模块签名"},
    {"check.route","接入方式"},
    {"check.transaction","安装记录"},
    {"check.files","已登记的插件文件"},
    {"check.game-pins","游戏与图形模块版本"},
    {"check.health.manifest","安装与适配包清单一致"},
    {"check.health.host","宿主与适配包清单一致"},
    {"check.health.bridge","桥与适配包清单一致"},
    {"check.health.config","配置与适配包清单一致"},
    {"check.update.proxy","转发器与适配包一致"},
    {"check.published","适配包与已发布插件同版"},
    {"check.package-payload","适配包载荷与清单一致"},
    {"check.local-model","本地 NR 模型与清单一致"},
    {"check.package-config","适配包配置与清单一致"},
    {"check.existing-host","既有宿主版本"},
    {"check.existing-bridge","既有桥版本"},
    {"check.existing-config","既有安装配置"},
    {"check.existing-output-root","既有安装属于这一份 Lab"},
    {"check.model","游戏目录里的 NR 模型"},
    // ---- why a check could not run or did not pass
    {"exe-unreadable","EXE 无法读取（商店许可保护）。"},
    {"exe-unreadable-no-package","EXE 无法读取，且无对应商店包身份。"},
    {"pe-invalid","PE 头无法解析（{value}）。"},
    {"not-x64","EXE 不是 x64 映像。"},
    {"scan-truncated","扫描未完成：{entries} 项（上限 {limit} 项、{seconds} 秒、3 层）。"},
    {"module-unreadable","有模块无法读取：{value}。"},
    {"unsigned-modules","未通过 Authenticode 校验：{value}。"},
    {"denuvo-sections","节表含 Denuvo 虚拟化段：{value}。"},
    {"markers-found","发现反作弊相关文件：{value}。"},
    {"conflicts-found","发现：{value}。"},
    {"no-dlss-modules","目录内没有 Streamline 或 NGX DLSS 模块。"},
    {"published-unreadable","无法读取已发布的插件：{value}"},
    // ---- status reasons, by state
    {"package-missing","已识别 DLSS（{route}，{store}），尚无适配包。"},
    {"modules-unsigned","Streamline 模块签名未通过。"},
    // ---- risks: facts the user is told about, never a refusal
    {"risk-anti-tamper","EXE 带 Denuvo 反篡改。Overglaze 只做被动共存，不修补、欺骗、调试或转储它。"},
    {"risk-anticheat","发现反作弊相关文件：{names}（按文件名判断）。"},
    {"risk-notice","联网或带反作弊的游戏可能无法启动、被踢出，或被游戏方处罚（包括封号）。Overglaze 不隐藏自己，不绕过或修改任何保护。风险由你自行承担。"},
    {"risk-unproven","未检测到反作弊或反篡改，不代表没有。"},
    {"risk-accept","点击下方按钮即表示接受上述风险。"},
    {"tag.anti-tamper","Denuvo"},
    {"tag.anticheat","反作弊"},
    {"load-plan","加载方式：{load_mode}"},
    {"load-late-anti-tamper","带 Denuvo 的游戏晚加载，游戏根目录不放文件。"},
    {"package-stale","适配包需刷新至已发布版本 {published}。"},
    {"package-ready","当前版本匹配适配包「{package}」（{route}）。"},
    {"launch-root-layout","从商店启动，按 Insert 打开面板。NR 默认关闭，运行状态以面板为准。"},
    {"launch-root-proxy","插件位于 {subdir}\\，根目录为转发器 dxgi.dll。从商店启动，按 Insert 打开面板；NR 默认关闭。"},
    {"launch-root-proxy-on-insert","插件位于 {subdir}\\，根目录为转发器 dxgi.dll。首次按 Insert 后加载并打开面板，约需几秒；NR 默认关闭。"},
    {"launch-late","插件位于 {subdir}\\。Steam 启动选项填 {option}，首次按 Insert 接入并打开面板；NR 默认关闭。"},
    {"launch-late-watch","插件位于 {subdir}\\。先运行 {command} 并保持窗口打开，再启动游戏；接入后按 Insert 打开面板。NR 默认关闭。"},
    // ---- how a late-loading game is started (the page's launch card)
    {"launch.steam","在 Steam 中：库 → 右键游戏 → 属性 → 通用 → 启动选项，粘贴下面一行。"},
    {"launch.insert","进入游戏后，首次按 Insert 加载插件并打开面板。"},
    {"launch.remove","卸载 Overglaze 前先删除这条启动选项，否则游戏无法启动。"},
    {"launch.watch","不经 Steam 启动：先运行下面的命令并保持窗口打开，再从商店启动游戏。游戏启动后自动接入，按 Insert 打开面板。"},
    {"launch.uninstall","卸载后可删除 Steam 启动选项；卸载 Overglaze 前必须删除，否则游戏无法启动。"},
    {"installed","插件文件已校验，与适配包清单一致。"},
    {"legacy-install","已安装旧版 DLSS Lab，新版无法加载。退出游戏后可迁移；旧文件卸载前保存恢复副本。"},
    {"legacy-package","旧版 DLSS Lab 适配包，需刷新后安装。"},
    {"model-ready","NR 模型已就绪（已审阅的版本）。"},
    {"update-available","有新版插件（{parts}），当前安装仍可用。退出游戏后可更新；旧文件卸载前保存恢复副本。"},
    {"package-behind-published","适配包落后于已发布版本 {published}；更新将重新预检、刷新适配包并重装。"},
    {"needs-update","插件与清单不符（{parts}），启动时将拒绝加载。退出游戏后需更新。"},
    {"incomplete","上次操作未完成或文件缺失，可清理已登记的剩余文件。"},
    {"game-changed","游戏或图形模块版本已变化（{module}）；不要启用 NR。"},
    {"game-package-changed","游戏包版本已变化（{package}）；不安装、不启用。"},
    {"installed-package-missing","适配包缺失；不要启用 NR。"},
    {"uninstall-still-possible","仍可卸载身份未变的 Lab 文件。"},
    {"repin-available","可重新适配：预检、卸载旧插件、归档旧适配包至 adapters-retired，再生成新包并安装。"},
    {"existing-external","外部安装的 Lab 插件与适配包同版；卸载仅移除已核验的宿主、桥和配置，保留模型。"},
    {"existing-research","研究版安装由研究工具管理；本页只读。"},
    {"installed-by-other-copy","插件属于另一份 Lab（数据目录 {output_root}），需在原程序中管理或卸载。"},
    {"unsupported-store","EXE 无法读取且无商店包身份，无法校验身份，暂不支持。"},
    {"loader-conflict","目录里已有其他加载器或 ReShade（{files}）；先处理兼容关系。"},
    {"no-dlss","目录内没有 Streamline 或 NGX DLSS 模块；NR 需要光线重建或超分的输入。"},
    {"preflight-incomplete","{count} 项无法检查（{check}），未判定通过。"},
    // ---- blocked: a check that refused (every refusal has a name)
    {"path-invalid","游戏路径无效：{message}"},
    {"process-check-failed","无法检查游戏进程，暂不允许修改：{message}"},
    {"multiple-packages","多个适配包指向这款游戏：{message}"},
    {"transaction-invalid","安装记录损坏或与这款游戏不匹配：{message}"},
    {"file-unreadable","文件正在使用、权限不足或不是普通文件：{message}"},
    {"installed-files-modified","插件文件被修改（{file}）；拒绝卸载或覆盖。"},
    {"foreign-loader","{file} 版本未识别；不覆盖、不删除。"},
    {"foreign-bridge","游戏目录内 NR 桥版本未识别；不覆盖、不删除。"},
    {"existing-config-unsupported","游戏目录里的安装配置不是受支持的契约。"},
    {"existing-config-mismatch","游戏目录里的安装配置与适配包不一致。"},
    {"model-mismatch","游戏目录里已有一份身份不同的 NR 模型；不覆盖。"},
    {"package-payload-mismatch","适配包里的插件文件与它的清单不一致；不安装。"},
    {"local-model-mismatch","Lab 的本地 NR 模型与适配包清单不一致。"},
    {"model-missing","缺少 nvngx_dlssnr.dll：{path}。NVIDIA 模型需自行提供，本项目不包含、也不分发。"},
    {"model-unknown-version","nvngx_dlssnr.dll 版本未审阅（SHA-256 {hash}），不使用。"},
    {"model-unrecognized-allowed","未识别 · 非原版 · 风险自负（SHA-256 {hash}）。未识别的模型可能无法运行、画质与原版不同，或带来安全风险；Overglaze 只校验原版。"},
    {"model-unrecognized-tag","未识别 · 非原版 · 风险自负"},
    {"model-unrecognized-notice","未识别的模型可能无法运行、画质与原版不同，或带来安全风险；Overglaze 只校验原版。"},
    {"migrate-not-legacy","这款游戏没有旧版（DLSS Lab）安装，不需要迁移。"},
    {"package-config-mismatch","适配包配置与清单里的 config_sha256 不一致。"},
    {"published-host-missing","找不到已发布的插件：{message}"},
    {"internal-error","检查时出错：{message}"},
    // ---- why an action is not offered or not possible now
    {"game-running","游戏正在运行；退出后可用。"},
    {"busy","正在执行另一项操作。"},
    {"forget-installed","游戏目录里还有已登记的插件文件；先卸载，再从列表移除。"},
    {"update-refresh-refused","更新预检未通过（{verdict}），无法刷新适配包。"},
    {"repin-research-track","研究轨适配包由研究工具管理，本页不改写。"},
    {"repin-compiled-row","需先更新编译期审阅行（{profile}），无法自动适配。"},
    {"repin-no-game-root","适配包没有记录游戏目录，无法重新预检。"},
    {"repin-preflight-failed","重新预检没有通过（{verdict}）：不重新适配。"},
    {"repin-unsigned-modules","Streamline 签名未通过，不自动适配；命令行可用 --allow-unsigned-modules 显式允许。"},
    {"repin-route-changed","接入方式由 {old} 变为 {new}，需重新生成适配包。"},
    {"repin-not-changed","游戏版本没有变化，不需要重新适配。"},
    {"repin-foreign-install","存在外部安装的 Lab 文件，需由原安装工具卸载后适配。"},
    {"not-possible-now","当前不可用，需重新检查。"},
    {"refresh-would-break-installed","刷新适配包会使当前插件拒绝加载。需用「更新插件」完成卸载、刷新和安装。"},
    // ---- operation results
    {"install-done","安装完成。"},
    {"update-done","新版已安装，旧版恢复副本已保存。"},
    {"migrate-done","迁移完成，新版已安装，旧版恢复副本已保存。"},
    {"uninstall-done","插件已卸载，恢复副本：data\\settings\\plugin-manager。游戏、原有模型和采集数据保留。"},
    {"repin-done","重新适配并安装完成，旧适配包归档于 {retired}。"},
    {"package-made","适配包「{package}」已生成（{route}）。"},
    {"package-made-controller","适配包「{package}」已生成（{route}）。"},
    {"package-refreshed","适配包「{package}」已刷新。"},
    {"forgotten","已从列表移除，没有删除游戏文件。"},
    {"registered","游戏已添加。"},
    {"operation-failed","{operation}未完成 · {stage}：{message}"},
    {"update-failed-uninstalled","更新未完成 · {stage}：{message}。旧插件已卸载；恢复副本：{recovery}。当前未安装，可重新安装。"},
    {"repin-failed-uninstalled","重新适配未完成 · {stage}：{message}。旧插件已卸载；恢复副本：{recovery}。当前未安装。"},
    {"repin-failed-retired","重新适配未完成 · {stage}：{message}。旧适配包归档于 {retired}，未删除；当前无可用适配包，需重新生成。"},
    // ---- progress stages (named after the transaction's real steps)
    {"stage.install.check","检查确认与状态"},
    {"stage.install.pin-game","锁定游戏 EXE 与模块"},
    {"stage.install.space","检查磁盘空间"},
    {"stage.install.receipts","核对安装记录与回执"},
    {"stage.install.stage-files","复制到暂存目录并校验"},
    {"stage.install.record-transaction","写入安装事务"},
    {"stage.install.activate","写入游戏目录"},
    {"stage.install.verify","运行安装检查器"},
    {"stage.install.commit","记录安装完成"},
    {"stage.install.cleanup-staging","删除暂存目录"},
    {"stage.uninstall.check","检查确认与状态"},
    {"stage.uninstall.pin-files","锁定已登记文件"},
    {"stage.uninstall.space","检查恢复副本所需空间"},
    {"stage.uninstall.recovery-copy","保存恢复副本"},
    {"stage.uninstall.record-transaction","写入卸载事务"},
    {"stage.uninstall.remove-files","删除插件文件"},
    {"stage.uninstall.commit","记录卸载完成"},
    {"stage.uninstall.retention","整理恢复副本"},
    {"stage.update.check","检查确认与状态"},
    {"stage.update.refresh-check","重新预检"},
    {"stage.update.uninstall","卸载旧插件"},
    {"stage.update.refresh-package","刷新适配包"},
    {"stage.update.install","安装新插件"},
    {"stage.repin.check","检查确认与状态"},
    {"stage.repin.preflight","重新预检"},
    {"stage.repin.uninstall","卸载旧插件"},
    {"stage.repin.retire-package","归档旧适配包"},
    {"stage.repin.make-package","生成新适配包"},
    {"stage.repin.install","安装"},
};
const std::unordered_map<std::string_view,std::string_view>& index(){
    static const auto map=[]{std::unordered_map<std::string_view,std::string_view> m;for(const auto& e:kTable)m.emplace(e.code,e.text);return m;}();
    return map;}
std::string value_text(const std::string& name,const json& v,const json& params){
    if(v.is_string()){const auto s=v.get<std::string>();
        std::string prefix=name;for(auto& c:prefix)if(c=='_')c='-';
        if(name=="stage"&&params.contains("operation")&&params.at("operation").is_string())
            if(const auto* t=code_text("stage."+params.at("operation").get<std::string>()+"."+s))return t;
        if(const auto* t=code_text(prefix+"."+s))return t;
        return s;}
    if(v.is_array()){std::string out;for(const auto& item:v){if(!out.empty())out+="、";out+=value_text(name,item,params);}return out;}
    if(v.is_boolean())return v.get<bool>()?"是":"否";
    if(v.is_number_unsigned())return std::to_string(v.get<std::uint64_t>());
    if(v.is_number_integer())return std::to_string(v.get<std::int64_t>());
    if(v.is_number_float()){char b[32];std::snprintf(b,sizeof(b),"%g",v.get<double>());return b;}
    if(v.is_null())return "?";
    return v.dump();}
}
const char* outcome_name(Outcome o) noexcept{return o==Outcome::pass?"pass":o==Outcome::fail?"fail":"unknown";}
json check_json(const Check& c){return {{"name",c.name},{"ok",c.outcome==Outcome::pass?json(true):c.outcome==Outcome::fail?json(false):json(nullptr)},
    {"value",c.value},{"reason",c.reason.empty()?json(nullptr):json(c.reason)}};}
json reason_json(const Reason& r){return {{"code",r.code},{"params",r.params.is_object()?r.params:json::object()}};}
const std::vector<CodeText>& code_table(){return kTable;}
const char* code_text(std::string_view code) noexcept{
    try{const auto& m=index();const auto it=m.find(code);return it==m.end()?nullptr:it->second.data();}catch(...){return nullptr;}}
std::vector<std::string> placeholders(std::string_view text){std::vector<std::string> out;
    for(std::size_t i=0;(i=text.find('{',i))!=std::string_view::npos;){const auto end=text.find('}',i);if(end==std::string_view::npos)break;
        std::string name(text.substr(i+1,end-i-1));if(std::find(out.begin(),out.end(),name)==out.end())out.push_back(name);i=end+1;}
    return out;}
std::string render(std::string_view code,const json& params){
    const auto* raw=code_text(code);if(!raw)return "["+std::string(code)+"]";
    const std::string_view text(raw);std::string out;out.reserve(text.size()+32);
    for(std::size_t i=0;i<text.size();){
        if(text[i]=='{'){const auto end=text.find('}',i);
            if(end!=std::string_view::npos){const std::string name(text.substr(i+1,end-i-1));
                out+=params.is_object()&&params.contains(name)?value_text(name,params.at(name),params):"?";i=end+1;continue;}}
        out+=text[i++];}
    return out;}
std::string render(const Reason& r){return render(r.code,r.params);}
std::string render_all(const std::vector<Reason>& reasons){std::string out;for(const auto& r:reasons){if(!out.empty())out+=' ';out+=render(r);}return out;}
json code_table_json(){json codes=json::object();for(const auto& e:kTable)codes[std::string(e.code)]=std::string(e.text);
    return {{"schema","overglaze-game-codes-v1"},{"codes",codes},
        {"scope","every word the game manager shows about a game; the backend reports codes with parameters and composes no sentences"}};}
}

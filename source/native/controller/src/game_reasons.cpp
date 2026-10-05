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
    {"label.needs-update","需要更新才能使用"},
    {"label.other-copy","由另一份 Lab 安装"},
    {"label.external","已安装（外部方式）"},
    {"label.research-managed","研究版安装 · 由研究工具管理"},
    {"label.game-changed","游戏已更新，需要重新适配"},
    {"label.incomplete","安装未完成"},
    {"label.modified","文件被外部修改"},
    {"label.blocked","无法处理"},
    {"label.denuvo-blocked","不支持：反篡改保护"},
    {"label.anticheat-blocked","不支持：反作弊"},
    {"label.no-dlss","不支持：游戏没有 DLSS"},
    {"label.unsupported-store","暂不支持：无法识别商店包"},
    {"label.loader-conflict","需要处理：目录里已有其他注入工具"},
    {"label.running","游戏运行中"},
    {"label.legacy","旧版（DLSS Lab）安装 · 需要迁移"},
    // ---- buttons
    {"action.install","安装 NR 插件"},
    {"action.update","更新插件"},
    {"action.migrate","迁移到新版"},
    {"action.uninstall","卸载插件"},
    {"action.cleanup","清理未完成安装"},
    {"action.make-package","生成适配包"},
    {"action.refresh-package","刷新适配包"},
    {"action.repin","重新检查并适配"},
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
    {"load-mode.root_dxgi_minimal","游戏目录里的 dxgi.dll（研究版布局）"},
    {"load-mode.root_proxy_d3d12","随游戏启动加载"},
    {"load-mode.root_proxy_on_insert","随游戏启动，第一次按 Insert 时加载"},
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
    {"verdict.denuvo-blocked","EXE 带 Denuvo 反篡改"},
    {"verdict.anticheat-blocked","检测到反作弊标记"},
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
    {"check.health.manifest","适配包清单认得这次安装"},
    {"check.health.host","宿主与适配包清单一致"},
    {"check.health.bridge","桥与适配包清单一致"},
    {"check.health.config","配置与适配包清单一致"},
    {"check.update.proxy","转发器与适配包一致"},
    {"check.published","适配包与已发布插件同版"},
    {"check.package-payload","适配包载荷与清单一致"},
    {"check.local-model","本地 NR 模型与清单一致"},
    {"check.package-config","适配包配置与清单一致"},
    {"check.existing-host","既有宿主是 Lab 认得的版本"},
    {"check.existing-bridge","既有桥是 Lab 认得的版本"},
    {"check.existing-config","既有安装配置"},
    {"check.existing-output-root","既有安装属于这一份 Lab"},
    {"check.model","游戏目录里的 NR 模型"},
    // ---- why a check could not run or did not pass
    {"exe-unreadable","EXE 无法读取（受商店许可保护的包，如 Xbox app）。"},
    {"exe-unreadable-no-package","EXE 无法读取，系统里也没有覆盖它的包身份。"},
    {"pe-invalid","PE 头无法解析（{value}）。"},
    {"not-x64","EXE 不是 x64 映像。"},
    {"scan-truncated","目录只扫了 {entries} 项（上限 {limit} 项、{seconds} 秒、3 层），没有扫完。"},
    {"module-unreadable","有模块无法读取：{value}。"},
    {"unsigned-modules","未通过 Authenticode 校验：{value}。"},
    {"denuvo-sections","节表含 Denuvo 虚拟化段：{value}。"},
    {"markers-found","发现反作弊标记：{value}。"},
    {"conflicts-found","发现：{value}。"},
    {"no-dlss-modules","目录内没有 Streamline 或 NGX DLSS 模块。"},
    {"published-unreadable","无法读取已发布的插件：{value}"},
    // ---- status reasons, by state
    {"package-missing","已识别 DLSS 游戏（{route}，{store}）。还没有适配包：先生成适配包，再安装。"},
    {"modules-unsigned","注意：Streamline 模块未通过签名校验。"},
    {"denuvo-override","该 EXE 带 Denuvo 反篡改，操作者已在命令行显式接受被动共存；Lab 不修补、不欺骗、不调试、不转储反篡改。"},
    {"package-stale","适配包落后于当前已发布的插件（{published}）：先刷新适配包，再安装。"},
    {"package-ready","本地版本命中适配包「{package}」（{route}）。"},
    {"launch-root-layout","安装后从商店正常启动游戏，进游戏后按 Insert 打开面板；NR 默认关闭。运行时 NR 是否生效以游戏内面板为准。"},
    {"launch-root-proxy","游戏目录只放转发器 dxgi.dll，插件在 {subdir}\\ 子目录。从商店正常启动游戏，进图后按 Insert 打开面板；NR 默认关闭。"},
    {"launch-root-proxy-on-insert","游戏目录只放转发器 dxgi.dll，插件在 {subdir}\\ 子目录。从商店正常启动；启动期间插件什么都不做，进游戏后第一次按 Insert 才加载并自动打开面板，约需几秒；NR 默认关闭。"},
    {"launch-late","晚加载：游戏目录里不放任何 Lab 文件，插件在 {subdir}\\ 子目录。Steam 游戏在启动选项填 \"{launcher}\" %command%，照常启动游戏，进游戏后第一次按 Insert 自动接入并打开面板；NR 默认关闭。"},
    {"installed","插件文件已校验，与适配包清单一致。"},
    {"legacy-install","这款游戏装的是改名前的旧版插件（DLSS Lab），新版不会加载它。退出游戏后点「迁移到新版」：按登记的身份卸载旧文件（保存恢复副本），用新名字重写适配包，再装入新版。"},
    {"legacy-package","这个适配包还是改名前的旧版（DLSS Lab）：刷新适配包会用新名字重写它，然后再安装。"},
    {"model-ready","NR 模型已就绪（已审阅的版本）。"},
    {"update-available","有新版插件（{parts}）：退出游戏后点「更新插件」，会先卸载旧文件（保存恢复副本），再装新版本。游戏现在仍可正常使用。"},
    {"package-behind-published","已发布的插件比这款游戏的适配包新（{published}）：更新时会先重新预检并刷新适配包，再重新安装，一次完成。"},
    {"needs-update","适配包清单已经不认游戏目录里的插件文件（{parts}不同），游戏内插件启动时会拒绝加载。退出游戏后点「更新插件」。"},
    {"incomplete","上次操作未完成或文件缺失，可清理已登记的剩余文件。"},
    {"game-changed","游戏或图形模块版本已变化（{module}）；不要启用 NR。"},
    {"game-package-changed","游戏包版本已变化（{package}）；不安装、不启用。"},
    {"installed-package-missing","找不到这款游戏的适配包（可能已被移走或退役）；不要启用 NR。"},
    {"uninstall-still-possible","仍可卸载身份未变的 Lab 文件。"},
    {"repin-available","可以重新检查并适配：重新预检、卸载旧插件、把旧适配包移到 adapters-retired（不删除）、生成新适配包后重新安装。"},
    {"existing-external","识别到其他方式安装的 Lab 插件（与适配包同版）；卸载只管理已核验的宿主、桥和配置，保留原有模型。"},
    {"existing-research","这是研究版安装，由研究工具管理；本页只读。"},
    {"installed-by-other-copy","这款游戏的插件属于另一份 Lab（数据目录 {output_root}）；请在那一份程序里管理，或在那里卸载后在这里重新安装。"},
    {"denuvo-blocked","EXE 带 Denuvo 反篡改；不绕过，不安装。"},
    {"anticheat-blocked","检测到反作弊标记（{markers}）；不安装。"},
    {"unsupported-store","EXE 受商店包保护无法读取，系统里也没有它的包身份；身份无法钉死，暂不支持。"},
    {"loader-conflict","目录里已有其他加载器或 ReShade（{files}）；先处理兼容关系。"},
    {"no-dlss","目录内没有 Streamline 或 NGX DLSS 模块；NR 需要光线重建或超分的输入。"},
    {"preflight-incomplete","有 {count} 项检查无法完成（{check}）：结果不是“通过”，见检查明细。"},
    // ---- blocked: a check that refused (every refusal has a name)
    {"path-invalid","游戏路径无效：{message}"},
    {"process-check-failed","无法检查游戏进程，暂不允许修改：{message}"},
    {"multiple-packages","多个适配包指向这款游戏：{message}"},
    {"transaction-invalid","安装记录损坏或与这款游戏不匹配：{message}"},
    {"file-unreadable","文件正在使用、权限不足或不是普通文件：{message}"},
    {"installed-files-modified","已登记的插件文件被外部修改（{file}）；拒绝卸载或覆盖。"},
    {"foreign-loader","游戏目录里的 {file} 不是 Lab 认得的版本（可能是其他插件）；不覆盖、不删除。"},
    {"foreign-bridge","游戏目录里的 NR 桥不是 Lab 认得的版本；不覆盖、不删除。"},
    {"existing-config-unsupported","游戏目录里的安装配置不是受支持的契约。"},
    {"existing-config-mismatch","游戏目录里的安装配置与适配包不一致。"},
    {"model-mismatch","游戏目录里已有一份身份不同的 NR 模型；不覆盖。"},
    {"package-payload-mismatch","适配包里的插件文件与它的清单不一致；不安装。"},
    {"local-model-mismatch","Lab 的本地 NR 模型与适配包清单不一致。"},
    {"model-missing","找不到 NR 模型 nvngx_dlssnr.dll（{path}）。本项目不包含、也不分发这个文件，它属于 NVIDIA；请自行准备，放到这个位置后重新检查。"},
    {"model-unknown-version","这份 nvngx_dlssnr.dll（SHA-256 {hash}）不是已审阅的版本，不使用。"},
    {"migrate-not-legacy","这款游戏没有旧版（DLSS Lab）安装，不需要迁移。"},
    {"package-config-mismatch","适配包配置与清单里的 config_sha256 不一致。"},
    {"published-host-missing","找不到已发布的插件：{message}"},
    {"internal-error","检查时出错：{message}"},
    // ---- why an action is not offered or not possible now
    {"game-running","游戏正在运行；退出后可用。"},
    {"busy","正在执行另一项操作。"},
    {"forget-installed","游戏目录里还有已登记的插件文件；先卸载，再从列表移除。"},
    {"update-refresh-refused","更新要先刷新适配包，但重新预检没有通过（{verdict}）。"},
    {"update-needs-denuvo-flag","这款游戏带 Denuvo：刷新适配包要在命令行 update 时显式加 --denuvo-passive-coexistence，本页不提供这个开关。"},
    {"refresh-needs-denuvo-flag","这款游戏带 Denuvo：刷新适配包要在命令行 refresh-package 时显式加 --denuvo-passive-coexistence，本页不提供这个开关。"},
    {"repin-research-track","研究轨的适配包由研究工具重新适配，本程序不改写。"},
    {"repin-compiled-row","这款游戏有编译期审阅行（{profile}）：游戏新版本要先更新审阅行，不能自动重新适配。"},
    {"repin-no-game-root","适配包没有记录游戏目录，无法重新预检。"},
    {"repin-preflight-failed","重新预检没有通过（{verdict}）：不重新适配。"},
    {"repin-needs-denuvo-flag","这款游戏带 Denuvo：重新适配要在命令行 repin 时显式加 --denuvo-passive-coexistence，本页不提供这个开关。"},
    {"repin-unsigned-modules","Streamline 模块未通过签名校验：不自动重新适配（命令行可用 --allow-unsigned-modules 显式允许）。"},
    {"repin-route-changed","游戏更新后接入方式从 {old} 变成了 {new}：请重新生成适配包，不走自动重新适配。"},
    {"repin-not-changed","游戏版本没有变化，不需要重新适配。"},
    {"repin-foreign-install","游戏目录里有不经本管理器安装的 Lab 文件：先用安装它的工具卸载，再重新适配。"},
    {"not-possible-now","当前状态不允许这个操作；请点「重新检查」。"},
    {"refresh-would-break-installed","这款游戏已经装了插件：单独刷新适配包会让已装的文件失效（游戏内插件启动时会拒绝加载）。请用「更新插件」，它会依次卸载、刷新、安装。"},
    // ---- operation results
    {"install-done","安装完成。"},
    {"update-done","更新完成：旧插件文件已卸载（恢复副本已保存），新版本已装入。"},
    {"migrate-done","迁移完成：旧版文件已按身份卸载（恢复副本已保存），适配包已用新名字重写，新版已装入。"},
    {"uninstall-done","已移除本次登记的插件文件。游戏、原有模型和采集数据保留；恢复副本存于 data\\settings\\plugin-manager。"},
    {"repin-done","重新适配完成：旧适配包已移到 {retired}（未删除），新适配包已生成并安装。"},
    {"package-made","已生成适配包「{package}」（{route}）。现在可以安装。"},
    {"package-made-controller","已生成适配包「{package}」（{route}；游戏目录只放转发器 dxgi.dll，插件在 overglaze\\ 子目录；viewport 与深度类型在运行时从游戏自己的调用读出）。现在可以安装。"},
    {"package-refreshed","适配包「{package}」已刷新到当前已发布的插件。"},
    {"forgotten","已从列表移除，没有删除游戏文件。"},
    {"registered","游戏已登记。安装需要单独确认，不会因添加路径而自动写入游戏。"},
    {"operation-failed","{operation}没有完成（停在：{stage}）：{message}"},
    {"update-failed-uninstalled","更新没有完成（停在：{stage}）。旧插件已卸载，恢复副本在 {recovery}；游戏现在是未安装状态，可以直接重新安装。原因：{message}"},
    {"repin-failed-uninstalled","重新适配没有完成（停在：{stage}）。旧插件已卸载，恢复副本在 {recovery}；游戏现在是未安装状态。原因：{message}"},
    {"repin-failed-retired","重新适配没有完成（停在：{stage}）。旧适配包已移到 {retired}（未删除），这款游戏现在没有可用的适配包，需要重新生成。原因：{message}"},
    // ---- progress stages (named after the transaction's real steps)
    {"stage.install.check","核对确认与当前状态"},
    {"stage.install.pin-game","按句柄钉住游戏 EXE 与模块"},
    {"stage.install.space","检查磁盘空间"},
    {"stage.install.receipts","核对安装记录与回执"},
    {"stage.install.stage-files","复制到暂存目录并校验"},
    {"stage.install.record-transaction","写入安装事务"},
    {"stage.install.activate","复制到游戏目录（加载器最后）"},
    {"stage.install.verify","运行安装检查器"},
    {"stage.install.commit","记录安装完成"},
    {"stage.install.cleanup-staging","删除暂存目录"},
    {"stage.uninstall.check","核对确认与当前状态"},
    {"stage.uninstall.pin-files","按句柄钉住已登记的文件"},
    {"stage.uninstall.space","检查恢复副本所需空间"},
    {"stage.uninstall.recovery-copy","保存恢复副本"},
    {"stage.uninstall.record-transaction","写入卸载事务"},
    {"stage.uninstall.remove-files","按句柄删除插件文件（转发器最先）"},
    {"stage.uninstall.commit","记录卸载完成"},
    {"stage.uninstall.retention","按保留规则整理恢复副本"},
    {"stage.update.check","核对确认与当前状态"},
    {"stage.update.refresh-check","重新预检（需要刷新适配包时）"},
    {"stage.update.uninstall","卸载旧插件"},
    {"stage.update.refresh-package","刷新适配包"},
    {"stage.update.install","安装新插件"},
    {"stage.repin.check","核对确认与当前状态"},
    {"stage.repin.preflight","完整重新预检"},
    {"stage.repin.uninstall","卸载旧插件"},
    {"stage.repin.retire-package","退役旧适配包（移到 adapters-retired）"},
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

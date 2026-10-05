// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_late_panel.hpp"
#include <cstdio>
namespace lab::latebind {
using Microsoft::WRL::ComPtr;
namespace {
std::string hex(const void* p){char b[24];std::snprintf(b,sizeof(b),"0x%llx",static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p)));return b;}
}

ChainTracker::Entry* ChainTracker::entry(const void* chain)noexcept{
    if(!chain)return nullptr;
    for(auto& e:entries_)if(e.chain==chain)return &e;
    Entry* slot=nullptr;for(auto& e:entries_)if(!e.chain){slot=&e;break;}
    // Full: the least recently presenting chain that is not followed makes room.
    if(!slot){for(auto& e:entries_)if(e.chain!=followed_&&(!slot||e.last<slot->last))slot=&e;++evictions_;}
    if(!slot)return nullptr;
    *slot=Entry{chain,0,0,false};return slot;
}
void ChainTracker::adopt(const void* chain)noexcept{followed_=chain;followed_last_=seq_;++adoptions_;(void)entry(chain);}
ChainTracker::Verdict ChainTracker::present(const void* chain)noexcept{
    ++seq_;if(previous_&&chain!=previous_)++switches_;previous_=chain;
    auto* e=entry(chain);if(e){++e->presents;e->last=seq_;}
    if(chain==followed_){followed_last_=seq_;return Verdict::followed;}
    ++other_presents_;
    if(!followed_||!e||e->refused)return Verdict::other;
    // The followed chain presented within the window: both are presenting, or
    // it has only just stopped. Either way the first one stays followed.
    if(seq_-followed_last_<=policy_.stale_presents){++held_;return Verdict::other;}
    if(e->presents<policy_.confirm_presents)return Verdict::other;
    if(adoptions_>=policy_.max_adoptions){++over_cap_;return Verdict::other;}
    return Verdict::take_over;
}
void ChainTracker::refuse(const void* chain,const std::string& why){if(auto* e=entry(chain))e->refused=true;++refusals_[why];}
json ChainTracker::status()const{
    json chains=json::array();
    for(const auto& e:entries_)if(e.chain)chains.push_back({{"chain",hex(e.chain)},{"presents",e.presents},{"followed",e.chain==followed_},{"refused",e.refused}});
    json refused=json::object();for(const auto& [k,v]:refusals_)refused[k]=v;
    return {{"followed",followed_?json(hex(followed_)):json(nullptr)},{"adoptions",adoptions_},{"take_overs",adoptions_?adoptions_-1:0},
        {"chain_switches",switches_},{"other_chain_presents",other_presents_},{"held_while_followed_recently_presented",held_},
        {"take_overs_over_cap",over_cap_},{"evicted_chains",evictions_},{"refused",refused},{"chains",chains},
        {"policy",{{"stale_presents",policy_.stale_presents},{"confirm_presents",policy_.confirm_presents},{"max_adoptions",policy_.max_adoptions}}},
        {"scope","identities only (at most 4 tracked); no reference to any swapchain is held"}};
}

LatePanel::LatePanel(GameOverlay& overlay,Evidence evidence,ChainTracker::Policy policy):overlay_(overlay),evidence_(std::move(evidence)),chains_(policy){}
void LatePanel::adopt(IDXGISwapChain* chain)noexcept{std::lock_guard lock(mutex_);chains_.adopt(chain);}
void LatePanel::note(std::string text){
    if(!notes_.empty()&&notes_.back()==text)return;
    notes_.push_back(text);while(notes_.size()>4)notes_.pop_front();
    if(evidence_.report)try{evidence_.report(text);}catch(...){}
}
void LatePanel::unbind(const char* why){
    source_.clear();if(!attached_)return;
    attached_=false;++detaches_;
    if(!overlay_.detach()){++detach_failures_;withdrawn_=true;
        withdrawn_note_=std::string("withdrawn: the panel's own GPU work did not complete while detaching (")+why+"); control through overglazectl";note(withdrawn_note_);}
}
bool LatePanel::follow(IDXGISwapChain* chain)noexcept{
    std::lock_guard lock(mutex_);
    try{
        switch(chains_.present(chain)){
        case ChainTracker::Verdict::followed:return true;
        case ChainTracker::Verdict::other:return false;
        case ChainTracker::Verdict::take_over:break;}
        ComPtr<IDXGISwapChain3> c3;std::string why;
        if(FAILED(chain->QueryInterface(IID_PPV_ARGS(&c3))))why="not-a-swapchain3";
        else if(evidence_.accept&&!evidence_.accept(c3.Get(),why)&&why.empty())why="refused";
        if(!why.empty()){chains_.refuse(chain,why);++refused_take_overs_;note("take-over refused: "+why);return false;}
        // The old chain is gone or silent: our binding to it ends here, before
        // anything is drawn on the new one. NR state is not touched.
        unbind("followed-chain-changed");
        if(evidence_.rebind)evidence_.rebind();
        chains_.adopt(chain);
        note("followed another swapchain: the adopted one stopped presenting (take-over "+std::to_string(chains_.adoptions()-1)+")");
        return true;
    }catch(const std::exception& e){try{note(std::string("follow: ")+e.what());}catch(...){}return false;}
    catch(...){return false;}
}
void LatePanel::bind(IDXGISwapChain* chain,bool may_attach,std::uint64_t now)noexcept{
    std::lock_guard lock(mutex_);
    try{
        if(!chain||chain!=chains_.followed()||withdrawn_)return;
        // Judged before the overlay draws this present: a contradicted queue is
        // never drawn on again, attached or about to be.
        if(evidence_.contradicted&&evidence_.contradicted(source_)){
            const bool was=attached_;unbind("contradicted");if(withdrawn_)return;
            if(evidence_.restart&&evidence_.restart(now)){++redecisions_;
                note(std::string(was?"detached":"not attached")+": the present-transition evidence contradicted the chosen queue; "
                     "re-deciding under the same rules (re-decision "+std::to_string(redecisions_)+")");}
            else{++refused_redecisions_;withdrawn_=true;
                withdrawn_note_="withdrawn: the present-transition evidence was contradicted again after the re-decision bound was spent; control through overglazectl";
                note(withdrawn_note_);}
            return;
        }
        if(attached_||!may_attach)return;
        ComPtr<IDXGISwapChain3> c3;if(FAILED(chain->QueryInterface(IID_PPV_ARGS(&c3))))return;
        auto a=evidence_.resolve?evidence_.resolve(c3.Get()):Answer{};
        if(!a.queue){note(a.note.empty()?"waiting-for-render-queue":a.note);return;}
        overlay_.attach(c3.Get(),a.queue.Get());
        if(!overlay_.bound()){note("the panel did not bind to this chain (it is bound to another live window)");return;}
        attached_=true;source_=a.source;++attaches_;note("attached ("+a.source+")");
        if(open_.exchange(false))overlay_.show();
    }catch(const std::exception& e){try{note(e.what());}catch(...){}}
    catch(...){}
}
bool LatePanel::attached()const{std::lock_guard lock(mutex_);return attached_;}
std::string LatePanel::state()const{std::lock_guard lock(mutex_);
    return withdrawn_?withdrawn_note_:attached_?std::string("attached"):notes_.empty()?std::string("waiting-for-render-queue"):notes_.back();}
std::string LatePanel::source()const{std::lock_guard lock(mutex_);return source_;}
json LatePanel::status()const{
    std::lock_guard lock(mutex_);json notes=json::array();for(const auto& n:notes_)notes.push_back(n);
    return {{"attached",attached_},{"withdrawn",withdrawn_},{"source",source_.empty()?json(nullptr):json(source_)},
        {"attaches",attaches_},{"detaches",detaches_},{"detach_failures",detach_failures_},
        {"redecisions",redecisions_},{"refused_redecisions",refused_redecisions_},{"refused_take_overs",refused_take_overs_},
        {"notes",notes},{"chains",chains_.status()},
        {"scope","late path only: the followed swapchain is an identity; the panel holds no reference to it or to its back buffers between presents"}};
}
}

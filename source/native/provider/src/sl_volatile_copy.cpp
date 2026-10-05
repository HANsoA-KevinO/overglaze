// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Copy-at-tag-time for eOnlyValidNow legacy tags; the rules are in
// lab_sl_volatile_copy.hpp. Nothing on the render thread allocates or throws.
#include "lab_sl_volatile_copy.hpp"
#include <thread>
namespace lab::slboundary {
using Microsoft::WRL::ComPtr;
namespace {
// Formats with no single-plane byte layout this copy may move as it is:
// unknown; the two-plane depth/stencil families (19-22, 44-47) and the typed
// depth formats a non-depth texture cannot carry (40 D32_FLOAT, 55 D16_UNORM);
// block-compressed (70-84, 94-99); packed oddities (66, 68, 69); planar/video.
bool format_refused(DXGI_FORMAT f) noexcept {
    const auto v=static_cast<unsigned>(f);
    return v==0||(v>=19&&v<=22)||(v>=44&&v<=47)||v==40||v==55||(v>=70&&v<=84)||(v>=94&&v<=99)||v==66||v==68||v==69||v>=100;
}
bool same_shape(const D3D12_RESOURCE_DESC& a,const D3D12_RESOURCE_DESC& b) noexcept {
    return a.Width==b.Width&&a.Height==b.Height&&a.Format==b.Format;
}
// The current COM reference count, without changing it. A holder that may
// still have GPU work on a texture holds a reference until that work retired.
ULONG references(ID3D12Resource* r) noexcept {r->AddRef();return r->Release();}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,0,before,after};return b;
}
bool same_device(ID3D12Resource* r,ID3D12GraphicsCommandList* l,ComPtr<ID3D12Device>& device) noexcept {
    ComPtr<ID3D12Device> ld;ComPtr<IUnknown> a,b;
    return SUCCEEDED(r->GetDevice(IID_PPV_ARGS(&device)))&&SUCCEEDED(l->GetDevice(IID_PPV_ARGS(&ld)))&&
        SUCCEEDED(device.As(&a))&&SUCCEEDED(ld.As(&b))&&a.Get()==b.Get();
}
const char* pool_name(unsigned p) noexcept {return p==1?"depth":p==2?"motion":"linear-depth";}
}
VolatileCopies::~VolatileCopies(){
    // Never destroy a copy texture whose last recording may not have executed.
    for(auto& r:pools_)for(auto& s:r.slots)(void)s.texture.Detach();
    for(auto& x:retired_)(void)x.texture.Detach();
}
std::unique_lock<std::mutex> VolatileCopies::lock() const noexcept {
    // Every holder copies fixed-size data only: a short bounded wait is cheap.
    std::unique_lock guard(mutex_,std::try_to_lock);
    for(unsigned spin=0;spin<256&&!guard.owns_lock();++spin){std::this_thread::yield();(void)guard.try_lock();}
    return guard;
}
bool VolatileCopies::idle(const Slot& s) noexcept {return s.texture&&!s.claimed&&references(s.texture.Get())==1;}
bool VolatileCopies::retire(Texture& t,std::uint64_t now) noexcept {
    if(!t)return true;
    for(auto& x:retired_)if(!x.texture){x.texture=std::move(t);x.present=presents_.load();x.tick=now;return true;}
    // Full: the caller keeps it in its slot (counted) -- never a release of
    // possibly in-flight memory, and never a texture let go of uncounted.
    return false;
}
bool VolatileCopies::tag_returned(const Call& c,const Bindings& b,PFun_slGetNativeInterface* native,std::optional<Call>& out) noexcept {
    if(c.api!=Api::tags||c.result!=sl::Result::eOk||stopped_.load())return false;
    const unsigned n=std::min<unsigned>(c.inputs.tag_count,static_cast<unsigned>(c.inputs.tags.size()));
    // Per role, the one tag of this call the binding layer keeps (Bindings::
    // returned): the last of the role, except that self-configuring a valid
    // hardware depth is not replaced by a linear one. Only that tag is copied:
    // two depth semantics in one call cost one copy and never fight over a pool.
    std::array<int,3> keep{-1,-1,-1};bool keep_valid=false;
    for(unsigned j=0;j<n;++j){const auto& t=c.inputs.tags[j];const int r=b.role_of(t.type);if(r<=0)continue;
        if(r==1&&b.self_configured()&&keep[1]>=0&&keep_valid&&
           Bindings::depth_rank_of(c.inputs.tags[keep[1]].type)>Bindings::depth_rank_of(t.type))continue;
        keep[r]=static_cast<int>(j);if(r==1)keep_valid=!t.null_resource&&t.native&&!t.issues;}
    // Input roles only: the output colour's refusal is the binding layer's own.
    unsigned volatile_tags=0,kept=0;
    for(unsigned j=0;j<n;++j){const auto& t=c.inputs.tags[j];const int r=b.role_of(t.type);
        if(r>0&&t.lifecycle==sl::eOnlyValidNow){++volatile_tags;kept+=keep[r]==static_cast<int>(j);}}
    if(!volatile_tags)return false;
    const auto tally=[&](CopyOutcome o){outcomes_[static_cast<unsigned>(o)]+=volatile_tags;};
    if(!wanted_.load()){tally(CopyOutcome::not_wanted);return false;} // zero cost: no copy, no Call copy
    if(!kept){tally(CopyOutcome::superseded);return false;}          // the binding keeps a durable tag of the role
    if(!b.stores_tag_call(c)){tally(CopyOutcome::call_not_stored);return false;} // the binding layer drops it anyway
    out.emplace(c);
    // Backing off: copied only inside a probe window; every other tag says why.
    const bool backing_off=backoff_.load()&&probe_.load()!=kProbeOpen;
    CopyOutcome list_problem=CopyOutcome::none;ComPtr<ID3D12GraphicsCommandList> list;
    if(backing_off)list_problem=CopyOutcome::backoff;
    else if(c.frame_scoped_tags)list_problem=CopyOutcome::frame_scoped;
    else if(!c.command)list_problem=CopyOutcome::no_command_buffer;
    else{
        // A Streamline proxy list is unwrapped to its native list, as at the
        // Evaluate; a buffer that does not resolve is refused, not assumed native.
        ComPtr<IUnknown> base;
        if(native){void* p=nullptr;try{if(native(c.command,&p)==sl::Result::eOk&&p)base.Attach(static_cast<IUnknown*>(p));}catch(...){base.Reset();}}
        else base=static_cast<IUnknown*>(c.command);
        if(!base||FAILED(base.As(&list))||list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT){list.Reset();list_problem=CopyOutcome::command_list_unusable;}
    }
    for(unsigned j=0;j<n;++j){auto& t=out->inputs.tags[j];const int r=b.role_of(t.type);
        if(r<=0||t.lifecycle!=sl::eOnlyValidNow)continue;
        const auto why=keep[r]!=static_cast<int>(j)?CopyOutcome::superseded:
            list_problem!=CopyOutcome::none?list_problem:copy(c,t,static_cast<unsigned>(r),list.Get());
        ++outcomes_[static_cast<unsigned>(why)];
        if(why==CopyOutcome::copied)++copied_[pool_of(static_cast<unsigned>(r),t.type)];else t.copy_refusal=static_cast<std::uint8_t>(why);
    }
    return true;
}
CopyOutcome VolatileCopies::copy(const Call& c,Tag& t,unsigned role,ID3D12GraphicsCommandList* list) noexcept {
    if(t.issues||t.null_resource||!t.native||t.resource_type!=sl::ResourceType::eTex2d)return CopyOutcome::invalid_tag;
    if(t.state==UINT32_MAX)return CopyOutcome::state_unknown;
    ComPtr<ID3D12Resource> source;
    // The tagged address is what the binding would hand on: only copy it if it IS the resource.
    if(FAILED(static_cast<IUnknown*>(t.native)->QueryInterface(IID_PPV_ARGS(&source)))||source.Get()!=static_cast<ID3D12Resource*>(t.native))
        return CopyOutcome::invalid_tag;
    const auto d=source->GetDesc();
    if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.DepthOrArraySize!=1||!d.Width||d.Width>16384||!d.Height||d.Height>16384)return CopyOutcome::not_a_texture;
    if(d.SampleDesc.Count!=1)return CopyOutcome::multisample;
    if(format_refused(d.Format))return CopyOutcome::format_unsupported;
    if(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)return CopyOutcome::simultaneous_access;
    if(d.Layout!=D3D12_TEXTURE_LAYOUT_UNKNOWN)return CopyOutcome::layout_unsupported;
    ComPtr<ID3D12Device> device;if(!same_device(source.Get(),list,device))return CopyOutcome::device_mismatch;
    D3D12_RESOURCE_DESC want{};want.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;want.Width=d.Width;want.Height=d.Height;
    want.DepthOrArraySize=1;want.MipLevels=1;want.Format=d.Format;want.SampleDesc.Count=1;want.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const unsigned pool=pool_of(role,t.type);
    Slot* slot=nullptr;
    {auto guard=lock();if(!guard.owns_lock())return CopyOutcome::lock_contention;
        if(!device_)device_=device;
        auto& r=pools_[pool];
        const bool current=r.shaped&&same_shape(r.shape,want);
        unsigned shaped=0;
        if(current)for(auto& s:r.slots){if(!s.texture||!same_shape(s.desc,want))continue;++shaped;
            // Never the newest copy of this pool: its tag may still be pending.
            if((r.latest&&s.serial==r.latest)||!idle(s))continue;
            if(!slot||s.serial<slot->serial)slot=&s;}
        if(!slot){r.needed=want;r.need=true;
            // The pool serves another shape: the worker changes it, at most once per hold.
            if(!current&&r.shaped)return CopyOutcome::awaiting_reshape;
            if(!shaped){r.want=std::max(r.want,2u);return CopyOutcome::awaiting_allocation;} // first use, or none allocated yet
            if(shaped<kSlots)r.want=std::max(r.want,shaped+1);
            return CopyOutcome::slots_busy;}
        slot->claimed=true;}
    // Recorded on the game's own list at its tag call, as Streamline records its
    // own copy there: subresource 0, from the state the game declared for it.
    auto* texture=slot->texture.Get();
    const auto declared=static_cast<D3D12_RESOURCE_STATES>(t.state);
    std::array<D3D12_RESOURCE_BARRIER,2> before{},after{};unsigned count=0;
    if(!(declared&D3D12_RESOURCE_STATE_COPY_SOURCE)){before[count]=transition(source.Get(),declared,D3D12_RESOURCE_STATE_COPY_SOURCE);
        after[count]=transition(source.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,declared);++count;}
    before[count]=transition(texture,kRestState,D3D12_RESOURCE_STATE_COPY_DEST);after[count]=transition(texture,D3D12_RESOURCE_STATE_COPY_DEST,kRestState);++count;
    list->ResourceBarrier(count,before.data());
    D3D12_TEXTURE_COPY_LOCATION to{},from{};to.pResource=texture;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.SubresourceIndex=0;
    from.pResource=source.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.SubresourceIndex=0;
    list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    list->ResourceBarrier(count,after.data());
    // COM identity of the list, for the same-list count only (never dereferenced).
    ComPtr<IUnknown> identity;(void)list->QueryInterface(IID_PPV_ARGS(&identity));
    // Recorded: this must complete, so it waits for the lock rather than lose it.
    std::lock_guard guard(mutex_);
    slot->serial=++serial_;slot->tag_call=c.id;slot->list=identity.Get();slot->claimed=false;pools_[pool].latest=slot->serial;
    // The game's own word is kept beside the copy (Tag::tagged_native/state).
    t.game_native=t.native;t.game_state=t.state;
    t.native=texture;t.state=kRestState;t.lab_copy=slot->serial;
    return CopyOutcome::copied;
}
CopyOutcome VolatileCopies::verify(unsigned role,const Tag& tag,std::uint64_t tag_call,const void* list) noexcept {
    if(role<1||role>2||!tag.lab_copy)return CopyOutcome::replaced_before_evaluate;
    ComPtr<IUnknown> identity;if(list)(void)static_cast<IUnknown*>(const_cast<void*>(list))->QueryInterface(IID_PPV_ARGS(&identity));
    auto guard=lock();if(!guard.owns_lock())return CopyOutcome::lock_contention;
    for(const auto& s:pools_[pool_of(role,tag.type)].slots)if(s.texture&&s.texture.Get()==tag.native&&s.serial==tag.lab_copy&&!s.claimed){
        if(s.tag_call!=tag_call)break;
        ++(identity&&s.list==identity.Get()?same_list_:other_list_);return CopyOutcome::none;}
    ++outcomes_[static_cast<unsigned>(CopyOutcome::replaced_before_evaluate)];
    return CopyOutcome::replaced_before_evaluate;
}
void VolatileCopies::evaluated(const Resolution& r,bool admitted) noexcept {
    if(!active_.load())return; // no copy texture ever existed: nothing was wasted
    const bool held=r.binding.resources[1].lab_copy||r.binding.resources[2].lab_copy;
    // A refusal of an input role's copy (allocation pending, slots busy...) heals
    // by itself: neither wasted nor a success.
    if(admitted){wasted_streak_=0;backoff_=false;}
    else if(held&&!(r.volatile_roles&6u)){++wasted_;if(++wasted_streak_>=kWastedStreak&&!backoff_.exchange(true))++backoffs_;}
    // A probe window spans exactly one Evaluate interval: it opens at one target
    // Evaluate's return and closes at the next one's.
    auto p=probe_.load();
    if(p==kProbeOpen)(void)probe_.compare_exchange_strong(p,kProbeNone);
    else if(p==kProbeRequested)(void)probe_.compare_exchange_strong(p,kProbeOpen);
}
void VolatileCopies::service(bool wanted,std::uint64_t now) noexcept {
    wanted_=wanted&&!stopped_.load();
    if(wanted_)unwanted_since_=0;else if(!unwanted_since_)unwanted_since_=now?now:1;
    // Unwanted (NR OFF on a ready runtime): the next wanted period starts clean.
    if(!wanted_){backoff_=false;probe_=kProbeNone;wasted_streak_=0;}
    else if(backoff_.load()&&probe_.load()==kProbeNone&&now-last_probe_>=kProbeMs){last_probe_=now;probe_=kProbeRequested;++probes_;}
    // Not wanted for a while (NR OFF), or stopped: every texture leaves service.
    const bool drop=!wanted_&&(stopped_.load()||now-unwanted_since_>=kHoldMs);
    struct Plan {unsigned pool=0;D3D12_RESOURCE_DESC desc{};unsigned count=0;};
    std::array<Plan,kPools> plans{};unsigned planned=0;ComPtr<ID3D12Device> device;
    std::array<Texture,kRetired> released{};unsigned freed=0;
    {std::lock_guard guard(mutex_);
        for(unsigned p=1;p<kPools;++p){auto& r=pools_[p];
            if(drop){bool kept=false;
                for(auto& s:r.slots)if(s.texture&&!s.claimed){if(retire(s.texture,now))s={};else{++retire_deferred_;kept=true;}}else kept|=bool(s.texture);
                if(!kept)r.shaped=false;r.need=false;r.want=0;r.latest=0;continue;}
            if(!wanted_||!r.need)continue;
            if(!r.shaped||!same_shape(r.shape,r.needed)){
                // A new shape for this pool, at most once per hold: two resources of
                // one semantic that alternate never churn textures.
                if(r.shaped&&now-r.changed<kHoldMs){++reshape_holds_;continue;}
                if(r.shaped)++reshapes_;r.shape=r.needed;r.shaped=true;r.changed=now;r.want=2;r.latest=0;}
            unsigned have=0,free=0;
            for(auto& s:r.slots){if(!s.texture){++free;continue;}
                if(same_shape(s.desc,r.shape)){++have;continue;}
                if(s.claimed)continue; // being recorded: next poll
                // Another shape: out of service into the hold, or kept in its slot while the hold is full.
                if(retire(s.texture,now)){s={};++free;}else ++retire_deferred_;}
            const unsigned target=std::min(kSlots,std::max(2u,r.want));
            const unsigned count=std::min(free,have<target?target-have:0u);
            if(count)plans[planned++]={p,r.shape,count};else if(have>=target)r.need=false;}
        device=device_;
        for(auto& x:retired_)if(x.texture&&presents_.load()-x.present>=kHoldPresents&&now-x.tick>=kHoldMs&&references(x.texture.Get())==1){
            released[freed++]=std::move(x.texture);x={};}
    }
    for(unsigned i=0;i<freed;++i){released[i].Reset();++releases_;}
    if(!planned||!device)return;
    // The worker allocates; the render thread only ever uses what is installed.
    std::array<std::array<Texture,kSlots>,kPools> made{};
    for(unsigned p=0;p<planned;++p){const auto& plan=plans[p];
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        for(unsigned i=0;i<plan.count;++i){
            if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&plan.desc,kRestState,nullptr,IID_PPV_ARGS(&made[p][i])))){
                made[p][i].Reset();++allocation_failures_;continue;}
            ++allocations_;allocated_bytes_+=device->GetResourceAllocationInfo(0,1,&plan.desc).SizeInBytes;}}
    {std::lock_guard guard(mutex_);
        for(unsigned p=0;p<planned;++p){auto& r=pools_[plans[p].pool];
            for(auto& t:made[p]){if(!t)continue;
                Slot* free=nullptr;for(auto& s:r.slots)if(!s.texture){free=&s;break;}
                // The shape moved on while we allocated, or no room: never installed. Such
                // a texture was never on any list; it is released below, outside the lock.
                if(!free||!r.shaped||!same_shape(plans[p].desc,r.shape)||!wanted_.load())continue;
                free->texture=std::move(t);free->desc=plans[p].desc;free->serial=0;free->tag_call=0;free->list=nullptr;free->claimed=false;}
            unsigned have=0;for(const auto& s:r.slots)have+=s.texture&&same_shape(s.desc,r.shape);
            if(have>=std::min(kSlots,std::max(2u,r.want)))r.need=false;}
        active_=true;}
    made={}; // uninstalled textures: never recorded anywhere, so released at once
}
json VolatileCopies::describe() const {
    // Idle (no OnlyValidNow input tag ever seen, nothing allocated): one key, so
    // a game that never declares OnlyValidNow pays nothing in the bounded snapshot.
    json out={{"wanted",wanted_.load()}};
    json refused=json::object();
    for(unsigned i=static_cast<unsigned>(CopyOutcome::not_wanted);i<static_cast<unsigned>(CopyOutcome::count);++i)
        if(const auto n=outcomes_[i].load())refused[copy_outcome_name(static_cast<CopyOutcome>(i))]=n;
    const auto depth=copied_[1].load(),motion=copied_[2].load(),linear=copied_[3].load();
    if(refused.empty()&&!depth&&!motion&&!linear&&!allocations_.load()&&!allocation_failures_.load())return out;
    json slots=json::object();unsigned retired=0;
    {std::lock_guard guard(mutex_);
        for(unsigned p=1;p<kPools;++p){unsigned count=0;D3D12_RESOURCE_DESC d{};
            for(const auto& s:pools_[p].slots)if(s.texture){++count;d=s.desc;}
            if(count)slots[pool_name(p)]=json::array({count,d.Width,d.Height,static_cast<unsigned>(d.Format)});}
        for(const auto& x:retired_)retired+=x.texture?1u:0u;}
    json copied={{"depth",depth},{"motion",motion}};if(linear)copied["linear-depth"]=linear;
    out.update({{"copied",copied},{"refused",refused},
        {"bound_same_list",same_list_.load()},{"bound_other_list",other_list_.load()},
        {"slots",slots},{"slots_columns",{"count","width","height","dxgi_format"}},
        {"allocations",allocations_.load()},{"allocation_failures",allocation_failures_.load()},{"allocated_bytes",allocated_bytes_.load()},
        {"retired_pending",retired},{"releases",releases_.load()},{"retire_deferred_hold_full",retire_deferred_.load()},
        {"scope","eOnlyValidNow depth/motion copied on the game's slSetTag list at that call, as Streamline does; the output colour never"}});
    // Only once they happened: the snapshot bound is shared.
    if(const auto n=reshapes_.load())out["reshapes"]=n;
    if(const auto n=reshape_holds_.load())out["reshape_holds"]=n;
    if(const auto n=wasted_.load()){out["wasted_evaluates"]=n;out["backoff"]={{"active",backoff_.load()},{"entered",backoffs_.load()},{"probes",probes_.load()}};}
    return out;
}
}

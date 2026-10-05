// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_sl_boundary.hpp"
#include <Windows.h>
#include <type_traits>

namespace lab::slboundary {
namespace {
bool bytes(void* to,const void* from,std::size_t size) noexcept {
    if(!from)return false;
    __try {memcpy(to,from,size);return true;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
        ?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return false;}
}
template<class T> bool read(T& to,const void* from) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);return bytes(&to,from,sizeof(T));
}
bool header(const sl::BaseStructure& a,const sl::BaseStructure& b) noexcept {
    return a.structType==b.structType && a.structVersion==b.structVersion && a.next==b.next;
}
template<class T> bool typed(T& value,const void* pointer,const sl::BaseStructure& base,Inputs& out) noexcept {
    if(!read(value,pointer) || !header(value,base)){out.issues|=fault;return false;}return true;
}
void viewport(Inputs& out,const void* ptr,const sl::BaseStructure& base) noexcept {
    if(out.viewport_present){out.issues|=duplicate;return;}out.viewport_present=true;
    if(base.structVersion!=sl::kStructVersion1){out.issues|=version;return;}
    sl::ViewportHandle value;
    if(typed(value,ptr,base,out)){out.viewport=static_cast<std::uint32_t>(value);if(out.viewport==UINT32_MAX)out.issues|=invalid;}
}
void constants(Inputs& out,const void* ptr,const sl::BaseStructure& base) noexcept {
    if(out.constants_present){out.issues|=duplicate;return;}out.constants_present=true;
    // Only the version compiled in our pinned headers. In particular, never
    // read sizeof(v2) from a shorter v1 object and invent optional tail data.
    if(base.structVersion!=sl::kStructVersion2){out.issues|=version;return;}
    if(typed(out.constants,ptr,base,out))out.constants.next=nullptr;
}
void tag(Inputs& out,const void* ptr,const sl::BaseStructure& base) noexcept {
    if(out.tag_count==out.tags.size()){out.issues|=bound;out.call_issues|=bound;return;}
    if(base.structVersion!=sl::kStructVersion1){out.issues|=version;out.call_issues|=version;return;}
    sl::ResourceTag value;if(!typed(value,ptr,base,out)){out.call_issues|=fault;return;}
    auto& t=out.tags[out.tag_count++];t.type=value.type;t.lifecycle=value.lifecycle;t.extent=value.extent;
    for(unsigned i=0;i+1<out.tag_count;++i)if(out.tags[i].type==t.type)t.issues|=duplicate;
    if(t.lifecycle!=sl::eOnlyValidNow && t.lifecycle!=sl::eValidUntilPresent && t.lifecycle!=sl::eValidUntilEvaluate)t.issues|=invalid;
    if((!t.extent.width)!=(!t.extent.height) || (!t.extent.width&&(t.extent.left||t.extent.top)))t.issues|=invalid;
    if(!value.resource){t.null_resource=true;out.issues|=t.issues;return;}
    sl::BaseStructure b({},0);sl::Resource r;
    if(!read(b,value.resource))t.issues|=fault;
    else if(b.structType!=sl::Resource::s_structType)t.issues|=invalid;
    else if(b.structVersion!=sl::kStructVersion1)t.issues|=version;
    else if(!read(r,value.resource)||!header(r,b))t.issues|=fault;
    else {
        t.native=r.native;t.resource_type=r.type;t.state=r.state;
        if(!r.native || r.state==UINT32_MAX || r.type!=sl::ResourceType::eTex2d)t.issues|=invalid;
        if(r.next)t.issues|=extension;
    }
    out.issues|=t.issues;
}
void direct_viewport(Inputs& out,const sl::ViewportHandle& v) noexcept {
    sl::BaseStructure base({},0);
    if(!read(base,&v)){out.issues|=fault;return;}
    if(base.structType!=sl::ViewportHandle::s_structType){out.issues|=invalid;return;}
    viewport(out,&v,base);if(base.next)out.issues|=extension;
}
}
bool read_pointer(void*& out,const void* source) noexcept{return read(out,source);}
Inputs decode_inputs(const sl::BaseStructure** input,std::uint32_t count) noexcept {
    // call_issues gets everything EXCEPT a problem confined to one inline tag
    // (that stays in the tag's own issues, and in issues): the chain, the
    // viewport, constants and any structure we do not know are the call's.
    Inputs out;
    const auto call_level=[&](unsigned bits){out.issues|=bits;out.call_issues|=bits;};
    // Run a helper that only knows `issues`, and attribute what it adds to the call.
    const auto as_call=[&](auto&& decode){const unsigned saved=out.issues;out.issues=0;decode();out.call_issues|=out.issues;out.issues|=saved;};
    if(count>16){call_level(bound);return out;}
    std::array<const sl::BaseStructure*,32> seen{};unsigned visited=0;
    for(unsigned i=0;i<count;++i){const sl::BaseStructure* p=nullptr;
        if(!read(p,input?input+i:nullptr)){call_level(fault);return out;}
        while(p){
            for(unsigned j=0;j<visited;++j)if(seen[j]==p){call_level(cycle);return out;}
            if(visited==seen.size()){call_level(bound);return out;}seen[visited++]=p;
            sl::BaseStructure b({},0);if(!read(b,p)){call_level(fault);return out;}
            if(b.structType==sl::ViewportHandle::s_structType)as_call([&]{viewport(out,p,b);});
            else if(b.structType==sl::Constants::s_structType)as_call([&]{constants(out,p,b);});
            else if(b.structType==sl::ResourceTag::s_structType)tag(out,p,b); // sets its own call-level bits
            else call_level(unknown_input);
            p=b.next;
        }
    }
    if(!out.viewport_present)call_level(invalid);return out;
}
Inputs decode_tags(const sl::ViewportHandle& v,const sl::ResourceTag* tags,std::uint32_t count) noexcept {
    Inputs out;direct_viewport(out,v);out.call_issues|=out.issues;
    if(count>16){out.issues|=bound;out.call_issues|=bound;return out;}
    for(unsigned i=0;i<count;++i){sl::BaseStructure base({},0);const auto* p=tags?tags+i:nullptr;
        if(!read(base,p)){out.issues|=fault;out.call_issues|=fault;break;}
        if(base.structType!=sl::ResourceTag::s_structType){out.issues|=invalid;out.call_issues|=invalid;continue;}
        tag(out,p,base);if(base.next){out.issues|=extension;out.call_issues|=extension;}
    }
    return out;
}
Inputs decode_constants(const sl::ViewportHandle& v,const sl::Constants& c) noexcept {
    Inputs out;direct_viewport(out,v);sl::BaseStructure base({},0);
    if(!read(base,&c))out.issues|=fault;
    else if(base.structType!=sl::Constants::s_structType)out.issues|=invalid;
    else {constants(out,&c,base);if(base.next)out.issues|=extension;}
    out.call_issues=out.issues;return out;
}
}

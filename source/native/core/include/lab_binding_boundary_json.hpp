// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include "lab_binding_boundary.hpp"
namespace lab::diagnostic {
inline json binding_boundary_json(const BindingBoundaryReport& r){
    const char* names[]{"outer-SL-entry","RR-begin-callback-return","RR-end-callback-return","outer-SL-return"};
    const auto bank=[](const BindingBankSnapshot& b){return json{{"root",b.root},{"table_mask",b.tables},
        {"descriptor_mask",b.descriptors},{"constant_mask",b.constants},{"constant_descriptor_fingerprint",b.constant_hash},
        {"table_addresses_by_slot",b.table_addresses}};};
    json rows=json::array();
    for(unsigned i=0;i<std::min<unsigned>(r.count,static_cast<unsigned>(r.calls.size()));++i){const auto& c=r.calls[i];json points=json::array();
        for(unsigned j=0;j<4;++j)if(c.seen&(1u<<j)){const auto& p=c.points[j];const auto& e=p.event;const auto& b=p.bindings;
            points.push_back({{"point",names[j]},{"call",e.call},{"frame",e.frame},{"viewport",e.viewport},{"thread",e.thread},
                {"command",e.command},{"metadata_valid",bool(e.valid)},{"SL_tag_resources",e.resources},{"SL_declared_states",e.declared_states},
                {"tracked",bool(b.tracked)},{"insertion_ready",bool(b.ready)},{"reason",b.reason},{"recording_generation",b.generation},
                {"heaps",b.heaps},{"heap_count",b.heap_count},{"heap_permutations",b.heap_permutations},
                {"pipeline",b.pipeline},{"raytracing_pipeline",b.raytracing_pipeline},{"compute",bank(b.compute)},{"graphics",bank(b.graphics)}});}
        json changes=json::array();
        for(unsigned j=1;j<4;++j)if((c.seen&(3u<<(j-1)))==(3u<<(j-1))){
            const auto& before=c.points[j-1].bindings;const auto& after=c.points[j].bindings;
            bool same_set=before.heap_count==after.heap_count;
            for(unsigned h=0;h<std::min(after.heap_count,2u);++h){bool found=false;
                for(unsigned k=0;k<std::min(before.heap_count,2u);++k)found|=after.heaps[h]==before.heaps[k];same_set&=found;}
            auto delta=[](const BindingBankSnapshot& a,const BindingBankSnapshot& b){return json{
                {"root_changed",a.root!=b.root},{"lost_table_mask",a.tables&~b.tables},{"added_table_mask",b.tables&~a.tables},
                {"table_addresses_changed",a.table_addresses!=b.table_addresses},{"constant_descriptor_fingerprint_changed",a.constant_hash!=b.constant_hash}};};
            changes.push_back({{"from",names[j-1]},{"to",names[j]},{"heap_set_changed",!same_set},
                {"heap_order_only_changed",same_set&&before.heaps!=after.heaps},{"compute",delta(before.compute,after.compute)},
                {"graphics",delta(before.graphics,after.graphics)}});
        }
        rows.push_back({{"complete",bool(c.complete)},{"invalid",bool(c.invalid)},{"seen_mask",c.seen},{"points",points},{"adjacent_changes",changes}});
    }
    return {{"state",binding_audit_name(r.phase)},{"started_ms",r.started_ms},{"deadline_ms",r.deadline_ms},
        {"complete_calls",r.complete_calls},{"callback_losses",r.callback_losses},{"pre_entry_callbacks",r.pre_entry_callbacks},{"call_limit",4},{"storage_bytes",sizeof(r)},
        {"rows",rows},{"raw_files",0},{"GPU_commands_by_inspection",0},{"NR_evaluates_by_inspection",0},
        {"scope","tracked binding metadata only; RR callback boundaries are NOT exact NGX entry/return; no resource barrier or pixel proof"},
        {"visual_fix_verified",false},{"p0_gate_open",false}};
}
}

// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_model_setup.hpp"
#include <iostream>
namespace {unsigned checks=0;void need(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}}
int main(){std::filesystem::path fixture;
    try{
        lab::ModelSetupState pending;
        need(pending.should_prompt(true,true,false),"Fresh normal app with missing model prompts");
        need(!pending.should_prompt(false,true,false),"Fixture mode never prompts");
        need(!pending.should_prompt(true,false,false),"No prompt before model check completes");
        need(!pending.should_prompt(true,true,true),"Known model avoids prompt");
        for(const auto decision:{lab::ModelSetupState::Decision::skipped,lab::ModelSetupState::Decision::completed}){
            lab::ModelSetupState s;s.decision=decision;const auto restored=lab::ModelSetupState::parse(s.document());
            need(restored.decision==decision&&!restored.should_prompt(true,true,false),"Acknowledgement suppresses repeat prompts, without asserting model readiness");}
        need(lab::ModelSetupState::parse(pending.document()).should_prompt(true,true,false),"Pending roundtrip preserves first-run setup");
        for(const auto& invalid:{lab::json(),lab::json{{"kind","overglaze-model-setup"},{"version",2},{"decision","skipped"}},lab::json{{"kind","overglaze-model-setup"},{"version",1},{"decision","ready"}}}){
            bool rejected=false;try{lab::ModelSetupState::parse(invalid);}catch(...){rejected=true;}need(rejected,"Unknown setup metadata is not silently promoted");}
        lab::ModelSetupPreferences disabled;need(!disabled.enabled(),"Disabled fixture preferences do no I/O");disabled.remember(lab::ModelSetupState::Decision::skipped);
        need(disabled.state.decision==lab::ModelSetupState::Decision::skipped&&disabled.error.empty(),"Disabled fixture may exercise state without a settings write");
        fixture=std::filesystem::temp_directory_path()/lab::wide("overglaze-model-setup-test-"+lab::uuid());std::filesystem::create_directory(fixture);
        {lab::ModelSetupPreferences store(fixture);need(store.enabled()&&store.state.should_prompt(true,true,false),"New isolated data root enables setup");store.remember(lab::ModelSetupState::Decision::skipped);need(store.error.empty(),"Skip persisted atomically");}
        {lab::ModelSetupPreferences store(fixture);need(store.state.decision==lab::ModelSetupState::Decision::skipped,"Next launch remembers skip");store.remember(lab::ModelSetupState::Decision::completed);need(store.error.empty(),"Completion persisted");}
        {lab::ModelSetupPreferences store(fixture);need(store.state.decision==lab::ModelSetupState::Decision::completed&&!store.state.should_prompt(true,true,false),"Completion does not falsely claim a currently available model");}
        // Every file and directory below this UUID fixture was created by this test.
        std::filesystem::remove_all(fixture);fixture.clear();std::cout<<"Model setup: "<<checks<<" checks passed\n";return 0;
    }catch(const std::exception& e){if(!fixture.empty())std::filesystem::remove_all(fixture);std::cerr<<e.what()<<'\n';return 1;}
}

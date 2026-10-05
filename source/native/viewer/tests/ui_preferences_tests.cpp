// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_ui_preferences.hpp"
#include "lab_display_selection.hpp"
#include <fstream>
#include <iostream>
namespace {void need(bool v,const char* why){if(!v)throw std::runtime_error(why);}}
int main(){try{
    lab::ViewerPreferences v;v.view=1;v.wipe=.27f;v.display.exposure=-1.25f;v.display.white_nits=240;v.hdr=true;
    need(lab::ViewerPreferences::parse(v.document()).document()==v.document(),"Viewer preference roundtrip");
    for(unsigned i=0;i<13;++i){auto bad=v.document();if(i==0)bad["version"]=5;if(i==1)bad["exposure"]=7;if(i==2)bad["hdr"]=1;if(i==3)bad["view"]=.5;
        if(i==4)bad["wipe"]=-.1;if(i==5)bad["curve"]=5;if(i==6)bad["white_nits"]=0;if(i==7)bad["nr_on"]=true;if(i==8)bad["exposure"]=nullptr;
        if(i==9)bad["mapping"]=4;if(i==10)bad["contrast"]=2;if(i==11)bad["mapping"]=.5;if(i==12)bad["processing"]=1;
        bool rejected=false;try{lab::ViewerPreferences::parse(bad);}catch(...){rejected=true;}need(rejected,"Invalid preferences rejected, not coerced");}
    lab::OverlayPreferences overlay;overlay.hotkey=VK_F8;overlay.white=180;need(lab::OverlayPreferences::parse(overlay.document()).hotkey==VK_F8,"Supported persistent key");
    {auto aces=v;aces.display.mapping=3;need(lab::ViewerPreferences::parse(aces.document()).display.mapping==3,"ACES v3 roundtrip");
        auto v2=v.document();v2.erase("processing");v2["version"]=2;v2["mapping"]=1;need(lab::ViewerPreferences::parse(v2).display.mapping==1,"v2 mapping preserved");
        v2["mapping"]=3;bool denied=false;try{lab::ViewerPreferences::parse(v2);}catch(...){denied=true;}need(denied,"v2 must not silently gain new enum values");}
    auto bad=overlay.document();bad["hotkey"]=VK_F4;bool rejected=false;try{lab::OverlayPreferences::parse(bad);}catch(...){rejected=true;}need(rejected,"Unsafe key rejected");
    lab::OverlayPreferences model;model.has_model=true;model.tone=.25f;model.structure=.75f;model.style=2;model.exposure_stops=5.5f;model.exposure_auto=1;
    const auto restored=lab::OverlayPreferences::parse(model.document());
    need(restored.has_model&&restored.tone==.25f&&restored.structure==.75f&&restored.style==2&&restored.exposure_stops==5.5f&&restored.exposure_auto==1,"Model controls round-trip");
    {lab::OverlayPreferences wide=model;wide.tone=1.85f;wide.structure=2.f;wide.skin=.25f;wide.automask=1;const auto doc=wide.document();
     need(doc.at("version")==3,"Version 3 writes Skin/AutoMask");const auto back=lab::OverlayPreferences::parse(doc);
     need(back.tone==1.85f&&back.structure==2.f&&back.skin==.25f&&back.automask==1,"Version 3 round-trip (Tone/Structure 0..2, Skin, AutoMask)");
     auto v2=doc;v2["version"]=2;v2.erase("skin");v2.erase("automask");const auto old=lab::OverlayPreferences::parse(v2);
     need(old.has_model&&old.tone==1.85f&&old.skin==1.f&&old.automask==0,"Version 2 files still load; Skin defaults to 1, mask off");
     auto bad=doc;bad["skin"]=3;bool refused=false;try{(void)lab::OverlayPreferences::parse(bad);}catch(const std::exception&){refused=true;}need(refused,"Skin above 2 refused");}
    need(!lab::OverlayPreferences::parse(overlay.document()).has_model,"Version 1 file restores no model controls");
    // The persisted exposure range is the slider's own (-12..10).
    {auto deep=model;deep.exposure_stops=-9.5f;need(lab::OverlayPreferences::parse(deep.document()).exposure_stops==-9.5f,"Exposure below the old -6 floor persists");
     auto too_deep=model.document();too_deep["exposure_stops"]=-12.5;bool refused=false;try{lab::OverlayPreferences::parse(too_deep);}catch(...){refused=true;}
     need(refused,"Exposure beyond the slider range rejected");}
    auto bad_model=model.document();bad_model["style"]=3;rejected=false;try{lab::OverlayPreferences::parse(bad_model);}catch(...){rejected=true;}need(rejected,"Out-of-range persisted Style rejected");
    bad_model=model.document();bad_model["exposure_stops"]=99;rejected=false;try{lab::OverlayPreferences::parse(bad_model);}catch(...){rejected=true;}need(rejected,"Out-of-range persisted exposure rejected");
    need(!model.document().contains("nr_mode")&&!model.document().contains("enabled"),"The NR gate is never persisted");
    // A temporary data root (host contract V4: the data root is the caller's,
    // never a compiled path), with the preferences one level below it.
    std::wstring temp(MAX_PATH+1,L'\0');temp.resize(GetTempPathW(DWORD(temp.size()),temp.data()));
    const auto base=std::filesystem::path(temp).lexically_normal()/("overglaze-preferences-test-"+std::to_string(GetCurrentProcessId()));need(std::filesystem::create_directory(base),"New fixture data root");
    const auto root=base/("preferences-functional-"+std::to_string(GetCurrentProcessId()));need(std::filesystem::create_directory(root),"New fixture directory");
    const auto file=root/L"viewer.json";{
        lab::UiPreferencesFile store(file,lab::ViewerPreferences{}.document());need(store.error().empty(),"Preference worker opened");
        {lab::UiPreferencesFile other(file,v.document());need(!other.error().empty(),"Second preference writer denied");}
        for(unsigned i=0;i<1000;++i){v.wipe=(i%101)/100.f;store.save(v.document());}v.wipe=.27f;store.save(v.document());store.close();need(store.error().empty(),"Coalesced worker flushed latest update");}
    {lab::UiPreferencesFile store(file,lab::ViewerPreferences{}.document());need(store.loaded()==v.document()&&store.error().empty(),"Next process restores complete latest display state");}
    {auto legacy=v.document();legacy["version"]=1;legacy.erase("processing");legacy.erase("mapping");legacy.erase("contrast");{std::ofstream out(file);out<<legacy.dump();}
        const auto before=lab::sha256(file);const auto destination=root/L"viewer-v2.json";
        {lab::UiPreferencesFile store(destination,lab::ViewerPreferences{}.document(),file);auto migrated=lab::ViewerPreferences::parse(store.loaded());need(migrated.display.mapping==2&&migrated.display.exposure==v.display.exposure&&migrated.wipe==v.wipe,"Legacy preferences preserve rendering, exposure and wipe");migrated.display.mapping=0;store.save(migrated.document());store.close();need(store.error().empty(),"Migrated preferences write");}
        need(lab::sha256(file)==before,"Legacy preference file unchanged");{lab::UiPreferencesFile store(destination,v.document(),file);need(store.loaded()["mapping"]==0,"New preferences take precedence over legacy");}
        need(std::filesystem::remove(destination)&&std::filesystem::remove(destination.wstring()+L".lock"),"Clean migration fixture");}
    {auto old=v.document();old.erase("processing");old["version"]=2;old["mapping"]=1;{std::ofstream out(file);out<<old.dump();}
        const auto before=lab::sha256(file);const auto destination=root/L"viewer-v3.json";
        {lab::UiPreferencesFile store(destination,v.document(),file);auto migrated=lab::ViewerPreferences::parse(store.loaded());need(migrated.display.mapping==1&&migrated.display.exposure==v.display.exposure,"v2 inherited without changing display");
            migrated.display.mapping=3;store.save(migrated.document());store.close();need(store.error().empty(),"v3 ACES preferences written");}
        need(lab::sha256(file)==before,"v2 file remains byte-identical");
        need(std::filesystem::remove(destination)&&std::filesystem::remove(destination.wstring()+L".lock"),"Clean v3 fixture");}
    {auto old=v.document();old.erase("processing");old["version"]=3;old["mapping"]=3;{std::ofstream out(file);out<<old.dump();}
        const auto before=lab::sha256(file);const auto destination=root/L"viewer-v4.json";
        {lab::UiPreferencesFile store(destination,v.document(),file);auto migrated=lab::ViewerPreferences::parse(store.loaded());
            need(migrated.display.mapping==3&&migrated.processing,"v3 display preserved, processing remains enabled");migrated.processing=false;
            store.save(migrated.document());store.close();need(store.error().empty(),"v4 bypass state written");}
        {lab::UiPreferencesFile store(destination,v.document());const auto restored=lab::ViewerPreferences::parse(store.loaded());
            need(!restored.processing&&restored.display.mapping==3&&restored.display.exposure==v.display.exposure,"Bypass retains the saved recipe across restart");}
        need(lab::sha256(file)==before,"v3 original unchanged");need(std::filesystem::remove(destination)&&std::filesystem::remove(destination.wstring()+L".lock"),"Clean v4 fixture");}
    {std::ofstream broken(file);broken<<"{broken";}
    const auto old=lab::sha256(file);{lab::UiPreferencesFile store(file,lab::ViewerPreferences{}.document());need(!store.error().empty(),"Corrupt preference shown");store.save(v.document());}
    need(lab::sha256(file)==old,"Corrupt original never silently overwritten");
    {lab::UiPreferencesFile invalid(root.parent_path()/L"escaped.json",v.document(),{},base);need(!invalid.error().empty(),"Broad root rejected before file write");}
    // Preferences written before the rename carry the old kind: read under the new one.
    {const auto legacy=root/L"legacy-overlay.json";auto doc=lab::OverlayPreferences{}.document();doc["kind"]="dlsslab-overlay-preferences";
     {std::ofstream out(legacy,std::ios::binary);out<<doc.dump(2);}
     {lab::UiPreferencesFile store(legacy,lab::OverlayPreferences{}.document(),{},base);need(store.error().empty()&&store.loaded()["kind"]=="overglaze-overlay-preferences","Legacy overlay preferences read under the new kind");}
     need(std::filesystem::remove(legacy),"Cleanup legacy fixture");std::error_code ec;std::filesystem::remove(root/L"legacy-overlay.json.lock",ec);}
    need(lab::display_intersection({-1920,0,0,1080},{-100,100,500,800})==70000,"Negative monitor coordinates");
    need(lab::display_intersection({0,0,1920,1080},{-100,100,500,800})==350000,"Largest overlap chooses second display");
    need(lab::display_intersection({0,0,10,10},{10,0,20,10})==0,"No overlap is not an HDR output");
    for(const auto& entry:std::filesystem::directory_iterator(root)){need(entry.path().filename()==L"viewer.json"||entry.path().filename()==L"viewer.json.lock","No abandoned update file");need(std::filesystem::remove(entry.path()),"Cleanup own preference fixture");}need(std::filesystem::remove(root),"Remove empty fixture");need(std::filesystem::remove(base),"Remove empty fixture data root");
    std::cout<<"PASS typed display-only preferences, 1000 coalesced edits, reload, single writer, corrupt file preservation and output intersection; temporary files removed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

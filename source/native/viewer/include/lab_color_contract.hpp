// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include "lab_viewer_color.hpp"

// Viewing/export only. This header is not linked into the NR runner or host.
namespace lab::color {
// The colour interpretation a frame-pair capture records for each of its four
// RGBA16F stages (rr-output, nr-input, nr-output, hdr-return). Captures written
// before these neutral names spell stages 1 and 3 after the reference
// implementation the pre/post-ratio wrapper follows; the maths is identical, so
// those exact strings are read as aliases and older captures still open.
// Nothing writes the legacy spellings.
inline constexpr const char* kFramePairInterpretations[]{"admitted-RR-linear-working-RGB","prepost-operator3-sRGB-encoded-input",
    "raw-NR-values-interpreted-by-wrapper-as-sRGB","linear-working-RGB-after-prepost-ratio-reconstruction"};
inline constexpr const char* kLegacyFramePairInput="NBA-operator3-sRGB-encoded-input";
inline constexpr const char* kLegacyFramePairReturn="linear-working-RGB-after-NBA-ratio-reconstruction";
inline bool frame_pair_interpretation(unsigned stage,const std::string& value){
    if(stage>=4)return false;
    return value==kFramePairInterpretations[stage]||(stage==1&&value==kLegacyFramePairInput)||(stage==3&&value==kLegacyFramePairReturn);
}
enum class Stage : unsigned { rr=0, prepared=1, api_output=2, reconstructed=3, final_sdr=4, unknown=255 };
struct Contract {
    Stage stage=Stage::unknown;
    std::string role, interpretation;
    bool reconstruction_available=false;
    bool known()const{return stage!=Stage::unknown;}
    bool interface_image()const{return stage==Stage::prepared||stage==Stage::api_output;}
    bool final()const{return stage==Stage::final_sdr;}
    const char* label()const{
        switch(stage){
        case Stage::rr:return "01  RR 工作图（NR 前转换之前）";
        case Stage::prepared:return "02  NR 实际输入（已完成前转换）";
        case Stage::api_output:return "03  NR API 原始输出（未做回填）";
        case Stage::reconstructed:return "04  NR 后回填工作图（非模型直出）";
        case Stage::final_sdr:return "05  游戏最终 SDR（保留显示码值）";
        default:return "未知颜色契约 · 仅保留原始数据";
        }
    }
    json document()const{return {{"version","dlsslab-color-contract-v1"},{"source_stage",role},{"recorded_interpretation",interpretation},
        {"recognized",known()},{"evidence",!known()?"unknown":final()?"recorded-swapchain-G22-P709":"recorded-adapter-contract-not-calibration"},
        {"primaries",final()?"Rec.709/D65":"unknown"},{"transfer",!known()?"unknown":final()?"DXGI-G22-display-codes":interface_image()?"sRGB-transfer-as-used-by-wrapper":"adapter-relative-linear-working-RGB"},
        {"nits_per_unit",nullptr},{"game_display_transform_known",false},{"final_display_captured",final()},{"pure_neural_tensor",false},
        {"source_reconstruction_applied_by_viewer",false},{"view_reconstruction_available",reconstruction_available},{"source_values_modified",false}};}
};
inline Contract resolve(const json& manifest,unsigned index){
    const auto& row=manifest.at("stages").at(index);Contract c;
    c.role=row.value("stage","");c.interpretation=row.value("color_interpretation",row.value("interpretation",""));
    const auto kind=manifest.value("kind","");
    if(kind=="dlsslab-frame-pair-v1"&&row.value("format","")=="R16G16B16A16_FLOAT"){
        constexpr const char* names[]{"rr-output","nr-input","nr-output","hdr-return"};
        for(unsigned i=0;i<4;++i)if(c.role==names[i]&&frame_pair_interpretation(i,c.interpretation))c.stage=Stage(i);
    }else if(kind=="dlsslab-display-pair-v1"){
        if(c.role=="rr-output"&&row.value("dxgi_format",0u)==10&&c.interpretation=="RR-working-values-no-display-transform")c.stage=Stage::rr;
        const auto format=row.value("dxgi_format",0u);
        if(c.role=="pre-present-sdr"&&(format==28||format==87)&&c.interpretation=="raw-UNORM-display-codes-no-additional-gamma"&&
            manifest.contains("presentation")&&manifest.at("presentation").value("color_space",~0u)==0)c.stage=Stage::final_sdr;
    }
    if(c.interface_image())try{
        const auto& p=manifest.at("color_contract");const auto& rows=manifest.at("stages");
        c.reconstruction_available=manifest.at("same_call")==true&&manifest.at("complete")==true&&
            p.at("operator")==3&&p.at("exposure")==1&&p.at("matrices")=="identity"&&p.at("invalid_rgb_guard")==true&&
            rows.size()==4&&rows.at(0).at("stage")=="rr-output"&&rows.at(0).at("format")=="R16G16B16A16_FLOAT"&&
            rows.at(0).at("color_interpretation")=="admitted-RR-linear-working-RGB"&&
            rows.at(0).at("width")==row.at("width")&&rows.at(0).at("height")==row.at("height");
    }catch(const json::exception&){c.reconstruction_available=false;}
    return c;
}
inline viewer::Display effective(const Contract& source,viewer::Display requested){
    if(!source.known())throw std::runtime_error("Unknown source color contract; no automatic viewing transform");
    if(!std::isfinite(requested.exposure)||requested.exposure < -6||requested.exposure > 6||requested.mapping>3||requested.curve>2||requested.nr_linear>1||requested.nr_reconstruct>1||
        !std::isfinite(requested.contrast)||requested.contrast<.75f||requested.contrast>1.5f)throw std::runtime_error("Invalid display-only recipe");
    requested.stage=unsigned(source.stage);
    if(source.final()){
        if(requested.hdr)throw std::runtime_error("Final SDR code view requires an SDR window; HDR reinterpretation is not implemented");
        requested.exposure=0;requested.contrast=1;requested.mapping=1;requested.curve=0;requested.nr_linear=0;requested.nr_reconstruct=0;
    }else if(source.interface_image()){
        if(requested.nr_reconstruct){if(!source.reconstruction_available)throw std::runtime_error("Recorded RR reconstruction contract missing; display reconstruction refused");requested.nr_linear=0;}
        else requested.mapping=1; // Never ACES the compressed interface image itself.
    }else requested.nr_reconstruct=0;
    return requested;
}
inline json recipe(const Contract& source,const viewer::Display& requested){
    const auto d=effective(source,requested);
    const auto transform=source.final()?"identity-display-codes-v1":source.interface_image()&&!d.nr_reconstruct?(d.nr_linear?"diagnostic-linear-hypothesis-v1":"wrapper-srgb-code-view-v1"):
        d.mapping==3?"hill-aces-fitted-sdr-v1":d.mapping==0?"toe-linear-shoulder-maxrgb-v1":d.mapping==1?"linear-domain-display-v1":"legacy-curve-v1";
    return {{"contract",source.document()},{"transform",transform},{"exposure_ev",d.exposure},{"mapping",d.mapping},{"contrast",d.contrast},
        {"source_primaries_assumption",source.final()?"none-required-for-code-copy":"Rec.709/D65-viewing-assumption-unverified"},
        {"nr_linear_hypothesis",d.nr_linear!=0},{"output_encoding",source.final()?"G22/P709-preserved-codes":"sRGB"},
        {"view_reconstruction_applied",d.nr_reconstruct!=0},{"view_reconstruction",d.nr_reconstruct?"RR-assisted-operator3-identity-E1-FP16-v1":"none"},
        {"pure_nr_output_view",source.stage==Stage::api_output&&!d.nr_reconstruct},
        {"identity_rgb_codes",source.final()},{"source_alpha_preserved_in_raw",true},{"preview_alpha","opaque"},
        {"game_final_display_codes_preserved",source.final()},{"new_nr_evaluate",false},{"feedback_into_render_pipeline",false}};
}
inline bool comparable(const Contract& a,const Contract& b){return a.known()&&b.known()&&!a.final()&&!b.final()&&a.interface_image()==b.interface_image();}
}

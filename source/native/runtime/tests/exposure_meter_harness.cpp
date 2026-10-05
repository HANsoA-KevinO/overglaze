// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Reuse bounded allocation, padded row-copy and private-fence helpers only.
#include "color_test_gpu.hpp"
#include "lab_exposure_meter.hpp"
#include <cmath>
namespace {
constexpr auto readable=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
float luminance(float r,float g,float b){return .2126f*r+.7152f*g+.0722f*b;}
// Same weighting as the shader: mean of per-16x16-group means over valid pixels.
double expected_mean(const Bytes& rgba,unsigned w,unsigned h){
    double total=0;unsigned groups=0;
    for(unsigned gy=0;gy*16<h;++gy)for(unsigned gx=0;gx*16<w;++gx){double sum=0;unsigned n=0;
        for(unsigned y=gy*16;y<std::min(h,gy*16+16);++y)for(unsigned x=gx*16;x<std::min(w,gx*16+16);++x){
            const auto i=(std::size_t(y)*w+x)*4;const float r=unpack(rgba,i,true),g=unpack(rgba,i+1,true),b=unpack(rgba,i+2,true);
            if(!std::isfinite(r)||!std::isfinite(g)||!std::isfinite(b)||r<0||g<0||b<0)continue;
            sum+=std::log2(std::max(luminance(r,g,b),1e-5f));++n;}
        if(n){total+=sum/n;++groups;}}
    return groups?total/groups:0;
}
}
int main(){try{
    TestGpu g;lab::json report={{"origin","synthetic-input-no-NR"},{"checks",lab::json::array()}};
    auto texture=g.texture(DXGI_FORMAT_R16G16B16A16_FLOAT,false);
    lab::nr::ExposureMeter meter(g.device.Get(),width,height);
    require(meter.step()==1&&!meter.pending(),"Small extent samples every pixel and starts idle");
    rejects([&]{meter.acknowledge_completion();});
    // 1. Constant dark grey like the saved 007 call (0.0045): exact log2.
    Bytes constant(std::size_t(width)*height*8);
    for(unsigned p=0;p<width*height;++p){pack(constant,p*4,.0045f,true);pack(constant,p*4+1,.0045f,true);pack(constant,p*4+2,.0045f,true);pack(constant,p*4+3,1.f,true);}
    auto up=g.upload(texture.Get(),constant,D3D12_RESOURCE_STATE_COPY_DEST);g.execute();
    meter.record(g.commands.Get(),texture.Get());require(meter.pending(),"Recording marks the slot pending");
    rejects([&]{meter.record(g.commands.Get(),texture.Get());});
    g.execute();auto reading=meter.acknowledge_completion();
    const double expected_constant=expected_mean(constant,width,height);
    require(reading.valid&&reading.groups==6&&reading.samples==width*height&&std::fabs(reading.mean_log2_luminance-expected_constant)<2e-3,"Constant grey reading");
    report["checks"].push_back({{"case","constant-0.0045"},{"mean_log2",reading.mean_log2_luminance},{"expected",expected_constant},{"samples",reading.samples},{"groups",reading.groups}});
    // 2. Gradient with excluded pixels: NaN, negative, and a zero (clamped to 1e-5).
    Bytes gradient(constant.size());
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){const auto p=std::size_t(y)*width+x;
        float r=.001f+.02f*x,gch=.5f*r,b=2.f*r;
        if(p%97==5)r=std::numeric_limits<float>::quiet_NaN();
        if(p%89==7)gch=-.25f;
        if(p%53==11){r=gch=b=0;}
        pack(gradient,p*4,r,true);pack(gradient,p*4+1,gch,true);pack(gradient,p*4+2,b,true);pack(gradient,p*4+3,.5f,true);}
    auto up2=g.upload(texture.Get(),gradient,readable);g.execute();
    meter.record(g.commands.Get(),texture.Get());g.execute();reading=meter.acknowledge_completion();
    const double expected_gradient=expected_mean(gradient,width,height);
    unsigned excluded=0;for(unsigned p=0;p<width*height;++p)if((p%97==5||p%89==7)&&p%53!=11)++excluded; // the zero assignment above overrides invalid values
    require(reading.valid&&reading.samples+excluded==width*height&&std::fabs(reading.mean_log2_luminance-expected_gradient)<2e-3,"Gradient reading excludes nonfinite/negative pixels");
    report["checks"].push_back({{"case","gradient-with-invalid"},{"mean_log2",reading.mean_log2_luminance},{"expected",expected_gradient},{"samples",reading.samples},{"excluded",excluded}});
    // 3. Wrong layout refused before any recording.
    auto other=g.texture(DXGI_FORMAT_R32G32B32A32_FLOAT,true);
    bool accepted=true;try{meter.validate(g.commands.Get(),other.Get());}catch(...){accepted=false;}
    require(accepted,"RGBA32F of the fixed extent is accepted");
    D3D12_RESOURCE_DESC narrow_desc{};narrow_desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;narrow_desc.Width=width-1;narrow_desc.Height=height;narrow_desc.DepthOrArraySize=1;narrow_desc.MipLevels=1;narrow_desc.SampleDesc.Count=1;narrow_desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    auto mismatched=g.create(narrow_desc,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);
    rejects([&]{meter.validate(g.commands.Get(),mismatched.Get());});
    report["debug_errors"]=g.errors();require(report["debug_errors"].empty(),"D3D12 debug validation");
    report["passed"]=true;std::cout<<report.dump()<<'\n';return 0;
}catch(const TestGpuUnavailable& e){std::cout<<"SKIP "<<e.what()<<'\n';return kTestGpuSkip;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

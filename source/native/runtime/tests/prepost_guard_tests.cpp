// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "color_test_gpu.hpp"
#include "lab_prepost_ratio_codec.hpp"
int main(){try{
    TestGpu gpu;unsigned checks=0;
    for(bool half:{true,false})for(bool poison:{false,true}){
        const auto format=half?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;const unsigned scalar=half?2:4;
        auto original=gpu.texture(format,false),neural=gpu.texture(format,true);
        Bytes source(width*height*4*scalar),n(source.size());
        for(unsigned i=0;i<width*height;++i){
            for(unsigned ch=0;ch<3;++ch){pack(source,i*4+ch,.2f*(ch+1)+(i%7),half);pack(n,i*4+ch,.5f,half);}
            pack(source,i*4+3,static_cast<float>(i%5)/4,half);pack(n,i*4+3,0,half);
            if(poison)switch(i%6){case 0:pack(source,i*4,-.1f,half);break;
                case 1:pack(source,i*4,std::numeric_limits<float>::infinity(),half);break;
                case 2:pack(source,i*4,std::numeric_limits<float>::quiet_NaN(),half);break;
                case 3:pack(n,i*4,std::numeric_limits<float>::infinity(),half);break;
                case 4:pack(n,i*4,std::numeric_limits<float>::quiet_NaN(),half);break;default:break;}
        }
        auto upload=gpu.upload(original.Get(),source,D3D12_RESOURCE_STATE_COPY_DEST);
        auto nu=gpu.upload(neural.Get(),n,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(gpu.commands.Get(),neural.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);gpu.execute();
        Bytes baseline_pre,baseline_post;
        for(bool guard:{false,true}){
            if(poison&&!guard)continue;auto prepared=gpu.texture(format,true),composite=gpu.texture(format,true),hdr=gpu.texture(format,true),sdr=gpu.texture(format,true);
            lab::nr::PrePostColorInputs inputs{original.Get(),prepared.Get(),neural.Get(),composite.Get(),hdr.Get(),sdr.Get(),true,!poison,guard};
            lab::nr::PrePostColorCodec codec(gpu.device.Get(),inputs);
            lab::nr::PrePostColorConstants c{};c.width=width;c.height=height;c.pre_post=1;c.tonemap_operator=3;c.exposure=1;c.sigma=10;c.gamma=.454f;
            c.pre_matrix={{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};c.post_matrix=c.pre_matrix;
            codec.prepare(gpu.commands.Get(),c);codec.composite(gpu.commands.Get());codec.bind_completion(gpu.fence.Get(),gpu.serial+1);gpu.execute();
            require(codec.retire_if_complete(),"Guard completion");
            auto pre=gpu.read(prepared.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),post=gpu.read(composite.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if(!guard){baseline_pre=pre;baseline_post=post;}
            else if(!poison){require(pre==baseline_pre&&post==baseline_post,"Guard changes supported finite-domain math");++checks;}
            else for(unsigned i=0;i<width*height;++i){
                for(unsigned ch=0;ch<3;++ch){const auto v=unpack(pre,i*4+ch,half);require(std::isfinite(v)&&v>=0&&v<=1,"Invalid RGB entered model input");++checks;
                    if(i%6<3){require(v==0,"Invalid source must yield neutral NR input");++checks;}}
                if(i%6<5){for(unsigned ch=0;ch<4;++ch){const auto a=unpack(post,i*4+ch,half),e=unpack(source,i*4+ch,half);
                    require((std::isnan(a)&&std::isnan(e))||a==e,"Invalid source/model result must pass through original pixel");++checks;}}
                else require(std::isfinite(unpack(post,i*4,half)),"Valid neighbor corrupted");
            }
        }
    }
    require(gpu.errors().empty(),"D3D12 guard validation error");
    std::cout<<lab::json{{"passed",true},{"checks",checks},{"raw_files",0},{"guard_is_lab_extension",true},{"official_invalid_domain_behavior_claimed",false},{"nr_executed",false}}.dump()<<'\n';return 0;
}catch(const TestGpuUnavailable& e){std::cout<<"SKIP "<<e.what()<<'\n';return kTestGpuSkip;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

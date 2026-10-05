// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <string_view>
#include <vector>
namespace lab {
struct DisplayWhite {float nits=80;bool observed=false;};
// Read-only: never changes Windows HDR, SDR brightness or monitor settings.
inline DisplayWhite display_sdr_white(std::wstring_view device){
    for(unsigned attempt=0;attempt<3;++attempt){UINT count=0,modes_count=0;
        if(GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&count,&modes_count)!=ERROR_SUCCESS||count>128||modes_count>512)return {};
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(count);std::vector<DISPLAYCONFIG_MODE_INFO> modes(modes_count);
        const auto code=QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&count,paths.data(),&modes_count,modes.data(),nullptr);
        if(code==ERROR_INSUFFICIENT_BUFFER)continue;if(code!=ERROR_SUCCESS)return {};
        for(unsigned i=0;i<count;++i){const auto& path=paths[i];DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
            name.header={DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,sizeof(name),path.sourceInfo.adapterId,path.sourceInfo.id};
            if(DisplayConfigGetDeviceInfo(&name.header)!=ERROR_SUCCESS||device!=name.viewGdiDeviceName)continue;
            DISPLAYCONFIG_SDR_WHITE_LEVEL white{};white.header={DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL,sizeof(white),path.targetInfo.adapterId,path.targetInfo.id};
            if(DisplayConfigGetDeviceInfo(&white.header)==ERROR_SUCCESS&&white.SDRWhiteLevel>=1000&&white.SDRWhiteLevel<=12500)return {white.SDRWhiteLevel*.08f,true};
        }return {};
    }return {};
}
}

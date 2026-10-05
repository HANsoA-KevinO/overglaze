// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include <cstdint>
namespace lab::slobserve {
// The record the public Streamline API hooks fill, and the sink they hand it to.
// Provider-owned: filling a research CommandEvent directly would pull the whole
// research evidence recorder into every translation unit that touches the SL
// boundary.
//
// Everything here is borrowed metadata: no COM query or reference, no FrameToken
// virtual call, no private layout read, no file I/O. A Sink implementation runs
// INSIDE the game's own Streamline call, so it must be bounded, nonblocking,
// noexcept and thread-safe. The controller installs no sink at all, exactly as it
// installs no IResearchObserver and no WorkbenchResearchExtension.
struct Streamline {
    std::uint64_t id=0,parent=0,token=0,index_argument=0,inputs=0,output_argument=0;
    unsigned api=0,feature=0,count=0,index=0,viewport=0,read_status=0,viewport_matches=0;
    bool index_read=false,viewport_read=false,token_read=false;
};
struct Event {
    enum class Phase {begin,end} phase=Phase::begin;
    std::uint64_t qpc=0,api_begin_qpc=0,api_end_qpc=0,command=0;
    DWORD thread=0;
    unsigned result=0;
    Streamline sl{};
};
class Sink {
public:
    virtual ~Sink()=default;
    // Asked once per hooked call before anything is filled in; false costs nothing.
    virtual bool recording() const noexcept=0;
    virtual void record(const Event&) noexcept=0;
};
}

// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_live_submission.hpp"
namespace lab {
// Defined only in the fixture object, never compiled into the shipping router.
struct LiveSubmissionFixture {
    using Pause=void(*)(void*,unsigned);
    static void with_metadata(LiveSubmission&,void(*)(void*),void*);
    static void pause_before_lock(LiveSubmission&,Pause,void*);
};
}

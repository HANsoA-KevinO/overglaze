// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// The observation surface reports what it recorded. No NGX, no GPU, no game:
// this checks the REPORT CONTRACT, which is where the first real observation
// run lost everything it had collected.
#include "lab_ngx_observer.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
int checks = 0;
void need(bool ok, const std::string& why) {
    ++checks;
    if (!ok) throw std::runtime_error(why);
}
}

int main() try {
    lab::NgxObserver observer;

    // Nothing is attached in a test process, and attach() must say so by
    // waiting rather than by refusing: the loader simply is not mapped here.
    need(!observer.attached(), "A fresh observer must not claim an attachment");
    const bool took = observer.attach();
    need(!took, "attach() must not succeed without a mapped NGX core");
    need(!observer.attached(), "A failed attach must not set the attached flag");

    const auto report = observer.report();
    need(report.is_object(), "The report must be an object");

    // Every key the design step reads. `features` is listed first because it
    // was once built and then left out of the returned object: a run reported
    // over a thousand evaluates and not one of the records behind them.
    for (const auto* key : {"features", "state", "refusal", "registry_path", "mapped_path",
                            "loader", "creates", "evaluates", "releases", "feature_overflow",
                            "guide_vocabulary", "contract", "hooks", "admission"})
        need(report.contains(key), std::string("Report is missing '") + key + "'");

    need(report.at("features").is_array(), "features must be an array even when empty");
    need(report.at("features").empty(), "A surface that never attached has no features");
    need(report.at("creates") == 0 && report.at("evaluates") == 0 && report.at("releases") == 0,
         "An unattached surface must report no observed calls");
    need(report.at("state") == "waiting" || report.at("state") == "refused",
         "An unattached surface is waiting or refused, never attached");

    // Without a receiver nothing can be admitted, and the report must say so.
    need(report.at("admission").at("receiver") == false && report.at("admission").at("offered") == 0,
         "An observer with no receiver must report no admission");
    // The admission record, also published on its own (host key ngx_admission)
    // because the full report is the first key status clipping drops. The
    // ignored_* counters are the non-candidates -- frame generation above all --
    // that are forwarded and never offered (as Hellblade 2 requires).
    const auto admission = observer.admission();
    for (const auto* key : {"receiver", "offered", "admitted", "skipped", "aborted", "last_skip",
                            "ignored_other_features", "ignored_by_feature", "ignored_unobserved_without_output"})
        need(admission.contains(key), std::string("Admission record is missing '") + key + "'");
    need(admission == report.at("admission"), "admission() and report().admission are the same record");
    need(admission.at("ignored_other_features") == 0 && admission.at("ignored_by_feature").is_object() &&
         admission.at("ignored_by_feature").empty() && admission.at("ignored_unobserved_without_output") == 0,
         "Nothing observed, nothing ignored");
    need(observer.blocked() == nullptr, "Nothing was offered, so nothing is blocked");

    // The vocabulary is what a guide bitmask is read against; a silent change
    // would relabel every recorded key in every earlier report.
    need(report.at("guide_vocabulary") == 54, "Guide vocabulary changed; earlier reports are keyed to it");

    // The contract is shown to the user and written into every report. It is part of
    // the surface, not a comment: it states what this thing is NOT allowed to do.
    const auto contract = report.at("contract").get<std::string>();
    for (const auto* phrase : {"observation only", "no frame admitted", "no GPU work"})
        need(contract.find(phrase) != std::string::npos,
             std::string("The stated contract no longer says '") + phrase + "'");

    std::cout << "PASS " << checks << " NGX observation report checks; no NGX loaded, no GPU work\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL after " << checks << " checks: " << e.what() << "\n";
    return 1;
}

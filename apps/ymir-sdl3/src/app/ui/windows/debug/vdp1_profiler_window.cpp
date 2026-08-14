#include "vdp1_profiler_window.hpp"

#if Ymir_ENABLE_VDP1_PROFILING

    #include <fmt/format.h>

    #include <chrono>
    #include <filesystem>
    #include <string>

using namespace ymir::vdp::prof;

namespace app::ui {

VDP1ProfilerWindow::VDP1ProfilerWindow(SharedContext &context)
    : VDPWindowBase(context) {

    m_windowConfig.name = "VDP1 cost model profiler";
}

void VDP1ProfilerWindow::DrawContents() {
    const FrameStats f = m_showAveraged ? g_vdp1.Average(static_cast<size_t>(m_averageWindow)) : g_vdp1.Last();

    // The raster-side counters are written by the render thread when threaded VDP1 rendering is on, so
    // they will not line up with the command-side counters. Warn loudly -- a capture taken in this state
    // is not usable.
    if (m_context.saturn.GetConfiguration().video.threadedVDP1.Get()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.4f, 0.3f, 1.0f});
        ImGui::TextWrapped("Threaded VDP1 rendering is ENABLED. Raster counters will not match the command "
                           "counters. Turn it off in Settings > Video before capturing.");
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    ImGui::Checkbox("Average over last", &m_showAveraged);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderInt("frames", &m_averageWindow, 1, 600);

    ImGui::Separator();
    DrawSummary(f);
    ImGui::Separator();
    DrawBreakdown(f);
    ImGui::Separator();
    DrawCapture();
}

void VDP1ProfilerWindow::DrawSummary(const FrameStats &f) {
    const uint64 estPixels = f.EstPixelsTotal();

    // The headline number: how many pixels the cost model charged for every pixel the hardware would
    // actually have written. 1.0 means the model is calibrated; higher means it overshoots.
    const double ratio = f.OvershootRatio();
    ImGui::Text("Overshoot ratio (estimated / plotted):");
    ImGui::SameLine();
    if (ratio >= 2.0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.4f, 0.3f, 1.0f});
    } else if (ratio >= 1.25) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.8f, 0.3f, 1.0f});
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.4f, 1.0f, 0.4f, 1.0f});
    }
    ImGui::Text("%.2fx", ratio);
    ImGui::PopStyleColor();

    if (ImGui::BeginTable("summary", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        auto row = [](const char *label, const std::string &value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value.c_str());
        };

        row("Commands walked", fmt::format("{}  ({} skipped, {} invalid)", f.cmdTotal, f.cmdSkipped, f.cmdInvalid));
        row("Estimated cycles", fmt::format("{}", f.estCyclesTotal));
        row("Cycle budget granted", fmt::format("{}", f.cyclesGranted));
        row("Fetch cost share",
            fmt::format("{:.1f}%  (16 cycles x {} commands)", f.FetchCostFraction() * 100.0, f.cmdTotal));
        row("List completed", f.completed ? "yes" : "NO - cut short");
        row("Scanlines", fmt::format("VCNT {} -> {}", f.beginVCNT, f.endVCNT));

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Where the estimated pixels went:");

    if (ImGui::BeginTable("pixels", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Outcome");
        ImGui::TableSetupColumn("Pixels");
        ImGui::TableSetupColumn("Share of estimate");
        ImGui::TableHeadersRow();

        auto row = [&](const char *label, uint64 value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(value));
            ImGui::TableNextColumn();
            if (estPixels > 0) {
                ImGui::Text("%.1f%%", 100.0 * static_cast<double>(value) / static_cast<double>(estPixels));
            } else {
                ImGui::TextUnformatted("-");
            }
        };

        row("Estimated (charged)", estPixels);
        row("Actually plotted", f.pixelsPlotted);
        row("  of which blended (fb read)", f.fbBlends);
        row("Discarded: clipped", f.pixelsClipped);
        row("Discarded: transparent / end code", f.pixelsTransparent);
        row("Discarded: mesh / interlace", f.pixelsMeshed);
        row("Texel fetches", f.texelFetches);

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Text("Quad scanlines: estimator stepped %llu, rasterizer stepped %llu",
                static_cast<unsigned long long>(f.estQuadLines), static_cast<unsigned long long>(f.rasterQuadLines));
    if (f.estQuadLines > f.rasterQuadLines) {
        ImGui::TextDisabled("  -> %llu scanlines charged but never reached (quad clip early-out)",
                            static_cast<unsigned long long>(f.estQuadLines - f.rasterQuadLines));
    }
}

void VDP1ProfilerWindow::DrawBreakdown(const FrameStats &f) {
    ImGui::TextUnformatted("Estimated cost by command type:");

    if (ImGui::BeginTable("buckets", 4,
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Command");
        ImGui::TableSetupColumn("Count");
        ImGui::TableSetupColumn("Est. pixels");
        ImGui::TableSetupColumn("Avg per command");
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < kNumCmdBuckets; ++i) {
            if (f.cmdCount[i] == 0 && f.estPixels[i] == 0) {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const auto name = kCmdBucketNames[i];
            ImGui::TextUnformatted(name.data(), name.data() + name.size());
            ImGui::TableNextColumn();
            ImGui::Text("%u", f.cmdCount[i]);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(f.estPixels[i]));
            ImGui::TableNextColumn();
            if (f.cmdCount[i] > 0) {
                ImGui::Text("%.1f", static_cast<double>(f.estPixels[i]) / static_cast<double>(f.cmdCount[i]));
            } else {
                ImGui::TextUnformatted("-");
            }
        }

        ImGui::EndTable();
    }
}

void VDP1ProfilerWindow::DrawCapture() {
    const bool open = g_vdp1.IsCSVOpen();

    if (open) {
        if (ImGui::Button("Stop CSV capture")) {
            g_vdp1.CloseCSV();
            m_csvStatus = "Capture stopped.";
        }
        ImGui::SameLine();
        ImGui::Text("Recording to %s", g_vdp1.CSVPath().filename().string().c_str());
    } else {
        if (ImGui::Button("Start CSV capture")) {
            // One file per capture, named after the loaded game so sessions don't overwrite each other.
            const auto now = std::chrono::system_clock::now();
            const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

            std::string gameName = m_context.GetGameFileName().stem().string();
            if (gameName.empty()) {
                gameName = "nogame";
            }

            const auto dir = m_context.profile.GetPath(ProfilePath::Dumps);
            std::filesystem::create_directories(dir);
            const auto path = dir / fmt::format("vdp1_profile_{}_{}.csv", gameName, stamp);

            if (g_vdp1.OpenCSV(path)) {
                m_csvStatus = fmt::format("Recording to {}", path.string());
            } else {
                m_csvStatus = fmt::format("FAILED to open {}", path.string());
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Reset history")) {
        g_vdp1.ResetHistory();
    }

    if (!m_csvStatus.empty()) {
        ImGui::TextWrapped("%s", m_csvStatus.c_str());
    }

    ImGui::TextDisabled("%llu VDP1 frames captured", static_cast<unsigned long long>(g_vdp1.FrameCount()));
}

} // namespace app::ui

#endif // Ymir_ENABLE_VDP1_PROFILING

#pragma once

#include "vdp_window_base.hpp"

#include <ymir/hw/vdp/vdp1_profiler.hpp>

#if Ymir_ENABLE_VDP1_PROFILING

    #include <string>

namespace app::ui {

/// @brief Live view of the VDP1 cost model instrumentation.
///
/// Only compiled when the `Ymir_ENABLE_VDP1_PROFILING` CMake option is enabled.
/// See `libs/ymir-core/include/ymir/hw/vdp/vdp1_profiler.hpp` for what the numbers mean.
class VDP1ProfilerWindow : public VDPWindowBase {
public:
    VDP1ProfilerWindow(SharedContext &context);

protected:
    void DrawContents() override;

private:
    void DrawSummary(const ymir::vdp::prof::FrameStats &f);
    void DrawBreakdown(const ymir::vdp::prof::FrameStats &f);
    void DrawCapture();

    int m_averageWindow = 60;
    bool m_showAveraged = true;
    std::string m_csvStatus;
};

} // namespace app::ui

#endif // Ymir_ENABLE_VDP1_PROFILING

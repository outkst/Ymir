#pragma once

/// @file
/// @brief Opt-in instrumentation for measuring the accuracy of the VDP1 command cost model.
///
/// This is a *measurement scaffold*, not production code. It is compiled out entirely unless the
/// `Ymir_ENABLE_VDP1_PROFILING` CMake option is enabled, in which case every hook below becomes a
/// plain counter increment.
///
/// ## What it is for
///
/// `VDP::VDP1CalcCommandTiming` estimates the cost of each VDP1 command from its geometry alone. The
/// renderer then draws the command for real. The estimate is known to overshoot the actual hardware
/// cost, which makes games drop frames that should run at full speed.
///
/// This profiler records both sides of that comparison per VDP1 frame:
///
/// - The **estimate**: how many pixels `VDP1CalcCommandTiming` charged for, broken down by command type.
/// - The **reality**: how many pixels the rasterizer actually wrote to the framebuffer, plus a
///   breakdown of *why* the rest were not written (clipped, transparent, end code, mesh).
///
/// The ratio between them, and the composition of the discarded pixels, tells you which correction to
/// make to the cost model and how large it needs to be.
///
/// ## Threading caveat
///
/// The command-side counters are written by the emulation thread. The raster-side counters are written
/// by whichever thread runs the rasterizer, which is a *separate* thread when threaded VDP1 rendering
/// is enabled. In that configuration the raster counters for a frame may not have been fully written
/// when the emulation thread snapshots them at end-of-frame, so the two halves will not line up.
///
/// **Disable threaded VDP1 rendering when measuring.** `FrameStats::threadedVDP1` records the setting
/// so bad captures can be identified after the fact.

#include <ymir/core/types.hpp>

#ifndef Ymir_ENABLE_VDP1_PROFILING
    #define Ymir_ENABLE_VDP1_PROFILING 0
#endif

#if Ymir_ENABLE_VDP1_PROFILING

    #include <algorithm>
    #include <array>
    #include <cstddef>
    #include <filesystem>
    #include <fstream>
    #include <mutex>
    #include <string_view>

namespace ymir::vdp::prof {

/// @brief Per-command-type buckets. Deliberately coarser than `VDP1Command::CommandType`: commands that
/// share a cost path (the two distorted sprite encodings, the two polyline encodings) share a bucket.
enum class CmdBucket : size_t {
    NormalSprite,
    ScaledSprite,
    DistortedSprite,
    Polygon,
    Polylines,
    Line,
    NonDrawing, ///< clipping / local coordinate commands: fetch cost only
    _Count,
};

inline constexpr size_t kNumCmdBuckets = static_cast<size_t>(CmdBucket::_Count);

inline constexpr std::array<std::string_view, kNumCmdBuckets> kCmdBucketNames{
    "NormalSprite", "ScaledSprite", "DistortedSprite", "Polygon", "Polylines", "Line", "NonDrawing",
};

/// @brief Everything recorded for a single VDP1 drawing frame (one `VDP1BeginFrame` .. `VDP1EndFrame`).
struct FrameStats {
    uint64 frameIndex = 0;

    // -------------------------------------------------------------------------
    // Command side (emulation thread)

    /// @brief Number of drawing commands executed, per bucket.
    std::array<uint32, kNumCmdBuckets> cmdCount{};

    /// @brief Pixels charged by `VDP1CalcCommandTiming`, per bucket. This is the *estimate*.
    std::array<uint64, kNumCmdBuckets> estPixels{};

    uint32 cmdTotal = 0;   ///< All commands walked, including skipped/invalid/non-drawing.
    uint32 cmdSkipped = 0; ///< Commands with the skip bit set.
    uint32 cmdInvalid = 0; ///< Commands with an unrecognized command type.

    /// @brief Quad scanlines the estimator stepped over. Compare against `rasterQuadLines`.
    uint64 estQuadLines = 0;

    /// @brief Total cycles the estimator charged, i.e. what actually gets spent against the budget.
    /// Equals (16 * cmdTotal) + sum(estPixels).
    uint64 estCyclesTotal = 0;

    // -------------------------------------------------------------------------
    // Raster side (render thread when threaded VDP1 rendering is on)

    uint64 pixelsPlotted = 0;     ///< Pixels actually written to the framebuffer.
    uint64 pixelsClipped = 0;     ///< Rejected by the system/user clipping test.
    uint64 pixelsTransparent = 0; ///< Skipped: transparent texel or end code.
    uint64 pixelsMeshed = 0;      ///< Skipped by the mesh checkerboard, or by double-interlace line select.
    uint64 texelFetches = 0;      ///< Texel reads from VDP1 VRAM.
    uint64 fbBlends = 0;          ///< Plotted pixels needing a framebuffer read-modify-write (colorCalcBits != 0).
    uint64 rasterQuadLines = 0;   ///< Quad scanlines the rasterizer stepped over.

    // -------------------------------------------------------------------------
    // Budget / timing side

    uint64 cyclesGranted = 0; ///< Cycle budget handed to `VDP::Advance` while this frame was drawing.
    uint64 spillover = 0;     ///< Unspent overshoot carried at end of frame.
    uint64 penalty = 0;       ///< Accumulated VRAM-write stall penalty at end of frame.

    bool completed = false; ///< True if the command list reached an End command.
    uint32 beginVCNT = 0;   ///< Scanline the frame started on.
    uint32 endVCNT = 0;     ///< Scanline the frame ended on.

    // -------------------------------------------------------------------------
    // Configuration in effect (so a capture can be interpreted later)

    uint32 cyclesShift = 0;      ///< `VDP::m_VDP1CyclesShift`. 2 = the default 4x budget multiplier.
    bool threadedVDP1 = false;   ///< If true, raster counters are unreliable. See file header.

    // -------------------------------------------------------------------------
    // Derived helpers

    /// @brief Pixels the estimator charged for but the rasterizer never wrote.
    [[nodiscard]] uint64 EstPixelsTotal() const {
        uint64 total = 0;
        for (const uint64 n : estPixels) {
            total += n;
        }
        return total;
    }

    /// @brief Ratio of estimated pixels to actually plotted pixels. > 1.0 means the model overshoots.
    /// Returns 0.0 when nothing was plotted.
    [[nodiscard]] double OvershootRatio() const {
        if (pixelsPlotted == 0) {
            return 0.0;
        }
        return static_cast<double>(EstPixelsTotal()) / static_cast<double>(pixelsPlotted);
    }

    /// @brief Fraction of the estimated cost that is fixed per-command fetch overhead rather than fill.
    /// Tells you whether a frame is setup-bound or fill-bound.
    [[nodiscard]] double FetchCostFraction() const {
        if (estCyclesTotal == 0) {
            return 0.0;
        }
        return static_cast<double>(cmdTotal * 16ull) / static_cast<double>(estCyclesTotal);
    }

    void Reset() {
        *this = FrameStats{};
    }
};

/// @brief Collects `FrameStats` and optionally streams them to a CSV file.
///
/// A single process-wide instance (`prof::g_vdp1`) is used deliberately: this keeps the instrumentation
/// diff small and easy to delete, at the cost of not supporting multiple concurrent VDP instances. That
/// is an acceptable trade for a measurement build.
class Profiler {
public:
    // -------------------------------------------------------------------------
    // Command-side hooks (emulation thread)

    void BeginFrame(uint32 vcnt, uint32 cyclesShift, bool threadedVDP1) {
        m_cur.Reset();
        m_cur.frameIndex = m_nextFrameIndex++;
        m_cur.beginVCNT = vcnt;
        m_cur.cyclesShift = cyclesShift;
        m_cur.threadedVDP1 = threadedVDP1;
        ResetRasterCounters();
        m_active = true;
    }

    void CountCommand() {
        ++m_cur.cmdTotal;
    }

    void CountSkippedCommand() {
        ++m_cur.cmdSkipped;
    }

    void CountInvalidCommand() {
        ++m_cur.cmdInvalid;
    }

    /// @brief Records the estimated pixel cost of one drawing command.
    void CountDrawCommand(CmdBucket bucket, uint64 estPixels) {
        const size_t idx = static_cast<size_t>(bucket);
        ++m_cur.cmdCount[idx];
        m_cur.estPixels[idx] += estPixels;
    }

    void CountEstQuadLine() {
        ++m_cur.estQuadLines;
    }

    void CountCycles(uint64 cycles) {
        m_cur.estCyclesTotal += cycles;
    }

    void CountGrantedCycles(uint64 cycles) {
        if (m_active) {
            m_cur.cyclesGranted += cycles;
        }
    }

    void EndFrame(uint32 vcnt, bool completed, uint64 spillover, uint64 penalty) {
        if (!m_active) {
            return;
        }
        m_active = false;
        m_cur.endVCNT = vcnt;
        m_cur.completed = completed;
        m_cur.spillover = spillover;
        m_cur.penalty = penalty;

        // Snapshot the raster-side counters. See the threading caveat in the file header.
        m_cur.pixelsPlotted = m_pixelsPlotted;
        m_cur.pixelsClipped = m_pixelsClipped;
        m_cur.pixelsTransparent = m_pixelsTransparent;
        m_cur.pixelsMeshed = m_pixelsMeshed;
        m_cur.texelFetches = m_texelFetches;
        m_cur.fbBlends = m_fbBlends;
        m_cur.rasterQuadLines = m_rasterQuadLines;

        {
            std::lock_guard lock{m_mutex};
            m_last = m_cur;
            m_history[m_historyPos] = m_cur;
            m_historyPos = (m_historyPos + 1) % kHistorySize;
            if (m_historyCount < kHistorySize) {
                ++m_historyCount;
            }
        }
        WriteCSVRow(m_cur);
    }

    // -------------------------------------------------------------------------
    // Raster-side hooks (render thread when threaded)
    //
    // Plain non-atomic increments: making these atomic would add a locked instruction to the per-pixel
    // path and distort the very framerate behaviour being measured. There is exactly one rasterizer
    // thread, so these never race with each other -- only with the end-of-frame snapshot above.

    void CountPixelPlotted(bool blended) {
        ++m_pixelsPlotted;
        m_fbBlends += blended ? 1 : 0;
    }

    void CountPixelClipped() {
        ++m_pixelsClipped;
    }

    void CountPixelTransparent() {
        ++m_pixelsTransparent;
    }

    void CountPixelMeshed() {
        ++m_pixelsMeshed;
    }

    void CountTexelFetch() {
        ++m_texelFetches;
    }

    void CountRasterQuadLine() {
        ++m_rasterQuadLines;
    }

    // -------------------------------------------------------------------------
    // Readback

    [[nodiscard]] FrameStats Last() const {
        std::lock_guard lock{m_mutex};
        return m_last;
    }

    /// @brief Averages the last `count` completed frames. Per-frame numbers are noisy; aggregate before
    /// drawing conclusions.
    [[nodiscard]] FrameStats Average(size_t count) const {
        std::lock_guard lock{m_mutex};
        count = std::min(count, m_historyCount);
        FrameStats avg{};
        if (count == 0) {
            return avg;
        }
        for (size_t i = 0; i < count; ++i) {
            // Walk backwards from the most recent entry.
            const size_t idx = (m_historyPos + kHistorySize - 1 - i) % kHistorySize;
            const FrameStats &f = m_history[idx];
            for (size_t b = 0; b < kNumCmdBuckets; ++b) {
                avg.cmdCount[b] += f.cmdCount[b];
                avg.estPixels[b] += f.estPixels[b];
            }
            avg.cmdTotal += f.cmdTotal;
            avg.cmdSkipped += f.cmdSkipped;
            avg.cmdInvalid += f.cmdInvalid;
            avg.estQuadLines += f.estQuadLines;
            avg.estCyclesTotal += f.estCyclesTotal;
            avg.pixelsPlotted += f.pixelsPlotted;
            avg.pixelsClipped += f.pixelsClipped;
            avg.pixelsTransparent += f.pixelsTransparent;
            avg.pixelsMeshed += f.pixelsMeshed;
            avg.texelFetches += f.texelFetches;
            avg.fbBlends += f.fbBlends;
            avg.rasterQuadLines += f.rasterQuadLines;
            avg.cyclesGranted += f.cyclesGranted;
            avg.completed = avg.completed || f.completed;
        }
        for (size_t b = 0; b < kNumCmdBuckets; ++b) {
            avg.cmdCount[b] = static_cast<uint32>(avg.cmdCount[b] / count);
            avg.estPixels[b] /= count;
        }
        avg.cmdTotal = static_cast<uint32>(avg.cmdTotal / count);
        avg.cmdSkipped = static_cast<uint32>(avg.cmdSkipped / count);
        avg.cmdInvalid = static_cast<uint32>(avg.cmdInvalid / count);
        avg.estQuadLines /= count;
        avg.estCyclesTotal /= count;
        avg.pixelsPlotted /= count;
        avg.pixelsClipped /= count;
        avg.pixelsTransparent /= count;
        avg.pixelsMeshed /= count;
        avg.texelFetches /= count;
        avg.fbBlends /= count;
        avg.rasterQuadLines /= count;
        avg.cyclesGranted /= count;
        avg.frameIndex = m_last.frameIndex;
        avg.cyclesShift = m_last.cyclesShift;
        avg.threadedVDP1 = m_last.threadedVDP1;
        return avg;
    }

    /// @brief Number of frames captured since the last `ResetHistory`.
    [[nodiscard]] uint64 FrameCount() const {
        std::lock_guard lock{m_mutex};
        return m_nextFrameIndex;
    }

    void ResetHistory() {
        std::lock_guard lock{m_mutex};
        m_historyPos = 0;
        m_historyCount = 0;
        m_nextFrameIndex = 0;
        m_last = FrameStats{};
    }

    // -------------------------------------------------------------------------
    // CSV output

    /// @brief Begins streaming one CSV row per VDP1 frame to `path`. Overwrites any existing file.
    /// @return true if the file was opened.
    bool OpenCSV(const std::filesystem::path &path) {
        std::lock_guard lock{m_mutex};
        m_csv.close();
        m_csv.clear();
        m_csv.open(path, std::ios::out | std::ios::trunc);
        if (!m_csv) {
            return false;
        }
        m_csvPath = path;
        m_csv << "frame,begin_vcnt,end_vcnt,completed,cycles_shift,threaded_vdp1"
                 ",cmd_total,cmd_skipped,cmd_invalid"
                 ",est_cycles_total,est_pixels_total,est_quad_lines"
                 ",pixels_plotted,pixels_clipped,pixels_transparent,pixels_meshed"
                 ",texel_fetches,fb_blends,raster_quad_lines"
                 ",cycles_granted,spillover,penalty";
        for (const auto name : kCmdBucketNames) {
            m_csv << ",n_" << name;
        }
        for (const auto name : kCmdBucketNames) {
            m_csv << ",px_" << name;
        }
        m_csv << '\n';
        m_csv.flush();
        return true;
    }

    void CloseCSV() {
        std::lock_guard lock{m_mutex};
        m_csv.close();
        m_csv.clear();
        m_csvPath.clear();
    }

    [[nodiscard]] bool IsCSVOpen() const {
        std::lock_guard lock{m_mutex};
        return m_csv.is_open();
    }

    [[nodiscard]] std::filesystem::path CSVPath() const {
        std::lock_guard lock{m_mutex};
        return m_csvPath;
    }

private:
    void ResetRasterCounters() {
        m_pixelsPlotted = 0;
        m_pixelsClipped = 0;
        m_pixelsTransparent = 0;
        m_pixelsMeshed = 0;
        m_texelFetches = 0;
        m_fbBlends = 0;
        m_rasterQuadLines = 0;
    }

    void WriteCSVRow(const FrameStats &f) {
        std::lock_guard lock{m_mutex};
        if (!m_csv.is_open()) {
            return;
        }
        m_csv << f.frameIndex << ',' << f.beginVCNT << ',' << f.endVCNT << ',' << (f.completed ? 1 : 0) << ','
              << f.cyclesShift << ',' << (f.threadedVDP1 ? 1 : 0) << ',' << f.cmdTotal << ',' << f.cmdSkipped << ','
              << f.cmdInvalid << ',' << f.estCyclesTotal << ',' << f.EstPixelsTotal() << ',' << f.estQuadLines << ','
              << f.pixelsPlotted << ',' << f.pixelsClipped << ',' << f.pixelsTransparent << ',' << f.pixelsMeshed << ','
              << f.texelFetches << ',' << f.fbBlends << ',' << f.rasterQuadLines << ',' << f.cyclesGranted << ','
              << f.spillover << ',' << f.penalty;
        for (const uint32 n : f.cmdCount) {
            m_csv << ',' << n;
        }
        for (const uint64 n : f.estPixels) {
            m_csv << ',' << n;
        }
        m_csv << '\n';
    }

    static constexpr size_t kHistorySize = 600; // ~10 seconds at 60 fps

    FrameStats m_cur{};
    bool m_active = false;
    uint64 m_nextFrameIndex = 0;

    // Raster-side counters, reset per frame.
    uint64 m_pixelsPlotted = 0;
    uint64 m_pixelsClipped = 0;
    uint64 m_pixelsTransparent = 0;
    uint64 m_pixelsMeshed = 0;
    uint64 m_texelFetches = 0;
    uint64 m_fbBlends = 0;
    uint64 m_rasterQuadLines = 0;

    mutable std::mutex m_mutex;
    FrameStats m_last{};
    std::array<FrameStats, kHistorySize> m_history{};
    size_t m_historyPos = 0;
    size_t m_historyCount = 0;

    std::ofstream m_csv;
    std::filesystem::path m_csvPath;
};

/// @brief The process-wide profiler instance.
inline Profiler g_vdp1;

} // namespace ymir::vdp::prof

    // -------------------------------------------------------------------------
    // Hook macros. These expand to nothing when profiling is disabled, so call sites stay readable and
    // cost exactly zero in normal builds.

    #define YMIR_VDP1PROF(expr) (::ymir::vdp::prof::g_vdp1.expr)

#else // !Ymir_ENABLE_VDP1_PROFILING

    #define YMIR_VDP1PROF(expr) ((void)0)

#endif

#pragma once
#ifndef DISABLE_VISUALIZATION

#include <imgui.h>

namespace Charting {

// Tabbed panes inside one fullscreen host window.
//
// Eleven panels tiled onto one screen left every one of them too small to read.
// Grouping them into tabs means each pane gets real estate, and it scales: a new
// panel joins a tab instead of stealing width from the other ten. The vendored
// ImGui (1.91.9) has no docking, but BeginTabBar is core, so tabs need nothing
// extra.
//
// Panes are child regions rather than windows, so they cannot be dragged,
// collapsed or stacked, and geometry is recomputed each frame -- imgui.ini
// persistence is off because a saved copy could only ever be stale.
enum class DashTab { Overview, Positions, Strategies, Signals, Health, Data };

enum class PanelSlot {
    Controls, Statistics, EquityCurve, PeriodReturns,      // Overview
    Inventories, Orders, ActiveOrderbook, Trades,          // Positions
    StrategyList, StrategyDetail,                          // Strategies
    Signals,                                               // Signals
    HealthSummary, Streams, StateMachines,                 // Health
    Datasets,                                              // Data
};

namespace Palette {
inline constexpr ImVec4 ok{0.35f, 0.85f, 0.35f, 1.0f};
inline constexpr ImVec4 warn{0.95f, 0.75f, 0.25f, 1.0f};
inline constexpr ImVec4 bad{0.95f, 0.35f, 0.35f, 1.0f};
inline constexpr ImVec4 idle{0.45f, 0.60f, 0.85f, 1.0f};
inline constexpr ImVec4 muted{0.55f, 0.55f, 0.55f, 1.0f};
}  // namespace Palette

// Drawn rather than typed: the default ImGui font is ASCII-only.
void status_dot(const ImVec4& color);

// Which tab a panel belongs to; render_frame uses this to skip panels whose tab
// is not open, so hidden panels cost nothing.
DashTab tab_of(PanelSlot slot);

// Call once immediately after BeginTabItem: records the tab's content origin
// and size, which every pane in that tab is positioned against. Panes are
// placed absolutely, so without this they would stack from wherever the
// previous pane left the cursor.
void begin_tab_content();

// Places and opens the pane. Always pair with end_pane(), including on early
// returns, exactly as ImGui::Begin/End required.
void begin_pane(PanelSlot slot);
void end_pane();

}  // namespace Charting

#endif

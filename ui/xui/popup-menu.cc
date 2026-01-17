//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "ui/xemu-notifications.h"
#include <string>
#include <vector>
#include <filesystem>
#include <map>
#include <cstring>
#include <algorithm>
#include "misc.hh"
#include "actions.hh"
#include "font-manager.hh"
#include "viewport-manager.hh"
#include "scene-manager.hh"
#include "popup-menu.hh"
#include "input-manager.hh"
#include "xemu-hud.h"
#include "IconsFontAwesome6.h"
#include "../xemu-snapshots.h"
#include "main-menu.hh"
#include "../xemu-patches.h"

PopupMenuItemDelegate::~PopupMenuItemDelegate() {}
void PopupMenuItemDelegate::PushMenu(PopupMenu &menu) {}
void PopupMenuItemDelegate::PopMenu() {}
void PopupMenuItemDelegate::ClearMenuStack() {}
void PopupMenuItemDelegate::LostFocus() {}
void PopupMenuItemDelegate::PushFocus() {}
void PopupMenuItemDelegate::PopFocus() {}
bool PopupMenuItemDelegate::DidPop() { return false; }

static bool PopupMenuButton(std::string text, std::string icon = "")
{
    ImGui::PushFont(g_font_mgr.m_menu_font);
    auto button_text = string_format("%s %s", icon.c_str(), text.c_str());
    bool status = ImGui::Button(button_text.c_str(), ImVec2(-FLT_MIN, 0));
    ImGui::PopFont();
    return status;
}

static bool PopupMenuCheck(std::string text, std::string icon = "", bool v = false)
{
    bool status = PopupMenuButton(text, icon);
    if (v) {
        ImGui::PushFont(g_font_mgr.m_menu_font);
        const ImVec2 p0 = ImGui::GetItemRectMin();
        const ImVec2 p1 = ImGui::GetItemRectMax();
        const char *check_icon = ICON_FA_CHECK;
        ImVec2 ts_icon = ImGui::CalcTextSize(check_icon);
        ImDrawList *draw_list = ImGui::GetWindowDrawList();
        ImGuiStyle &style = ImGui::GetStyle();
        draw_list->AddText(ImVec2(p1.x - style.FramePadding.x - ts_icon.x,
                                  p0.y + (p1.y - p0.y - ts_icon.y) / 2),
                           ImGui::GetColorU32(ImGuiCol_Text), check_icon);
        ImGui::PopFont();
    }
    return status;
}

static bool PopupMenuSubmenuButton(std::string text, std::string icon = "")
{
    bool status = PopupMenuButton(text, icon);

    ImGui::PushFont(g_font_mgr.m_menu_font);
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    const char *right_icon = ICON_FA_CHEVRON_RIGHT;
    ImVec2 ts_icon = ImGui::CalcTextSize(right_icon);
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    ImGuiStyle &style = ImGui::GetStyle();
    draw_list->AddText(ImVec2(p1.x - style.FramePadding.x - ts_icon.x,
                              p0.y + (p1.y - p0.y - ts_icon.y) / 2),
                       ImGui::GetColorU32(ImGuiCol_Text), right_icon);
    ImGui::PopFont();
    return status;
}

static bool PopupMenuToggle(std::string text, std::string icon = "", bool *v = nullptr)
{
    bool l_v = false;
    if (v == NULL) v = &l_v;

    ImGuiStyle &style = ImGui::GetStyle();
    bool status = PopupMenuButton(text, icon);
    ImVec2 p_min = ImGui::GetItemRectMin();
    ImVec2 p_max = ImGui::GetItemRectMax();
    if (status) *v = !*v;

    ImGui::PushFont(g_font_mgr.m_menu_font);
    float title_height = ImGui::GetTextLineHeight();
    ImGui::PopFont();

    float toggle_height = title_height * 0.75;
    ImVec2 toggle_size(toggle_height * 1.75, toggle_height);
    ImVec2 toggle_pos(p_max.x - toggle_size.x - style.FramePadding.x,
                      p_min.y + (title_height - toggle_size.y)/2 + style.FramePadding.y);
    DrawToggle(*v, ImGui::IsItemHovered(), toggle_pos, toggle_size);

    return status;
}

static bool PopupMenuSlider(std::string text, std::string icon = "", float *v = NULL)
{
    bool status = PopupMenuButton(text, icon);
    ImVec2 p_min = ImGui::GetItemRectMin();
    ImVec2 p_max = ImGui::GetItemRectMax();

    ImGuiStyle &style = ImGui::GetStyle();

    float new_v = *v;

    if (ImGui::IsItemHovered()) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadRStickLeft)) new_v -= 0.05;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadRStickRight)) new_v += 0.05;
    }

    ImGui::PushFont(g_font_mgr.m_menu_font);
    float title_height = ImGui::GetTextLineHeight();
    ImGui::PopFont();

    float toggle_height = title_height * 0.75;
    ImVec2 slider_size(toggle_height * 3.75, toggle_height);
    ImVec2 slider_pos(p_max.x - slider_size.x - style.FramePadding.x,
                      p_min.y + (title_height - slider_size.y)/2 + style.FramePadding.y);

    if (ImGui::IsItemActive()) {
        ImVec2 mouse = ImGui::GetMousePos();
        new_v = GetSliderValueForMousePos(mouse, slider_pos, slider_size);
    }

    DrawSlider(*v, ImGui::IsItemActive() || ImGui::IsItemHovered(), slider_pos,
               slider_size);

    *v = fmin(fmax(0, new_v), 1.0);

    return status;
}

PopupMenu::PopupMenu() : m_animation(0.12, 0.12), m_ease_direction(0, 0)
{
    m_focus = false;
    m_pop_focus = false;
}

void PopupMenu::InitFocus()
{
    m_pop_focus = true;
}

PopupMenu::~PopupMenu()
{

}

void PopupMenu::Show(const ImVec2 &direction)
{
    m_animation.EaseIn();
    m_ease_direction = direction;
    m_focus = true;
}

void PopupMenu::Hide(const ImVec2 &direction)
{
    m_animation.EaseOut();
    m_ease_direction = direction;
}

bool PopupMenu::IsAnimating()
{
    return m_animation.IsAnimating();
}

void PopupMenu::Draw(PopupMenuItemDelegate &nav)
{
    m_animation.Step();

    ImGuiIO &io = ImGui::GetIO();
    float t = m_animation.GetSinInterpolatedValue();
    float window_alpha = t;
    ImVec2 window_pos = ImVec2(io.DisplaySize.x / 2 + (1-t) * m_ease_direction.x,
                               io.DisplaySize.y / 2 + (1-t) * m_ease_direction.y);

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, window_alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        g_viewport_mgr.Scale(ImVec2(10, 5)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, 0.5));
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_WindowBg));
    ImGui::PushStyleColor(ImGuiCol_NavHighlight, IM_COL32_BLACK_TRANS);

    if (m_focus) ImGui::SetNextWindowFocus();
    ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, ImVec2(0.5, 0.5));
    
    // Check if this menu needs a wider window for table display
    float window_width = 400 * g_viewport_mgr.m_scale;
    if (NeedsWiderWindow()) {
        window_width = 1100 * g_viewport_mgr.m_scale;
    }
    ImGui::SetNextWindowSize(ImVec2(window_width, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0);

    ImGui::Begin("###PopupMenu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    if (DrawItems(nav)) nav.PopMenu();
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow)) nav.LostFocus();
    ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 sz = ImGui::GetWindowSize();
    ImGui::End();

    if (!g_input_mgr.IsNavigatingWithController()) {
        ImGui::PushFont(g_font_mgr.m_menu_font);
        pos.y -= ImGui::GetFrameHeight();
        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(ImVec2(sz.x, ImGui::GetFrameHeight()));
        ImGui::SetNextWindowBgAlpha(0);
        ImGui::Begin("###PopupMenuNav", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 200));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32_BLACK_TRANS);
        if (ImGui::Button(ICON_FA_ARROW_LEFT)) {
            nav.PopMenu();
        }
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetTextLineHeight());
        if (ImGui::Button(ICON_FA_XMARK)) {
            nav.ClearMenuStack();
        }
        ImGui::PopStyleColor(2);
        ImGui::End();
        ImGui::PopFont();
    }

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(7);
    m_pop_focus = false;
    m_focus = false;
}

bool PopupMenu::DrawItems(PopupMenuItemDelegate &nav)
{
    return false;
}

class DisplayModePopupMenu : public virtual PopupMenu {
public:
    bool DrawItems(PopupMenuItemDelegate &nav) override
    {
        const char *values[] = {
            "Center", "Scale", "Stretch"
        };

        for (int i = 0; i < CONFIG_DISPLAY_UI_FIT__COUNT; i++) {
            bool selected = g_config.display.ui.fit == i;
            if (m_focus && selected) ImGui::SetKeyboardFocusHere();
            if (PopupMenuCheck(values[i], "", selected))
                g_config.display.ui.fit = i;
        }

        return false;
    }
};

class AspectRatioPopupMenu : public virtual PopupMenu {
public:
    bool DrawItems(PopupMenuItemDelegate &nav) override
    {
        const char *values[] = {
            "Native",
            "Auto (Default)",
            "4:3",
            "16:9"
        };

        for (int i = 0; i < CONFIG_DISPLAY_UI_ASPECT_RATIO__COUNT; i++) {
            bool selected = g_config.display.ui.aspect_ratio == i;
            if (m_focus && selected) ImGui::SetKeyboardFocusHere();
            if (PopupMenuCheck(values[i], "", selected))
                g_config.display.ui.aspect_ratio = i;
        }

        return false;
    }
};

extern MainMenuScene g_main_menu;

class SettingsPopupMenu : public virtual PopupMenu {
protected:
    DisplayModePopupMenu display_mode;
    AspectRatioPopupMenu aspect_ratio;

public:
    bool DrawItems(PopupMenuItemDelegate &nav) override
    {
        bool pop = false;

        if (m_focus && !m_pop_focus) {
            ImGui::SetKeyboardFocusHere();
        }
        PopupMenuSlider("Volume", ICON_FA_VOLUME_HIGH, &g_config.audio.volume_limit);
        bool fs = xemu_is_fullscreen();
        if (PopupMenuToggle("Fullscreen", ICON_FA_WINDOW_MAXIMIZE, &fs)) {
            xemu_toggle_fullscreen();
        }
        if (PopupMenuSubmenuButton("Display Mode", ICON_FA_EXPAND)) {
            nav.PushFocus();
            nav.PushMenu(display_mode);
        }
        if (PopupMenuSubmenuButton("Aspect Ratio", ICON_FA_EXPAND)) {
            nav.PushFocus();
            nav.PushMenu(aspect_ratio);
        }
        if (PopupMenuButton("Snapshots...", ICON_FA_CLOCK_ROTATE_LEFT)) {
            nav.ClearMenuStack();
            g_scene_mgr.PushScene(g_main_menu);
            g_main_menu.ShowSnapshots();
        }
        if (PopupMenuButton("All settings...", ICON_FA_SLIDERS)) {
            nav.ClearMenuStack();
            g_scene_mgr.PushScene(g_main_menu);
        }
        if (m_pop_focus) {
            nav.PopFocus();
        }
        return pop;
    }
};

class GamesPopupMenu : public virtual PopupMenu {
protected:
    std::multimap<std::string, std::string> sorted_file_names;

public:
    void Show(const ImVec2 &direction) override
    {
        PopupMenu::Show(direction);
        PopulateGameList();
    }

    bool DrawItems(PopupMenuItemDelegate &nav) override
    {
        bool pop = false;

        if (m_focus && !m_pop_focus) {
            ImGui::SetKeyboardFocusHere();
        }

        for (const auto &[label, file_path] : sorted_file_names) {
            if (PopupMenuButton(label, ICON_FA_COMPACT_DISC)) {
                ActionLoadDiscFile(file_path.c_str());
                nav.ClearMenuStack();
                pop = true;
            }
        }

        if (sorted_file_names.size() == 0) {
            if (PopupMenuButton("No games found", ICON_FA_SLIDERS)) {
                nav.ClearMenuStack();
                g_scene_mgr.PushScene(g_main_menu);
            }
        }

        if (m_pop_focus) {
            nav.PopFocus();
        }
        return pop;
    }

    void PopulateGameList() {
        const char *games_dir = g_config.general.games_dir;

        sorted_file_names.clear();
        std::filesystem::path directory(games_dir);
        if (std::filesystem::is_directory(directory)) {
            for (const auto &file :
                 std::filesystem::directory_iterator(directory)) {
                const auto &file_path = file.path();
                if (std::filesystem::is_regular_file(file_path) &&
                    (file_path.extension() == ".iso" ||
                     file_path.extension() == ".xiso")) {
                    sorted_file_names.insert(
                        { file_path.stem().string(), file_path.string() });
                }
            }
        }
    }
};

// PatchesPopupMenu class definition

class PatchesPopupMenu : public virtual PopupMenu {
protected:
    std::vector<XemuMemoryPatch*> sorted_patches;
    char* current_disc_path_cache;
    XemuGamePatches* current_game_patches;
    bool is_visible;
    
    // Table sorting state
    enum SortColumn {
        SORT_PATCH_TITLE = 0,
        SORT_CATEGORY = 1,
        SORT_STATE = 2
    };
    
    struct SortState {
        SortColumn column;
        bool ascending;
        SortState() : column(SORT_PATCH_TITLE), ascending(true) {}
    } sort_state;
    
    // Helper function to render sort indicators with fallback for font compatibility
    void RenderSortIndicator(bool ascending) {
        // Use ASCII arrows as fallback since Unicode arrows may not render properly
        // This ensures consistent display across different font configurations
        ImGui::Text("%s", ascending ? " ^" : " v");
    }
    
    // Game switching detection
    char* last_detected_disc_path;
    bool need_refresh;
    
    // Controller navigation state
    int selected_row;
    int selected_column;
    bool in_header_mode;  // true when navigating headers, false when navigating data rows
    
    // Visual feedback for recent changes
    int last_modified_row;
    int frames_since_modification;
    
    // State tracking
    bool game_not_in_database;

public:
    PatchesPopupMenu() : sorted_patches(), current_disc_path_cache(nullptr), current_game_patches(nullptr), 
                        is_visible(false), sort_state(), last_detected_disc_path(nullptr), need_refresh(true), 
                        selected_row(-1), selected_column(0), in_header_mode(false), last_modified_row(-1), frames_since_modification(0), 
                        game_not_in_database(false) { }

    ~PatchesPopupMenu() {
        if (current_disc_path_cache) {
            g_free(current_disc_path_cache);
        }
        if (last_detected_disc_path) {
            g_free(last_detected_disc_path);
        }
    }
    
    // Function implementations for dual matching strategy
    void CheckForGameSwitch() {
        char* current_disc_path = xemu_get_currently_loaded_disc_path();
        if (!current_disc_path) {
            game_not_in_database = true;
            return;
        }

        // Update current disc path cache for comparison
        if (last_detected_disc_path && strcmp(last_detected_disc_path, current_disc_path) == 0) {
            // Same game, no switch detected
            g_free(current_disc_path);
            return;
        }

        // Game has been switched, update cache and trigger refresh
        if (last_detected_disc_path) {
            g_free(last_detected_disc_path);
        }
        last_detected_disc_path = current_disc_path;
        need_refresh = true;
        game_not_in_database = false;
    }

    void PopulatePatchList() {
        // Clear existing patches
        sorted_patches.clear();
        current_game_patches = nullptr;

        if (!g_patches_loaded || !g_patches_initialized) {
            game_not_in_database = true;
            return;
        }

        XemuGamePatches* game = nullptr;
        game = xemu_patches_find_game_by_certificate();

        if (!game && last_detected_disc_path) {
            game = xemu_patches_find_game_by_filename(last_detected_disc_path);
        }

        if (!game) {
            game_not_in_database = true;
            return;
        }

        game_not_in_database = false;
        current_game_patches = game;

        // Populate sorted patches vector
        for (int i = 0; i < game->patch_count; i++) {
            sorted_patches.push_back(&game->patches[i]);
        }

        // Apply initial sorting using current sort state
        ApplySorting(sort_state.column, sort_state.ascending);

        // Initialize selection to first patch when patches are loaded
        if (sorted_patches.size() > 0) {
            selected_row = 0;
            selected_column = 0;
        }
    }
    
    // Custom sorting function for manual column sorting
    void ApplySorting(SortColumn sort_column, bool ascending) {
        if (sorted_patches.size() <= 1) return;
        
        std::sort(sorted_patches.begin(), sorted_patches.end(),
            [sort_column, ascending](const XemuMemoryPatch* a, const XemuMemoryPatch* b) {
                int delta = 0;
                
                switch (sort_column) {
                    case SORT_PATCH_TITLE: {
                        const char* name_a = a->name ? a->name : "";
                        const char* name_b = b->name ? b->name : "";
                        delta = strcmp(name_a, name_b);
                        break;
                    }
                    case SORT_CATEGORY: {
                        const char* cat_a = a->category ? a->category : "";
                        const char* cat_b = b->category ? b->category : "";
                        delta = strcmp(cat_a, cat_b);
                        break;
                    }
                    case SORT_STATE: {
                        delta = (a->enabled ? 1 : 0) - (b->enabled ? 1 : 0);
                        break;
                    }
                }
                
                return ascending ? (delta < 0) : (delta > 0);
            });
        
        // Update our sort state to track current sorting
        sort_state.column = sort_column;
        sort_state.ascending = ascending;
        
        // Re-initialize selection to first patch after sorting
        if (sorted_patches.size() > 0) {
            selected_row = 0;
        }
    }
    
    bool IsVisible() const { 
        // Check if patches system is available and there are patches for current game
        if (!g_patches_loaded || !g_patches_initialized) {
            return false;
        }
        
        char* current_disc_path = xemu_get_currently_loaded_disc_path();
        if (!current_disc_path) return false;
        
        // Try certificate-based matching first (most reliable)
        XemuGamePatches* game = xemu_patches_find_game_by_certificate();
        if (!game) {
            // Fallback to filename matching
            game = xemu_patches_find_game_by_filename(current_disc_path);
        }
        g_free(current_disc_path);
        return (game != nullptr && game->patch_count > 0);
    }

    bool NeedsWiderWindow() const override { 
        // Patches menu needs wider window to display the 3-column table properly
        return true; 
    }

    // Helper function to find game and patch indices from a patch pointer
    bool GetPatchIndices(XemuMemoryPatch* patch, int* out_game_index, int* out_patch_index) {
        if (!patch || !out_game_index || !out_patch_index) {
            return false;
        }
        
        // Search through all games to find the patch using CONTENT-BASED matching
        for (int game_idx = 0; game_idx < g_patches_db.game_count; game_idx++) {
            XemuGamePatches* game = &g_patches_db.games[game_idx];
            for (int patch_idx = 0; patch_idx < game->patch_count; patch_idx++) {
                // Try POINTER comparison first (fast path)
                if (&game->patches[patch_idx] == patch) {
                    *out_game_index = game_idx;
                    *out_patch_index = patch_idx;
                    return true;
                }
                
                // Try NAME-based comparison (content-based matching)
                if (patch->name && game->patches[patch_idx].name && 
                    strcmp(patch->name, game->patches[patch_idx].name) == 0) {
                    *out_game_index = game_idx;
                    *out_patch_index = patch_idx;
                    return true;
                }
            }
        }
        return false;
    }

    // Handle controller navigation for table
    void HandleControllerNavigation(PopupMenuItemDelegate &nav) {
        if (sorted_patches.empty() || !m_focus) {
            return;
        }
        
        // Get input state
        bool dpad_up = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp) || ImGui::IsKeyPressed(ImGuiKey_UpArrow);
        bool dpad_down = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) || ImGui::IsKeyPressed(ImGuiKey_DownArrow);
        bool dpad_left = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
        bool dpad_right = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow);
        bool a_pressed = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown) || ImGui::IsKeyPressed(ImGuiKey_Space);
        
        // Handle navigation
        if (dpad_up && sorted_patches.size() > 0) {
            if (in_header_mode) {
                // From header mode, stay in header mode
            } else {
                // Initialize to last row if no current selection
                if (selected_row < 0) {
                    selected_row = (int)sorted_patches.size() - 1;
                } else if (selected_row > 0) {
                    selected_row--;
                } else if (selected_row == 0) {
                    // At first row, UP goes to header mode
                    in_header_mode = true;
                    selected_row = -1;  // No row selected in header mode
                }
            }
        } else if (dpad_down && sorted_patches.size() > 0) {
            if (in_header_mode) {
                // From header mode, enter data mode and go to first row
                in_header_mode = false;
                selected_row = 0;
            } else {
                // Initialize to first row if no current selection
                if (selected_row < 0) {
                    selected_row = 0;
                } else if (selected_row < (int)sorted_patches.size() - 1) {
                    selected_row++;
                    // Exit header mode when moving to data rows
                    in_header_mode = false;
                }
            }
        } else if (dpad_left) {
            if (in_header_mode) {
                if (selected_column > 0) {
                    selected_column--;
                }
            } else {
                if (selected_column > 0) {
                    selected_column--;
                }
            }
        } else if (dpad_right) {
            if (in_header_mode) {
                if (selected_column < 2) {
                    selected_column++;
                }
            } else {
                if (selected_column < 2) {
                    selected_column++;
                }
            }
        } else if (dpad_up && selected_row == 0) {
            // From first row, enter header mode
            in_header_mode = true;
        } else if (a_pressed) {
            if (in_header_mode) {
                // A pressed on header - toggle sort for the selected column
                SortColumn sort_column = SORT_PATCH_TITLE;
                switch (selected_column) {
                    case 0: sort_column = SORT_PATCH_TITLE; break;
                    case 1: sort_column = SORT_CATEGORY; break;
                    case 2: sort_column = SORT_STATE; break;
                }
                
                // Toggle sort direction if same column, or use ascending for new column
                bool new_ascending = true;
                if (sort_state.column == sort_column) {
                    new_ascending = !sort_state.ascending;
                }
                
                ApplySorting(sort_column, new_ascending);
            } else if (selected_row >= 0) {
                // A pressed on data row - existing toggle functionality
                // Toggle checkbox when A is pressed on state column
                if (selected_column == 2) {
                    XemuMemoryPatch* patch = sorted_patches[selected_row];
                    bool new_enabled = !patch->enabled;
                    
                    // Reset modification tracking for new change
                    last_modified_row = selected_row;
                    frames_since_modification = 0;
                    
                    // Get game and patch indices
                    int game_index, patch_index;
                    if (GetPatchIndices(patch, &game_index, &patch_index)) {
                        if (xemu_patches_set_patch_enabled(game_index, patch_index, new_enabled)) {
                            if (new_enabled) {
                                xemu_patches_apply_patch_with_save_restore(patch, game_index, patch_index);
                            } else {
                                xemu_patches_remove_patch_with_restore(game_index, patch_index);
                            }
                        }
                    }
                }
            }
        }
    }


    void Show(const ImVec2 &direction) override
    {
        PopupMenu::Show(direction);
        is_visible = true;
        // Always populate patch list when showing the menu
        need_refresh = true;
        PopulatePatchList();
        
        // Initialize selection to first patch and set to data mode when window opens
        if (sorted_patches.size() > 0) {
            selected_row = 0;
            selected_column = 0;
            in_header_mode = false;
        } else {
            // No patches, start in header mode
            selected_row = -1;
            selected_column = 0;
            in_header_mode = true;
        }
    }
    
    void Hide(const ImVec2 &direction)
    {
        PopupMenu::Hide(direction);
        is_visible = false;
    }

    bool DrawItems(PopupMenuItemDelegate &nav) override
    {
        bool pop = false;

        // Check for game switching and handle updates
        CheckForGameSwitch();
        
        // Always refresh patch list if needed or if this is the first time showing the menu
        // Force immediate refresh when need_refresh is true to update display immediately
        if (need_refresh || !current_disc_path_cache) {
            PopulatePatchList();
            need_refresh = false;
        }
        
        // Ensure selection is properly initialized for current patch list
        if (sorted_patches.size() > 0) {
            if (selected_row < 0 || selected_row >= (int)sorted_patches.size()) {
                selected_row = 0;
                selected_column = 0;
            }
        }
        
        // If game not in database after refresh, kick user back to parent menu
        if (game_not_in_database) {
            nav.PopMenu();
            pop = true;
            return pop;
        }

        if (m_focus && !m_pop_focus) {
            ImGui::SetKeyboardFocusHere();
        }

        if (!current_game_patches) {
            if (PopupMenuButton("No disc loaded or game not in database", ICON_FA_COMPACT_DISC)) {
                nav.ClearMenuStack();
                pop = true;
            }
            return pop;
        }
        
        // Check if we have patches to display
        bool has_patches = (sorted_patches.size() > 0 && current_game_patches && current_game_patches->patch_count > 0);
        
        if (!has_patches) {
            // Show game information with XBE certificate details
            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Game: %s", current_game_patches->game_title);
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "TitleID: %s | Region: %s | Version: %s", 
                               current_game_patches->title_id ? current_game_patches->title_id : "Unknown",
                               current_game_patches->region ? current_game_patches->region : "Unknown",
                               current_game_patches->version ? current_game_patches->version : "Unknown");
            
            // Show XBE certificate additional info
            if (current_game_patches->alternate_title_id && strlen(current_game_patches->alternate_title_id) > 0) {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Alt TitleID: %s", current_game_patches->alternate_title_id);
            }
            if (current_game_patches->time_date && strlen(current_game_patches->time_date) > 0) {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Build Date: %s", current_game_patches->time_date);
            }
            if (current_game_patches->disc_number && strlen(current_game_patches->disc_number) > 0) {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Disc: %s", current_game_patches->disc_number);
            }
            
            ImGui::Separator();
            
            // Game is loaded but no patches available
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No patches available for this game");
            return pop;
        }
        else {
            // Show patches table
            
            // Create transparent backdrop for patches section
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15, 15));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.75f)); // More transparent dark background
            // Wide window to accommodate full table with all 3 columns
            ImGui::BeginChild("PatchesContent", ImVec2(1050, 600), true, ImGuiWindowFlags_ChildWindow);
            
            // Show game information with XBE certificate details
            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Game: %s", current_game_patches->game_title);
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "TitleID: %s | Region: %s | Version: %s", 
                               current_game_patches->title_id ? current_game_patches->title_id : "Unknown",
                               current_game_patches->region ? current_game_patches->region : "Unknown",
                               current_game_patches->version ? current_game_patches->version : "Unknown");
            
            // Show XBE certificate additional info
            if (current_game_patches->alternate_title_id && strlen(current_game_patches->alternate_title_id) > 0) {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Alt TitleID: %s", current_game_patches->alternate_title_id);
            }
            if (current_game_patches->time_date && strlen(current_game_patches->time_date) > 0) {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Build Date: %s", current_game_patches->time_date);
            }
            if (current_game_patches->disc_number && strlen(current_game_patches->disc_number) > 0) {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Disc: %s", current_game_patches->disc_number);
            }
            
            // Count enabled patches
            int enabled_count = 0;
            for (const auto* patch : sorted_patches) {
                if (patch && patch->enabled) {
                    enabled_count++;
                }
            }
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Available patches: %zu (%d Enabled)", sorted_patches.size(), enabled_count);
            ImGui::Separator();
            
            // Create patches table with 3 columns: Patch Title, Category, State
            ImGui::PushFont(g_font_mgr.m_menu_font);
            
            // Set focus to enable keyboard/gamepad navigation
            if (m_focus && !m_pop_focus) {
                ImGui::SetKeyboardFocusHere();
            }
            
            if (ImGui::BeginTable("PatchesTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoHostExtendX)) {
                // Setup columns with dynamic sizing - utilize the full width of the wider popup
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.65f);
                ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 220.0f);
                ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                
                // Custom header row with dark blue styling and sort indicators
                ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                ImGui::PushFont(g_font_mgr.m_menu_font);
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                
                // Name column header
                ImGui::TableNextColumn();
                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ImVec4(0.05f, 0.15f, 0.35f, 1.0f)));
                
                // Add highlight if this column is selected in header mode
                if (in_header_mode && selected_column == 0) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ImVec4(0.3f, 0.5f, 0.8f, 1.0f)));
                }
                
                // Create a clickable area for the header
                bool clicked_header = false;
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.1f, 0.1f, 0.5f));
                if (ImGui::Button("##NameHeader", ImVec2(ImGui::GetColumnWidth(), ImGui::GetTextLineHeightWithSpacing()))) {
                    if (sort_state.column == SORT_PATCH_TITLE) {
                        ApplySorting(SORT_PATCH_TITLE, !sort_state.ascending);
                    } else {
                        ApplySorting(SORT_PATCH_TITLE, true);
                    }
                    selected_column = 0;
                    in_header_mode = true;
                    clicked_header = true;
                }
                ImGui::PopStyleColor();
                ImGui::PopStyleColor();
                
                // Render header text (only if not clicked, to avoid duplicate rendering)
                if (!clicked_header) {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8); // Add padding
                    ImGui::Text("Name");
                }
                
                // Add sort indicator if this column is the current sort column
                if (sort_state.column == SORT_PATCH_TITLE) {
                    ImGui::SameLine();
                    RenderSortIndicator(sort_state.ascending);
                }
                
                // Category column header
                ImGui::TableNextColumn();
                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ImVec4(0.05f, 0.15f, 0.35f, 1.0f)));
                
                // Add highlight if this column is selected in header mode
                if (in_header_mode && selected_column == 1) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ImVec4(0.3f, 0.5f, 0.8f, 1.0f)));
                }
                
                // Create a clickable area for the header
                clicked_header = false;
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.1f, 0.1f, 0.5f));
                if (ImGui::Button("##CategoryHeader", ImVec2(ImGui::GetColumnWidth(), ImGui::GetTextLineHeightWithSpacing()))) {
                    if (sort_state.column == SORT_CATEGORY) {
                        ApplySorting(SORT_CATEGORY, !sort_state.ascending);
                    } else {
                        ApplySorting(SORT_CATEGORY, true);
                    }
                    selected_column = 1;
                    in_header_mode = true;
                    clicked_header = true;
                }
                ImGui::PopStyleColor();
                ImGui::PopStyleColor();
                
                // Render header text (only if not clicked, to avoid duplicate rendering)
                if (!clicked_header) {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8); // Add padding
                    ImGui::Text("Category");
                }
                
                // Add sort indicator if this column is the current sort column
                if (sort_state.column == SORT_CATEGORY) {
                    ImGui::SameLine();
                    RenderSortIndicator(sort_state.ascending);
                }
                
                // State column header
                ImGui::TableNextColumn();
                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ImVec4(0.05f, 0.15f, 0.35f, 1.0f)));
                
                // Add highlight if this column is selected in header mode
                if (in_header_mode && selected_column == 2) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ImVec4(0.3f, 0.5f, 0.8f, 1.0f)));
                }
                
                // Create a clickable area for the header
                clicked_header = false;
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.1f, 0.1f, 0.5f));
                if (ImGui::Button("##StateHeader", ImVec2(ImGui::GetColumnWidth(), ImGui::GetTextLineHeightWithSpacing()))) {
                    if (sort_state.column == SORT_STATE) {
                        ApplySorting(SORT_STATE, !sort_state.ascending);
                    } else {
                        ApplySorting(SORT_STATE, true);
                    }
                    selected_column = 2;
                    in_header_mode = true;
                    clicked_header = true;
                }
                ImGui::PopStyleColor();
                ImGui::PopStyleColor();
                
                // Render header text (only if not clicked, to avoid duplicate rendering)
                if (!clicked_header) {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8); // Add padding
                    ImGui::Text("State");
                }
                
                // Add sort indicator if this column is the current sort column
                if (sort_state.column == SORT_STATE) {
                    ImGui::SameLine();
                    RenderSortIndicator(sort_state.ascending);
                }
                
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
                ImGui::PopFont();
                
                // Manual sorting is handled by user input, not ImGui automatic sorting
                // The patches are sorted according to sort_state when user presses A on headers
                
                // Populate table rows
                for (size_t i = 0; i < sorted_patches.size(); i++) {
                    XemuMemoryPatch* patch = sorted_patches[i];
                    
                    // Set up row highlighting BEFORE TableNextRow
                    bool is_selected_row = (selected_row >= 0 && selected_row == (int)i);
                    bool is_modified_row = (last_modified_row >= 0 && last_modified_row == (int)i);
                    
                    if (is_selected_row) {
                        // Light white highlighting for selected row
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.9f, 0.9f, 0.9f, 0.8f)));
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImVec4(0.9f, 0.9f, 0.9f, 0.8f)));
                    } else if (is_modified_row && frames_since_modification < 30) {
                        // Light green highlighting for recently modified row (temporary, for ~0.5 seconds at 60fps)
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.8f, 1.0f, 0.8f, 0.8f)));
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImVec4(0.8f, 1.0f, 0.8f, 0.8f)));
                        frames_since_modification++;
                    } else {
                        // Reset to default alternating colors for other rows
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.0f)));
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.0f)));
                    }
                    
                    ImGui::TableNextRow();
                    
                    // Clean up patch name - remove leading spaces
                    std::string clean_name = patch->name ? patch->name : "Unnamed Patch";
                    size_t start_pos = clean_name.find_first_not_of(" \t\r\n");
                    if (start_pos != std::string::npos) {
                        clean_name = clean_name.substr(start_pos);
                    }
                    
                    // Patch Title column
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", clean_name.c_str());
                    
                    // Category column
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", patch->category ? patch->category : "Uncategorized");
                    
                    // State column with checkbox
                    ImGui::TableNextColumn();
                    bool new_enabled = patch->enabled;
                    
                    // Create a checkbox for the state with unique ID
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 2));
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 4));
                    ImGui::PushID((int)(intptr_t)patch); // Use patch pointer as unique ID
                    
                    // Use proper patch enabling/disabling functions
                    if (ImGui::Checkbox("##PatchEnabled", &new_enabled)) {
                        // Update patch state first
                        patch->enabled = new_enabled;
                        
                        // Get game and patch indices
                        int game_index, patch_index;
                        if (GetPatchIndices(patch, &game_index, &patch_index)) {
                            // Use the proper function to enable/disable patch
                            if (xemu_patches_set_patch_enabled(game_index, patch_index, new_enabled)) {
                                // Apply or remove patch immediately using the proper function
                                if (new_enabled) {
                                    xemu_patches_apply_patch_with_save_restore(patch, game_index, patch_index);
                                } else {
                                    xemu_patches_remove_patch_with_restore(game_index, patch_index);
                                }
                            }
                        }
                    }
                    
                    ImGui::PopStyleVar(2); // Pop checkbox style variables for this row
                    ImGui::PopID(); // Pop the unique ID for this checkbox
                }
                
                ImGui::EndTable();
            }

            // Restore font
            ImGui::PopFont();

            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
        } // Close the else block for patches table

        if (m_pop_focus) {
            nav.PopFocus();
        }

        return pop;
    }
};


class RootPopupMenu : public virtual PopupMenu {
protected:
    SettingsPopupMenu settings;
    GamesPopupMenu games;
    PatchesPopupMenu patches;
    bool refocus_first_item;
    bool current_game_in_database;

public:
    RootPopupMenu() {
        refocus_first_item = false;
        current_game_in_database = false;
    }
    
    bool IsCurrentGameInDatabase() {
        if (!g_patches_loaded || !g_patches_initialized) {
            return false;
        }
        
        char* current_disc_path = xemu_get_currently_loaded_disc_path();
        if (!current_disc_path) {
            return false;
        }
        
        // Try certificate-based matching first (most reliable)
        XemuGamePatches* game_patches = xemu_patches_find_game_by_certificate();
        if (!game_patches) {
            // Fallback to filename matching
            game_patches = xemu_patches_find_game_by_filename(current_disc_path);
        }
        
        g_free(current_disc_path);
        return game_patches != nullptr;
    }

    bool DrawItems(PopupMenuItemDelegate &nav) override
    {
        bool pop = false;

        // Check if current game has patches available
        current_game_in_database = patches.IsVisible();

        if (refocus_first_item || (m_focus && !m_pop_focus)) {
            ImGui::SetKeyboardFocusHere();
            refocus_first_item = false;
        }

        bool running = runstate_is_running();
        if (running) {
            if (PopupMenuButton("Pause", ICON_FA_CIRCLE_PAUSE)) {
                ActionTogglePause();
                refocus_first_item = true;
            }
        } else {
            if (PopupMenuButton("Resume", ICON_FA_CIRCLE_PLAY)) {
                ActionTogglePause();
                refocus_first_item = true;
            }
        }
        if (PopupMenuButton("Screenshot", ICON_FA_CAMERA)) {
            ActionScreenshot();
            pop = true;
        }
        if (PopupMenuButton("Save Snapshot", ICON_FA_DOWNLOAD)) {
            xemu_snapshots_save(NULL, NULL);
            xemu_queue_notification("Created new snapshot");
            pop = true;
        }
        
        // Check if current game has patches available
        XemuGamePatches* game_patches = xemu_patches_find_game_by_certificate();
        bool has_patches = (game_patches != nullptr && game_patches->patch_count > 0);
        
        if (has_patches) {
            // Enabled patches button
            if (PopupMenuSubmenuButton("Patches", ICON_FA_GEARS)) {
                nav.PushFocus();
                nav.PushMenu(patches);
            }
        } else {
            // Disabled patches button - gray out if no patches available
            ImGui::PushFont(g_font_mgr.m_menu_font);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
            ImGui::Button(string_format("%s Patches", ICON_FA_GEARS).c_str(), ImVec2(-FLT_MIN, 0));
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        
        if (PopupMenuSubmenuButton("Games", ICON_FA_GAMEPAD)) {
            nav.PushFocus();
            nav.PushMenu(games);
        }
        if (PopupMenuButton("Eject Disc", ICON_FA_EJECT)) {
            ActionEjectDisc();
            pop = true;
        }
        if (PopupMenuButton("Load Disc...", ICON_FA_COMPACT_DISC)) {
            // Close entire popup menu when loading a new disc
            if (patches.IsVisible()) {
                nav.ClearMenuStack();
            }
            ActionLoadDisc();
            pop = true;
        }
        if (PopupMenuSubmenuButton("Settings", ICON_FA_GEARS)) {
            nav.PushFocus();
            nav.PushMenu(settings);
        }
        if (PopupMenuButton("Restart", ICON_FA_ARROWS_ROTATE)) {
            // Close entire popup menu when restarting
            if (patches.IsVisible()) {
                nav.ClearMenuStack();
            }
            ActionReset();
            pop = true;
        }
        if (PopupMenuButton("Exit", ICON_FA_POWER_OFF)) {
            ActionShutdown();
            pop = true;
        }

        if (m_pop_focus) {
            nav.PopFocus();
        }

        return pop;
    }
};

RootPopupMenu root_menu;

void PopupMenuScene::PushMenu(PopupMenu &menu)
{
    menu.Show(m_view_stack.size() ? EASE_VECTOR_LEFT : EASE_VECTOR_DOWN);
    m_menus_in_transition.push_back(&menu);

    if (m_view_stack.size()) {
        auto current = m_view_stack.back();
        m_menus_in_transition.push_back(current);
        current->Hide(EASE_VECTOR_RIGHT);
    }

    m_view_stack.push_back(&menu);
}

void PopupMenuScene::PopMenu()
{
    if (!m_view_stack.size()) {
        return;
    }

    if (m_view_stack.size() > 1) {
        auto previous = m_view_stack[m_view_stack.size() - 2];
        previous->Show(EASE_VECTOR_RIGHT);
        previous->InitFocus();
        m_menus_in_transition.push_back(previous);
    }

    auto current = m_view_stack.back();
    m_view_stack.pop_back();
    current->Hide(m_view_stack.size() ? EASE_VECTOR_LEFT : EASE_VECTOR_DOWN);
    m_menus_in_transition.push_back(current);

    if (!m_view_stack.size()) {
        Hide();
    }
}

void PopupMenuScene::PushFocus()
{
    ImGuiContext *g = ImGui::GetCurrentContext();
    m_focus_stack.push_back(std::pair<ImGuiID, ImRect>(g->LastItemData.ID,
                                                       g->LastItemData.Rect));
}

void PopupMenuScene::PopFocus()
{
    auto next_focus = m_focus_stack.back();
    m_focus_stack.pop_back();
    ImGuiContext *g = ImGui::GetCurrentContext();
    g->NavInitRequest = false;
    g->NavInitResult.ID = next_focus.first;
    g->NavInitResult.RectRel = ImGui::WindowRectAbsToRel(g->CurrentWindow,
                                                         next_focus.second);
    // ImGui::NavUpdateAnyRequestFlag();
    g->NavAnyRequest = g->NavMoveScoringItems || g->NavInitRequest;// || (IMGUI_DEBUG_NAV_SCORING && g->NavWindow != NULL);
}

void PopupMenuScene::ClearMenuStack()
{
    if (m_view_stack.size()) {
        auto current = m_view_stack.back();
        current->Hide(EASE_VECTOR_DOWN);
        m_menus_in_transition.push_back(current);
    }
    m_view_stack.clear();
    m_focus_stack.clear();
    Hide();
}

void PopupMenuScene::HandleInput()
{
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)
        || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        PopMenu();
    }
}

void PopupMenuScene::Show()
{
    m_background.Show();
    m_nav_control_view.Show();
    // m_big_state_icon.Show();
    // m_title_info.Show();

    if (m_view_stack.size() == 0) {
        PushMenu(root_menu);
    }
}

void PopupMenuScene::Hide()
{
    m_background.Hide();
    m_nav_control_view.Hide();
    // m_big_state_icon.Hide();
    // m_title_info.Hide();
}

bool PopupMenuScene::IsAnimating()
{
    return m_menus_in_transition.size() > 0 ||
           m_background.IsAnimating() ||
           m_nav_control_view.IsAnimating();
    // m_big_state_icon.IsAnimating() ||
    // m_title_info.IsAnimating();
}

bool PopupMenuScene::Draw()
{
    m_background.Draw();
    // m_big_state_icon.Draw();
    // m_title_info.Draw();

    bool displayed = false;
    while (m_menus_in_transition.size()) {
        auto current = m_menus_in_transition.back();
        if (current->IsAnimating()) {
            current->Draw(*this);
            displayed = true;
            break;
        }
        m_menus_in_transition.pop_back();
    }

    if (!displayed) {
        if (m_view_stack.size()) {
            m_view_stack.back()->Draw(*this);
            HandleInput();
            displayed = true;
        }
    }

    m_nav_control_view.Draw();
    return displayed || IsAnimating();
}

void PopupMenuScene::LostFocus()
{
    ClearMenuStack();
}

PopupMenuScene g_popup_menu;

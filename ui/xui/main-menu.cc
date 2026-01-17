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
#include "common.hh"
#include "scene-manager.hh"
#include "widgets.hh"
#include "main-menu.hh"
#include "font-manager.hh"
#include "input-manager.hh"
#include "snapshot-manager.hh"
#include "viewport-manager.hh"
#include "xemu-hud.h"
#include "misc.hh"
#include "gl-helpers.hh"
#include "reporting.hh"
#include <ctype.h>

// Case-insensitive search helper for Windows compatibility
static const char* stristr(const char* str, const char* find)
{
    if (!str || !find) return nullptr;
    
    const char* s = str;
    const char* f = find;
    
    while (*s) {
        if (tolower(*s) == tolower(*f)) {
            const char* ss = s;
            const char* ff = f;
            
            while (*ss && *ff && tolower(*ss) == tolower(*ff)) {
                ss++;
                ff++;
            }
            
            if (*ff == '\0') {
                return s;
            }
        }
        s++;
    }
    
    return nullptr;
}
#include "qapi/error.h"
#include "actions.hh"

#include "../xemu-input.h"
#include "../xemu-notifications.h"
#include "../xemu-settings.h"
#include "../xemu-monitor.h"
#include "../xemu-version.h"
#include "../xemu-net.h"
#include "../xemu-os-utils.h"
#include "../xemu-xbe.h"
#include "../xemu-patches.h"
#include <algorithm>
#include <vector>

#include "../thirdparty/fatx/fatx.h"

#define DEFAULT_XMU_SIZE 8388608

// Helper function to trim whitespace from the beginning and end of a string
static void trim_string(char *str) {
    if (!str) return;
    
    // Trim leading whitespace
    char *start = str;
    while (isspace((unsigned char)*start)) start++;
    
    // Trim trailing whitespace  
    char *end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;
    
    // Terminate string after last non-whitespace character
    *(end + 1) = '\0';
    
    // If we found non-whitespace characters, move them to the beginning
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

// REMOVED: unused function free_address_value_array

// REMOVED: unused function render_highlighted_addresses

MainMenuScene g_main_menu;

MainMenuTabView::~MainMenuTabView() {}
void MainMenuTabView::Draw()
{
}

void MainMenuGeneralView::Draw()
{
#if defined(_WIN32)
    SectionTitle("Updates");
    Toggle("Check for updates", &g_config.general.updates.check,
           "Check for updates whenever xemu is opened");
#endif

#if defined(__x86_64__)
    SectionTitle("Performance");
    Toggle("Hard FPU emulation", &g_config.perf.hard_fpu,
           "Use hardware-accelerated floating point emulation (requires restart)");
#endif

    Toggle("Cache shaders to disk", &g_config.perf.cache_shaders,
           "Reduce stutter in games by caching previously generated shaders");

    SectionTitle("Miscellaneous");
    Toggle("Skip startup animation", &g_config.general.skip_boot_anim,
           "Skip the full Xbox boot animation sequence");
    FilePicker("Screenshot output directory", &g_config.general.screenshot_dir,
               NULL, true);
    FilePicker("Games directory", &g_config.general.games_dir, NULL, true);
    // toggle("Throttle DVD/HDD speeds", &g_config.general.throttle_io,
    //        "Limit DVD/HDD throughput to approximate Xbox load times");
}

bool MainMenuInputView::ConsumeRebindEvent(SDL_Event *event)
{
    if (!m_rebinding) {
        return false;
    }

    RebindEventResult rebind_result = m_rebinding->ConsumeRebindEvent(event);
    if (rebind_result == RebindEventResult::Complete) {
        m_rebinding = nullptr;
    }

    return rebind_result == RebindEventResult::Ignore;
}

bool MainMenuInputView::IsInputRebinding()
{
    return m_rebinding != nullptr;
}

void MainMenuInputView::Draw()
{
    SectionTitle("Controllers");
    ImGui::PushFont(g_font_mgr.m_menu_font_small);

    static int active = 0;

    // Output dimensions of texture
    float t_w = 512, t_h = 512;
    // Dimensions of (port+label)s
    float b_x = 0, b_x_stride = 100, b_y = 400;
    float b_w = 68, b_h = 81;
    // Dimensions of controller (rendered at origin)
    float controller_width  = 477.0f;
    float controller_height = 395.0f;
    // Dimensions of XMU
    float xmu_x = 0, xmu_x_stride = 256, xmu_y = 0;
    float xmu_w = 256, xmu_h = 256;

    // Setup rendering to fbo for controller and port images
    controller_fbo->Target();
    ImTextureID id = (ImTextureID)(intptr_t)controller_fbo->Texture();

    //
    // Render buttons with icons of the Xbox style port sockets with
    // circular numbers above them. These buttons can be activated to
    // configure the associated port, like a tabbed interface.
    //
    ImVec4 color_active(0.50, 0.86, 0.54, 0.12);
    ImVec4 color_inactive(0, 0, 0, 0);

    // Begin a 4-column layout to render the ports
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        g_viewport_mgr.Scale(ImVec2(0, 12)));
    ImGui::Columns(4, "mixed", false);

    const int port_padding = 8;
    for (int i = 0; i < 4; i++) {
        bool is_selected = (i == active);
        bool port_is_bound = (xemu_input_get_bound(i) != NULL);

        // Set an X offset to center the image button within the column
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            (int)((ImGui::GetColumnWidth() - b_w * g_viewport_mgr.m_scale -
                   2 * port_padding * g_viewport_mgr.m_scale) /
                  2));

        // We are using the same texture for all buttons, but ImageButton
        // uses the texture as a unique ID. Push a new ID now to resolve
        // the conflict.
        ImGui::PushID(i);
        float x = b_x + i * b_x_stride;
        ImGui::PushStyleColor(ImGuiCol_Button,
                              is_selected ? color_active : color_inactive);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            g_viewport_mgr.Scale(ImVec2(port_padding, port_padding)));
        bool activated = ImGui::ImageButton(
            "port_image_button",
            id,
            ImVec2(b_w * g_viewport_mgr.m_scale, b_h * g_viewport_mgr.m_scale),
            ImVec2(x / t_w, (b_y + b_h) / t_h),
            ImVec2((x + b_w) / t_w, b_y / t_h));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        if (activated) {
            active = i;
            m_rebinding = nullptr;
        }

        uint32_t port_color = 0xafafafff;
        bool is_hovered = ImGui::IsItemHovered();
        if (is_hovered) {
            port_color = 0xffffffff;
        } else if (is_selected || port_is_bound) {
            port_color = 0x81dc8a00;
        }

        RenderControllerPort(x, b_y, i, port_color);

        ImGui::PopID();
        ImGui::NextColumn();
    }
    ImGui::PopStyleVar(); // ItemSpacing
    ImGui::Columns(1);

    //
    // Render device driver combo
    //

    // List available device drivers
    const char *driver = bound_drivers[active];

    if (strcmp(driver, DRIVER_DUKE) == 0)
        driver = DRIVER_DUKE_DISPLAY_NAME;
    else if (strcmp(driver, DRIVER_S) == 0)
        driver = DRIVER_S_DISPLAY_NAME;

    ImGui::Columns(2, "", false);
    ImGui::SetColumnWidth(0, ImGui::GetWindowWidth()*0.25);

    ImGui::Text("Emulated Device");
    ImGui::SameLine(0, 0);
    ImGui::NextColumn();

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("###InputDrivers", driver,
                          ImGuiComboFlags_NoArrowButton)) {
        const char *available_drivers[] = { DRIVER_DUKE, DRIVER_S };
        const char *driver_display_names[] = { DRIVER_DUKE_DISPLAY_NAME,
                                               DRIVER_S_DISPLAY_NAME };
        bool is_selected = false;
        int num_drivers = sizeof(driver_display_names) / sizeof(driver_display_names[0]);
        for (int i = 0; i < num_drivers; i++) {
            const char *iter = driver_display_names[i];
            is_selected = strcmp(driver, iter) == 0;
            ImGui::PushID(iter);
            if (ImGui::Selectable(iter, is_selected)) {
                for (int j = 0; j < num_drivers; j++) {
                    if (iter == driver_display_names[j])
                        bound_drivers[active] = available_drivers[j];
                }
                xemu_input_bind(active, bound_controllers[active], 1);
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }

        ImGui::EndCombo();
    }
    DrawComboChevron();

    ImGui::NextColumn();

    //
    // Render input device combo
    //

    ImGui::Text("Input Device");
    ImGui::SameLine(0, 0);
    ImGui::NextColumn();

    // List available input devices
    const char *not_connected = "Not Connected";
    ControllerState *bound_state = xemu_input_get_bound(active);

    // Get current controller name
    const char *name;
    if (bound_state == NULL) {
        name = not_connected;
    } else {
        name = bound_state->name;
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("###InputDevices", name, ImGuiComboFlags_NoArrowButton))
    {
        // Handle "Not connected"
        bool is_selected = bound_state == NULL;
        if (ImGui::Selectable(not_connected, is_selected)) {
            xemu_input_bind(active, NULL, 1);
            bound_state = NULL;
        }
        if (is_selected) {
            ImGui::SetItemDefaultFocus();
        }

        // Handle all available input devices
        ControllerState *iter;
        QTAILQ_FOREACH(iter, &available_controllers, entry) {
            is_selected = bound_state == iter;
            ImGui::PushID(iter);
            const char *selectable_label = iter->name;
            char buf[128];
            if (iter->bound >= 0) {
                snprintf(buf, sizeof(buf), "%s (Port %d)", iter->name, iter->bound+1);
                selectable_label = buf;
            }
            if (ImGui::Selectable(selectable_label, is_selected)) {
                xemu_input_bind(active, iter, 1);

                // FIXME: We want to bind the XMU here, but we can't because we
                // just unbound it and we need to wait for Qemu to release the
                // file

                // If we previously had no controller connected, we can rebind
                // the XMU
                if (bound_state == NULL)
                    xemu_input_rebind_xmu(active);

                bound_state = iter;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }

        ImGui::EndCombo();
    }
    DrawComboChevron();

    ImGui::Columns(1);

    //
    // Add a separator between input selection and controller graphic
    //
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().WindowPadding.y / 2));

    //
    // Render controller image
    //
    bool device_selected = false;

    if (bound_state) {
        device_selected = true;
        RenderController(0, 0, 0x81dc8a00, 0x0f0f0f00, bound_state);
    } else {
        static ControllerState state{};
        RenderController(0, 0, 0x1f1f1f00, 0x0f0f0f00, &state);
    }

    ImVec2 cur = ImGui::GetCursorPos();

    ImVec2 controller_display_size;
    if (ImGui::GetContentRegionMax().x < controller_width*g_viewport_mgr.m_scale) {
        controller_display_size.x = ImGui::GetContentRegionMax().x;
        controller_display_size.y =
            controller_display_size.x * controller_height / controller_width;
    } else {
        controller_display_size =
            ImVec2(controller_width * g_viewport_mgr.m_scale,
                   controller_height * g_viewport_mgr.m_scale);
    }

    ImGui::SetCursorPosX(
        ImGui::GetCursorPosX() +
        (int)((ImGui::GetColumnWidth() - controller_display_size.x) / 2.0));

    ImGui::Image(id,
        controller_display_size,
        ImVec2(0, controller_height/t_h),
        ImVec2(controller_width/t_w, 0));
    ImVec2 pos = ImGui::GetCursorPos();
    if (!device_selected) {
        const char *msg = "Please select an available input device";
        ImVec2 dim = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPosX(cur.x + (controller_display_size.x-dim.x)/2);
        ImGui::SetCursorPosY(cur.y + (controller_display_size.y-dim.y)/2);
        ImGui::Text("%s", msg);
    }

    controller_fbo->Restore();

    ImGui::PopFont();
    ImGui::SetCursorPos(pos);

    if (bound_state) {
        ImGui::PushID(active);

        SectionTitle("Expansion Slots");
        // Begin a 2-column layout to render the expansion slots
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            g_viewport_mgr.Scale(ImVec2(0, 12)));
        ImGui::Columns(2, "mixed", false);

        xmu_fbo->Target();
        id = (ImTextureID)(intptr_t)xmu_fbo->Texture();

        const char *img_file_filters = ".img Files\0*.img\0All Files\0*.*\0";
        const char *comboLabels[2] = { "###ExpansionSlotA",
                                       "###ExpansionSlotB" };
        for (int i = 0; i < 2; i++) {
            // Display a combo box to allow the user to choose the type of
            // peripheral they want to use
            enum peripheral_type selected_type =
                bound_state->peripheral_types[i];
            const char *peripheral_type_names[2] = { "None", "Memory Unit" };
            const char *selected_peripheral_type =
                peripheral_type_names[selected_type];
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo(comboLabels[i], selected_peripheral_type,
                                  ImGuiComboFlags_NoArrowButton)) {
                // Handle all available peripheral types
                for (int j = 0; j < 2; j++) {
                    bool is_selected = selected_type == j;
                    ImGui::PushID(j);
                    const char *selectable_label = peripheral_type_names[j];

                    if (ImGui::Selectable(selectable_label, is_selected)) {
                        // Free any existing peripheral
                        if (bound_state->peripherals[i] != NULL) {
                            if (bound_state->peripheral_types[i] ==
                                PERIPHERAL_XMU) {
                                // Another peripheral was already bound.
                                // Unplugging
                                xemu_input_unbind_xmu(active, i);
                            }

                            // Free the existing state
                            g_free((void *)bound_state->peripherals[i]);
                            bound_state->peripherals[i] = NULL;
                        }

                        // Change the peripheral type to the newly selected type
                        bound_state->peripheral_types[i] =
                            (enum peripheral_type)j;

                        // Allocate state for the new peripheral
                        if (j == PERIPHERAL_XMU) {
                            bound_state->peripherals[i] =
                                g_malloc(sizeof(XmuState));
                            memset(bound_state->peripherals[i], 0,
                                   sizeof(XmuState));
                        }

                        xemu_save_peripheral_settings(
                            active, i, bound_state->peripheral_types[i], NULL);
                    }

                    if (is_selected) {
                        ImGui::SetItemDefaultFocus();
                    }

                    ImGui::PopID();
                }

                ImGui::EndCombo();
            }
            DrawComboChevron();

            // Set an X offset to center the image button within the column
            ImGui::SetCursorPosX(
                ImGui::GetCursorPosX() +
                (int)((ImGui::GetColumnWidth() -
                       xmu_w * g_viewport_mgr.m_scale -
                       2 * port_padding * g_viewport_mgr.m_scale) /
                      2));

            selected_type = bound_state->peripheral_types[i];
            if (selected_type == PERIPHERAL_XMU) {
                float x = xmu_x + i * xmu_x_stride;
                float y = xmu_y;

                XmuState *xmu = (XmuState *)bound_state->peripherals[i];
                if (xmu->filename != NULL && strlen(xmu->filename) > 0) {
                    RenderXmu(x, y, 0x81dc8a00, 0x0f0f0f00);

                } else {
                    RenderXmu(x, y, 0x1f1f1f00, 0x0f0f0f00);
                }

                ImVec2 xmu_display_size;
                if (ImGui::GetContentRegionMax().x <
                    xmu_h * g_viewport_mgr.m_scale) {
                    xmu_display_size.x = ImGui::GetContentRegionMax().x / 2;
                    xmu_display_size.y = xmu_display_size.x * xmu_h / xmu_w;
                } else {
                    xmu_display_size = ImVec2(xmu_w * g_viewport_mgr.m_scale,
                                              xmu_h * g_viewport_mgr.m_scale);
                }

                ImGui::SetCursorPosX(
                    ImGui::GetCursorPosX() +
                    (int)((ImGui::GetColumnWidth() - xmu_display_size.x) /
                          2.0));

                ImGui::Image(id, xmu_display_size, ImVec2(0.5f * i, 1),
                             ImVec2(0.5f * (i + 1), 0));

                // Button to generate a new XMU
                ImGui::PushID(i);
                if (ImGui::Button("New Image", ImVec2(250, 0))) {
                    int flags = NOC_FILE_DIALOG_SAVE |
                                NOC_FILE_DIALOG_OVERWRITE_CONFIRMATION;
                    const char *new_path = PausedFileOpen(
                        flags, img_file_filters, NULL, "xmu.img");

                    if (new_path) {
                        if (create_fatx_image(new_path, DEFAULT_XMU_SIZE)) {
                            // XMU was created successfully. Bind it
                            xemu_input_bind_xmu(active, i, new_path, false);
                        } else {
                            // Show alert message
                            char *msg = g_strdup_printf(
                                "Unable to create XMU image at %s", new_path);
                            xemu_queue_error_message(msg);
                            g_free(msg);
                        }
                    }
                }

                const char *xmu_port_path = NULL;
                if (xmu->filename == NULL)
                    xmu_port_path = g_strdup("");
                else
                    xmu_port_path = g_strdup(xmu->filename);
                if (FilePicker("Image", &xmu_port_path, img_file_filters)) {
                    if (strlen(xmu_port_path) == 0) {
                        xemu_input_unbind_xmu(active, i);
                    } else {
                        xemu_input_bind_xmu(active, i, xmu_port_path, false);
                    }
                }
                g_free((void *)xmu_port_path);

                ImGui::PopID();
            }

            ImGui::NextColumn();
        }

        xmu_fbo->Restore();

        ImGui::PopStyleVar(); // ItemSpacing
        ImGui::Columns(1);

        SectionTitle("Mapping");
        ImVec4 tc = ImGui::GetStyle().Colors[ImGuiCol_Header];
        tc.w = 0.0f;
        ImGui::PushStyleColor(ImGuiCol_Header, tc);

        if (ImGui::CollapsingHeader("Input Mapping")) {
            float p = ImGui::GetFrameHeight() * 0.3;
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(p, p));
            if (ImGui::BeginTable("input_remap_tbl", 2,
                                  ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_Borders)) {
                ImGui::TableSetupColumn("Emulated Input");
                ImGui::TableSetupColumn("Host Input");
                ImGui::TableHeadersRow();

                PopulateTableController(bound_state);

                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
        }

        if (bound_state->type == INPUT_DEVICE_SDL_GAMECONTROLLER) {
            Toggle("Enable Rumble",
                   &bound_state->controller_map->enable_rumble);
            Toggle("Invert Left X Axis",
                   &bound_state->controller_map->controller_mapping
                        .invert_axis_left_x);
            Toggle("Invert Left Y Axis",
                   &bound_state->controller_map->controller_mapping
                        .invert_axis_left_y);
            Toggle("Invert Right X Axis",
                   &bound_state->controller_map->controller_mapping
                        .invert_axis_right_x);
            Toggle("Invert Right Y Axis",
                   &bound_state->controller_map->controller_mapping
                        .invert_axis_right_y);
        }

        if (ImGui::Button("Reset to Default")) {
            xemu_input_reset_input_mapping(bound_state);
        }

        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    SectionTitle("Options");
    Toggle("Auto-bind controllers", &g_config.input.auto_bind,
           "Bind newly connected controllers to any open port");
    Toggle("Background controller input capture",
           &g_config.input.background_input_capture,
           "Capture even if window is unfocused (requires restart)");
}

void MainMenuInputView::Hide()
{
    m_rebinding = nullptr;
}

void MainMenuInputView::PopulateTableController(ControllerState *state)
{
    if (!state)
        return;

    // Must match g_keyboard_scancode_map and the controller
    // button map below.
    static constexpr const char *face_button_index_to_name_map[15] = {
        "A",
        "B",
        "X",
        "Y",
        "Back",
        "Guide",
        "Start",
        "Left Stick Button",
        "Right Stick Button",
        "White",
        "Black",
        "DPad Up",
        "DPad Down",
        "DPad Left",
        "DPad Right",
    };

    // Must match g_keyboard_scancode_map[15:]. Each axis requires
    // two keys for the positive and negative direction with the
    // exception of the triggers, which only require one each.
    static constexpr const char *keyboard_stick_index_to_name_map[10] = {
        "Left Stick Up",
        "Left Stick Left",
        "Left Stick Right",
        "Left Stick Down",
        "Left Trigger",
        "Right Stick Up",
        "Right Stick Left",
        "Right Stick Right",
        "Right Stick Down",
        "Right Trigger",
    };

    // Must match controller axis map below.
    static constexpr const char *gamepad_axis_index_to_name_map[6] = {
        "Left Stick Axis X",
        "Left Stick Axis Y",
        "Right Stick Axis X",
        "Right Stick Axis Y",
        "Left Trigger Axis",
        "Right Trigger Axis",
    };

    bool is_keyboard = state->type == INPUT_DEVICE_SDL_KEYBOARD;

    int num_axis_mappings;
    const char *const *axis_index_to_name_map;
    if (is_keyboard) {
      num_axis_mappings = std::size(keyboard_stick_index_to_name_map);
      axis_index_to_name_map = keyboard_stick_index_to_name_map;
    } else {
      num_axis_mappings = std::size(gamepad_axis_index_to_name_map);
      axis_index_to_name_map = gamepad_axis_index_to_name_map;
    }

    constexpr int num_face_buttons = std::size(face_button_index_to_name_map);
    const int table_rows = num_axis_mappings + num_face_buttons;
    for (int i = 0; i < table_rows; ++i) {
        ImGui::TableNextRow();

        // Button/Axis Name Column
        ImGui::TableSetColumnIndex(0);

        if (i < num_face_buttons) {
          ImGui::Text("%s", face_button_index_to_name_map[i]);
        } else {
          ImGui::Text("%s", axis_index_to_name_map[i - num_face_buttons]);
        }

        // Button Binding Column
        ImGui::TableSetColumnIndex(1);

        if (m_rebinding && m_rebinding->GetTableRow() == i) {
            ImGui::Text("Press a key to rebind");
            continue;
        }

        const char *remap_button_text = "Invalid";
        if (is_keyboard) {
          // g_keyboard_scancode_map includes both face buttons and axis buttons.
            int keycode = *(g_keyboard_scancode_map[i]);
            if (keycode != SDL_SCANCODE_UNKNOWN) {
                remap_button_text =
                    SDL_GetScancodeName(static_cast<SDL_Scancode>(keycode));
            }
        } else if (i < num_face_buttons) {
                int *button_map[num_face_buttons] = {
                    &state->controller_map->controller_mapping.a,
                    &state->controller_map->controller_mapping.b,
                    &state->controller_map->controller_mapping.x,
                    &state->controller_map->controller_mapping.y,
                    &state->controller_map->controller_mapping.back,
                    &state->controller_map->controller_mapping.guide,
                    &state->controller_map->controller_mapping.start,
                    &state->controller_map->controller_mapping.lstick_btn,
                    &state->controller_map->controller_mapping.rstick_btn,
                    &state->controller_map->controller_mapping.lshoulder,
                    &state->controller_map->controller_mapping.rshoulder,
                    &state->controller_map->controller_mapping.dpad_up,
                    &state->controller_map->controller_mapping.dpad_down,
                    &state->controller_map->controller_mapping.dpad_left,
                    &state->controller_map->controller_mapping.dpad_right,
                };

                int button = *(button_map[i]);
                if (button != SDL_CONTROLLER_BUTTON_INVALID) {
                    remap_button_text = SDL_GameControllerGetStringForButton(
                        static_cast<SDL_GameControllerButton>(button));
                }
        } else {
          int *axis_map[6] = {
            &state->controller_map->controller_mapping.axis_left_x,
            &state->controller_map->controller_mapping.axis_left_y,
            &state->controller_map->controller_mapping.axis_right_x,
            &state->controller_map->controller_mapping.axis_right_y,
            &state->controller_map->controller_mapping
              .axis_trigger_left,
            &state->controller_map->controller_mapping
              .axis_trigger_right,
          };
          int axis = *(axis_map[i - num_face_buttons]);
          if (axis != SDL_CONTROLLER_AXIS_INVALID) {
            remap_button_text = SDL_GameControllerGetStringForAxis(
                static_cast<SDL_GameControllerAxis>(axis));
          }
        }

        ImGui::PushID(i);
        float tw = ImGui::CalcTextSize(remap_button_text).x;
        auto &style = ImGui::GetStyle();
        float max_button_width =
          tw + g_viewport_mgr.m_scale * 2 * style.FramePadding.x;

        float min_button_width = ImGui::GetColumnWidth(1) / 2;
        float button_width = std::max(min_button_width, max_button_width);

        if (ImGui::Button(remap_button_text, ImVec2(button_width, 0))) {
          if (is_keyboard) {
            m_rebinding =
              std::make_unique<ControllerKeyboardRebindingMap>(i);
          } else {
            m_rebinding =
              std::make_unique<ControllerGamepadRebindingMap>(i,
                  state);
          }
        }
        ImGui::PopID();
    }
}

void MainMenuDisplayView::Draw()
{
    SectionTitle("Renderer");
    ChevronCombo("Backend", &g_config.display.renderer,
                 "Null\0"
                 "OpenGL\0"
#ifdef CONFIG_VULKAN
                 "Vulkan\0"
#endif
                 ,
                 "Select desired renderer implementation");
    int rendering_scale = nv2a_get_surface_scale_factor() - 1;
    if (ChevronCombo("Internal resolution scale", &rendering_scale,
                     "1x\0"
                     "2x\0"
                     "3x\0"
                     "4x\0"
                     "5x\0"
                     "6x\0"
                     "7x\0"
                     "8x\0"
                     "9x\0"
                     "10x\0",
                     "Increase surface scaling factor for higher quality")) {
        nv2a_set_surface_scale_factor(rendering_scale+1);
    }

    SectionTitle("Window");
    bool fs = xemu_is_fullscreen();
    if (Toggle("Fullscreen", &fs, "Enable fullscreen now")) {
        xemu_toggle_fullscreen();
    }
    Toggle("Fullscreen on startup",
           &g_config.display.window.fullscreen_on_startup,
           "Start xemu in fullscreen when opened");
    if (ChevronCombo("Window size", &g_config.display.window.startup_size,
                     "Last Used\0"
                     "640x480\0"
                     "720x480\0"
                     "1280x720\0"
                     "1280x800\0"
                     "1280x960\0"
                     "1920x1080\0"
                     "2560x1440\0"
                     "2560x1600\0"
                     "2560x1920\0"
                     "3840x2160\0",
                     "Select preferred startup window size")) {
    }
    Toggle("Vertical refresh sync", &g_config.display.window.vsync,
           "Sync to screen vertical refresh to reduce tearing artifacts");

    SectionTitle("Interface");
    Toggle("Show main menu bar", &g_config.display.ui.show_menubar,
           "Show main menu bar when mouse is activated");
    Toggle("Show notifications", &g_config.display.ui.show_notifications,
           "Display notifications in upper-right corner");
    Toggle("Hide mouse cursor", &g_config.display.ui.hide_cursor,
           "Hide the mouse cursor when it is not moving");

    int ui_scale_idx;
    if (g_config.display.ui.auto_scale) {
        ui_scale_idx = 0;
    } else {
        ui_scale_idx = g_config.display.ui.scale;
        if (ui_scale_idx < 0) ui_scale_idx = 0;
        else if (ui_scale_idx > 2) ui_scale_idx = 2;
    }
    if (ChevronCombo("UI scale", &ui_scale_idx,
                     "Auto\0"
                     "1x\0"
                     "2x\0",
                     "Interface element scale")) {
        if (ui_scale_idx == 0) {
            g_config.display.ui.auto_scale = true;
        } else {
            g_config.display.ui.auto_scale = false;
            g_config.display.ui.scale = ui_scale_idx;
        }
    }
    Toggle("Animations", &g_config.display.ui.use_animations,
           "Enable xemu user interface animations");
    ChevronCombo("Display mode", &g_config.display.ui.fit,
                 "Center\0"
                 "Scale\0"
                 "Stretch\0",
                 "Select how the framebuffer should fit or scale into the window");
    ChevronCombo("Aspect ratio", &g_config.display.ui.aspect_ratio,
                 "Native\0"
                 "Auto (Default)\0"
                 "4:3\0"
                 "16:9\0",
                 "Select the displayed aspect ratio");
}

void MainMenuAudioView::Draw()
{
    SectionTitle("Volume");
    char buf[32];
    snprintf(buf, sizeof(buf), "Limit output volume (%d%%)",
             (int)(g_config.audio.volume_limit * 100));
    Slider("Output volume limit", &g_config.audio.volume_limit, buf);

    SectionTitle("Quality");
    Toggle("Real-time DSP processing", &g_config.audio.use_dsp,
           "Enable improved audio accuracy (experimental)");

}

NetworkInterface::NetworkInterface(pcap_if_t *pcap_desc, char *_friendlyname)
{
    m_pcap_name = pcap_desc->name;
    m_description = pcap_desc->description ?: pcap_desc->name;
    if (_friendlyname) {
        char *tmp =
            g_strdup_printf("%s (%s)", _friendlyname, m_description.c_str());
        m_friendly_name = tmp;
        g_free((gpointer)tmp);
    } else {
        m_friendly_name = m_description;
    }
}

NetworkInterfaceManager::NetworkInterfaceManager()
{
    m_current_iface = NULL;
    m_failed_to_load_lib = false;
}

void NetworkInterfaceManager::Refresh(void)
{
    pcap_if_t *alldevs, *iter;
    char err[PCAP_ERRBUF_SIZE];

    if (xemu_net_is_enabled()) {
        return;
    }

#if defined(_WIN32)
    if (pcap_load_library()) {
        m_failed_to_load_lib = true;
        return;
    }
#endif

    m_ifaces.clear();
    m_current_iface = NULL;

    if (pcap_findalldevs(&alldevs, err)) {
        return;
    }

    for (iter=alldevs; iter != NULL; iter=iter->next) {
#if defined(_WIN32)
        char *friendly_name = get_windows_interface_friendly_name(iter->name);
        m_ifaces.emplace_back(new NetworkInterface(iter, friendly_name));
        if (friendly_name) {
            g_free((gpointer)friendly_name);
        }
#else
        m_ifaces.emplace_back(new NetworkInterface(iter));
#endif
        if (!strcmp(g_config.net.pcap.netif, iter->name)) {
            m_current_iface = m_ifaces.back().get();
        }
    }

    pcap_freealldevs(alldevs);
}

void NetworkInterfaceManager::Select(NetworkInterface &iface)
{
    m_current_iface = &iface;
    xemu_settings_set_string(&g_config.net.pcap.netif,
                             iface.m_pcap_name.c_str());
}

bool NetworkInterfaceManager::IsCurrent(NetworkInterface &iface)
{
    return &iface == m_current_iface;
}

MainMenuNetworkView::MainMenuNetworkView()
{
    should_refresh = true;
}

void MainMenuNetworkView::Draw()
{
    SectionTitle("Adapter");
    bool enabled = xemu_net_is_enabled();
    g_config.net.enable = enabled;
    if (Toggle("Enable", &g_config.net.enable,
               enabled ? "Virtual network connected (disable to change network "
                         "settings)" :
                         "Connect virtual network cable to machine")) {
        if (enabled) {
            xemu_net_disable();
        } else {
            xemu_net_enable();
        }
    }

    bool appearing = ImGui::IsWindowAppearing();
    if (enabled) ImGui::BeginDisabled();
    if (ChevronCombo(
            "Attached to", &g_config.net.backend,
            "NAT\0"
            "UDP Tunnel\0"
            "Bridged Adapter\0",
            "Controls what the virtual network controller interfaces with")) {
        appearing = true;
    }
    SectionTitle("Options");
    switch (g_config.net.backend) {
    case CONFIG_NET_BACKEND_PCAP:
        DrawPcapOptions(appearing);
        break;
    case CONFIG_NET_BACKEND_NAT:
        DrawNatOptions(appearing);
        break;
    case CONFIG_NET_BACKEND_UDP:
        DrawUdpOptions(appearing);
        break;
    default: break;
    }
    if (enabled) ImGui::EndDisabled();
}

void MainMenuNetworkView::DrawPcapOptions(bool appearing)
{
    if (iface_mgr.get() == nullptr) {
        iface_mgr.reset(new NetworkInterfaceManager());
        iface_mgr->Refresh();
    }

    if (iface_mgr->m_failed_to_load_lib) {
#if defined(_WIN32)
        const char *msg = "npcap library could not be loaded.\n"
                          "To use this backend, please install npcap.";
        ImGui::Text("%s", msg);
        ImGui::Dummy(ImVec2(0,10*g_viewport_mgr.m_scale));
        ImGui::SetCursorPosX((ImGui::GetWindowWidth()-120*g_viewport_mgr.m_scale)/2);
        if (ImGui::Button("Install npcap", ImVec2(120*g_viewport_mgr.m_scale, 0))) {
            SDL_OpenURL("https://nmap.org/npcap/");
        }
#endif
    } else {
        const char *selected_display_name =
            (iface_mgr->m_current_iface ?
                 iface_mgr->m_current_iface->m_friendly_name.c_str() :
                 g_config.net.pcap.netif);
        float combo_width = ImGui::GetColumnWidth();
        float combo_size_ratio = 0.5;
        combo_width *= combo_size_ratio;
        PrepareComboTitleDescription("Network interface",
                                     "Host network interface to bridge with",
                                     combo_size_ratio);
        ImGui::SetNextItemWidth(combo_width);
        ImGui::PushFont(g_font_mgr.m_menu_font_small);
        if (ImGui::BeginCombo("###network_iface", selected_display_name,
                              ImGuiComboFlags_NoArrowButton)) {
            if (should_refresh) {
                iface_mgr->Refresh();
                should_refresh = false;
            }

            int i = 0;
            for (auto &iface : iface_mgr->m_ifaces) {
                bool is_selected = iface_mgr->IsCurrent((*iface));
                ImGui::PushID(i++);
                if (ImGui::Selectable(iface->m_friendly_name.c_str(),
                                      is_selected)) {
                    iface_mgr->Select((*iface));
                }
                if (is_selected) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        } else {
            should_refresh = true;
        }
        ImGui::PopFont();
        DrawComboChevron();
    }
}

void MainMenuNetworkView::DrawNatOptions(bool appearing)
{
    static ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg;
    WidgetTitleDescriptionItem(
        "Port Forwarding",
        "Configure xemu to forward connections to guest on these ports");
    float p = ImGui::GetFrameHeight() * 0.3;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(p, p));
    if (ImGui::BeginTable("port_forward_tbl", 4, flags))
    {
        ImGui::TableSetupColumn("Host Port");
        ImGui::TableSetupColumn("Guest Port");
        ImGui::TableSetupColumn("Protocol");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();

        for (unsigned int row = 0; row < g_config.net.nat.forward_ports_count; row++)
        {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", g_config.net.nat.forward_ports[row].host);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d", g_config.net.nat.forward_ports[row].guest);

            ImGui::TableSetColumnIndex(2);
            switch (g_config.net.nat.forward_ports[row].protocol) {
            case CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL_TCP:
                ImGui::TextUnformatted("TCP"); break;
            case CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL_UDP:
                ImGui::TextUnformatted("UDP"); break;
            default: assert(0);
            }

            ImGui::TableSetColumnIndex(3);
            ImGui::PushID(row);
            if (ImGui::Button("Remove")) {
                remove_net_nat_forward_ports(row);
            }
            ImGui::PopID();
        }

        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        static char buf[8] = {"1234"};
        ImGui::SetNextItemWidth(ImGui::GetColumnWidth());
        ImGui::InputText("###hostport", buf, sizeof(buf));

        ImGui::TableSetColumnIndex(1);
        static char buf2[8] = {"1234"};
        ImGui::SetNextItemWidth(ImGui::GetColumnWidth());
        ImGui::InputText("###guestport", buf2, sizeof(buf2));

        ImGui::TableSetColumnIndex(2);
        static CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL protocol =
            CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL_TCP;
        assert(sizeof(protocol) >= sizeof(int));
        ImGui::SetNextItemWidth(ImGui::GetColumnWidth());
        ImGui::Combo("###protocol", &protocol, "TCP\0UDP\0");

        ImGui::TableSetColumnIndex(3);
        if (ImGui::Button("Add")) {
            int host, guest;
            if (sscanf(buf, "%d", &host) == 1 &&
                sscanf(buf2, "%d", &guest) == 1) {
                add_net_nat_forward_ports(host, guest, protocol);
            }
        }

        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

void MainMenuNetworkView::DrawUdpOptions(bool appearing)
{
    if (appearing) {
        strncpy(remote_addr, g_config.net.udp.remote_addr,
                sizeof(remote_addr) - 1);
        strncpy(local_addr, g_config.net.udp.bind_addr, sizeof(local_addr) - 1);
    }

    float size_ratio = 0.5;
    float width = ImGui::GetColumnWidth() * size_ratio;
    ImGui::PushFont(g_font_mgr.m_menu_font_small);
    PrepareComboTitleDescription(
        "Remote Address",
        "Destination addr:port to forward packets to (1.2.3.4:9968)",
        size_ratio);
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputText("###remote_host", remote_addr, sizeof(remote_addr))) {
        xemu_settings_set_string(&g_config.net.udp.remote_addr, remote_addr);
    }
    PrepareComboTitleDescription(
        "Bind Address", "Local addr:port to receive packets on (0.0.0.0:9968)",
        size_ratio);
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputText("###local_host", local_addr, sizeof(local_addr))) {
        xemu_settings_set_string(&g_config.net.udp.bind_addr, local_addr);
    }
    ImGui::PopFont();
}

MainMenuSnapshotsView::MainMenuSnapshotsView() : MainMenuTabView()
{
    xemu_snapshots_mark_dirty();

    m_search_regex = NULL;
    m_current_title_id = 0;
}

MainMenuSnapshotsView::~MainMenuSnapshotsView()
{
    g_free(m_search_regex);
}

bool MainMenuSnapshotsView::BigSnapshotButton(QEMUSnapshotInfo *snapshot,
                                              XemuSnapshotData *data,
                                              int current_snapshot_binding)
{
    ImGuiStyle &style = ImGui::GetStyle();
    ImDrawList *draw_list = ImGui::GetWindowDrawList();

    ImGui::PushFont(g_font_mgr.m_menu_font_small);
    ImVec2 ts_sub = ImGui::CalcTextSize(snapshot->name);
    ImGui::PopFont();

    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        g_viewport_mgr.Scale(ImVec2(5, 5)));

    ImGui::PushFont(g_font_mgr.m_menu_font_medium);

    ImVec2 ts_title = ImGui::CalcTextSize(snapshot->name);
    ImVec2 thumbnail_size = g_viewport_mgr.Scale(
        ImVec2(XEMU_SNAPSHOT_THUMBNAIL_WIDTH, XEMU_SNAPSHOT_THUMBNAIL_HEIGHT));
    ImVec2 thumbnail_pos(style.FramePadding.x, style.FramePadding.y);
    ImVec2 name_pos(thumbnail_pos.x + thumbnail_size.x +
                        style.FramePadding.x * 2,
                    thumbnail_pos.y);
    ImVec2 title_pos(name_pos.x,
                     name_pos.y + ts_title.y + style.FramePadding.x);
    ImVec2 date_pos(name_pos.x,
                    title_pos.y + ts_title.y + style.FramePadding.x);
    ImVec2 binding_pos(name_pos.x,
                       date_pos.y + ts_title.y + style.FramePadding.x);
    ImVec2 button_size(-FLT_MIN,
                       fmax(thumbnail_size.y + style.FramePadding.y * 2,
                            ts_title.y + ts_sub.y + style.FramePadding.y * 3));

    bool load = ImGui::Button("###button", button_size);

    ImGui::PopFont();

    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    draw_list->PushClipRect(p0, p1, true);

    // Snapshot thumbnail
    GLuint thumbnail = data->gl_thumbnail ? data->gl_thumbnail : g_icon_tex;
    int thumbnail_width, thumbnail_height;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, thumbnail);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,
                             &thumbnail_width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,
                             &thumbnail_height);

    // Draw black background behind thumbnail
    ImVec2 thumbnail_min(p0.x + thumbnail_pos.x, p0.y + thumbnail_pos.y);
    ImVec2 thumbnail_max(thumbnail_min.x + thumbnail_size.x,
                         thumbnail_min.y + thumbnail_size.y);
    draw_list->AddRectFilled(thumbnail_min, thumbnail_max, IM_COL32_BLACK);

    // Draw centered thumbnail image
    int scaled_width, scaled_height;
    ScaleDimensions(thumbnail_width, thumbnail_height, thumbnail_size.x,
                    thumbnail_size.y, &scaled_width, &scaled_height);
    ImVec2 img_min =
        ImVec2(thumbnail_min.x + (thumbnail_size.x - scaled_width) / 2,
               thumbnail_min.y + (thumbnail_size.y - scaled_height) / 2);
    ImVec2 img_max =
        ImVec2(img_min.x + scaled_width, img_min.y + scaled_height);
    draw_list->AddImage((ImTextureID)(uint64_t)thumbnail, img_min, img_max);

    // Snapshot title
    ImGui::PushFont(g_font_mgr.m_menu_font_medium);
    draw_list->AddText(ImVec2(p0.x + name_pos.x, p0.y + name_pos.y),
                       IM_COL32(255, 255, 255, 255), snapshot->name);
    ImGui::PopFont();

    // Snapshot XBE title name
    ImGui::PushFont(g_font_mgr.m_menu_font_small);
    const char *title_name = data->xbe_title_name ? data->xbe_title_name :
                                                    "(Unknown XBE Title Name)";
    draw_list->AddText(ImVec2(p0.x + title_pos.x, p0.y + title_pos.y),
                       IM_COL32(255, 255, 255, 200), title_name);

    // Snapshot date
    g_autoptr(GDateTime) date =
        g_date_time_new_from_unix_local(snapshot->date_sec);
    char *date_buf = g_date_time_format(date, "%Y-%m-%d %H:%M:%S");
    draw_list->AddText(ImVec2(p0.x + date_pos.x, p0.y + date_pos.y),
                       IM_COL32(255, 255, 255, 200), date_buf);
    g_free(date_buf);

    // Snapshot keyboard binding
    if (current_snapshot_binding != -1) {
        char *binding_text =
            g_strdup_printf("Bound to F%d", current_snapshot_binding + 5);
        draw_list->AddText(ImVec2(p0.x + binding_pos.x, p0.y + binding_pos.y),
                           IM_COL32(255, 255, 255, 200), binding_text);
        g_free(binding_text);
    }

    ImGui::PopFont();
    draw_list->PopClipRect();
    ImGui::PopStyleVar(2);

    return load;
}

void MainMenuSnapshotsView::ClearSearch()
{
    m_search_buf.clear();

    if (m_search_regex) {
        g_free(m_search_regex);
        m_search_regex = NULL;
    }
}

int MainMenuSnapshotsView::OnSearchTextUpdate(ImGuiInputTextCallbackData *data)
{
    GError *gerr = NULL;
    MainMenuSnapshotsView *win = (MainMenuSnapshotsView *)data->UserData;

    if (win->m_search_regex) {
        g_free(win->m_search_regex);
        win->m_search_regex = NULL;
    }

    if (data->BufTextLen == 0) {
        return 0;
    }

    char *buf = g_strdup_printf("(.*)%s(.*)", data->Buf);
    win->m_search_regex =
        g_regex_new(buf, (GRegexCompileFlags)0, (GRegexMatchFlags)0, &gerr);
    g_free(buf);
    if (gerr) {
        win->m_search_regex = NULL;
        return 1;
    }

    return 0;
}

void MainMenuSnapshotsView::Draw()
{
    g_snapshot_mgr.Refresh();

    SectionTitle("Snapshots");
    Toggle("Filter by current title",
           &g_config.general.snapshots.filter_current_game,
           "Only display snapshots created while running the currently running "
           "XBE");

    if (g_config.general.snapshots.filter_current_game) {
        struct xbe *xbe = xemu_get_xbe_info();
        if (xbe && xbe->cert) {
            if (xbe->cert->m_titleid != m_current_title_id) {
                char *title_name = g_utf16_to_utf8(xbe->cert->m_title_name, 40,
                                                   NULL, NULL, NULL);
                if (title_name) {
                    m_current_title_name = title_name;
                    g_free(title_name);
                } else {
                    m_current_title_name.clear();
                }

                m_current_title_id = xbe->cert->m_titleid;
            }
        } else {
            m_current_title_name.clear();
            m_current_title_id = 0;
        }
    }

    ImGui::SetNextItemWidth(ImGui::GetColumnWidth() * 0.8);
    ImGui::PushFont(g_font_mgr.m_menu_font_small);
    ImGui::InputTextWithHint("##search", "Search or name new snapshot...",
                             &m_search_buf, ImGuiInputTextFlags_CallbackEdit,
                             &OnSearchTextUpdate, this);

    bool snapshot_with_create_name_exists = false;
    for (int i = 0; i < g_snapshot_mgr.m_snapshots_len; ++i) {
        if (g_strcmp0(m_search_buf.c_str(),
                      g_snapshot_mgr.m_snapshots[i].name) == 0) {
            snapshot_with_create_name_exists = true;
            break;
        }
    }

    ImGui::SameLine();
    if (snapshot_with_create_name_exists) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8, 0, 0, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 0, 0, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 0, 0, 1));
    }
    if (ImGui::Button(snapshot_with_create_name_exists ? "Replace" : "Create",
                      ImVec2(-FLT_MIN, 0))) {
        xemu_snapshots_save(m_search_buf.empty() ? NULL : m_search_buf.c_str(),
                            NULL);
        ClearSearch();
    }
    if (snapshot_with_create_name_exists) {
        ImGui::PopStyleColor(3);
    }

    if (snapshot_with_create_name_exists && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A snapshot with the name \"%s\" already exists. "
                          "This button will overwrite the existing snapshot.",
                          m_search_buf.c_str());
    }
    ImGui::PopFont();

    bool at_least_one_snapshot_displayed = false;

    for (int i = g_snapshot_mgr.m_snapshots_len - 1; i >= 0; i--) {
        if (g_config.general.snapshots.filter_current_game &&
            g_snapshot_mgr.m_extra_data[i].xbe_title_name &&
            m_current_title_name.size() &&
            strcmp(m_current_title_name.c_str(),
                   g_snapshot_mgr.m_extra_data[i].xbe_title_name)) {
            continue;
        }

        if (m_search_regex) {
            GMatchInfo *match;
            bool keep_entry = false;

            g_regex_match(m_search_regex, g_snapshot_mgr.m_snapshots[i].name,
                          (GRegexMatchFlags)0, &match);
            keep_entry |= g_match_info_matches(match);
            g_match_info_free(match);

            if (g_snapshot_mgr.m_extra_data[i].xbe_title_name) {
                g_regex_match(m_search_regex,
                              g_snapshot_mgr.m_extra_data[i].xbe_title_name,
                              (GRegexMatchFlags)0, &match);
                keep_entry |= g_match_info_matches(match);
                g_free(match);
            }

            if (!keep_entry) {
                continue;
            }
        }

        QEMUSnapshotInfo *snapshot = &g_snapshot_mgr.m_snapshots[i];
        XemuSnapshotData *data = &g_snapshot_mgr.m_extra_data[i];

        int current_snapshot_binding = -1;
        for (int j = 0; j < 4; ++j) {
            if (g_strcmp0(*(g_snapshot_shortcut_index_key_map[j]),
                          snapshot->name) == 0) {
                assert(current_snapshot_binding == -1);
                current_snapshot_binding = j;
            }
        }

        ImGui::PushID(i);

        ImVec2 pos = ImGui::GetCursorScreenPos();
        bool load = BigSnapshotButton(snapshot, data, current_snapshot_binding);

        // FIXME: Provide context menu control annotation
        if (ImGui::IsItemHovered() &&
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft)) {
            ImGui::SetNextWindowPos(pos);
            ImGui::OpenPopup("Snapshot Options");
        }

        DrawSnapshotContextMenu(snapshot, data, current_snapshot_binding);

        ImGui::PopID();

        if (load) {
            ActionLoadSnapshotChecked(snapshot->name);
        }

        at_least_one_snapshot_displayed = true;
    }

    if (!at_least_one_snapshot_displayed) {
        ImGui::Dummy(g_viewport_mgr.Scale(ImVec2(0, 16)));
        const char *msg;
        if (g_snapshot_mgr.m_snapshots_len) {
            if (!m_search_buf.empty()) {
                msg = "Press Create to create new snapshot";
            } else {
                msg = "No snapshots match filter criteria";
            }
        } else {
            msg = "No snapshots to display";
        }
        ImVec2 dim = ImGui::CalcTextSize(msg);
        ImVec2 cur = ImGui::GetCursorPos();
        ImGui::SetCursorPosX(cur.x + (ImGui::GetColumnWidth() - dim.x) / 2);
        ImGui::TextColored(ImVec4(0.94f, 0.94f, 0.94f, 0.70f), "%s", msg);
    }
}

void MainMenuSnapshotsView::DrawSnapshotContextMenu(
    QEMUSnapshotInfo *snapshot, XemuSnapshotData *data,
    int current_snapshot_binding)
{
    if (!ImGui::BeginPopupContextItem("Snapshot Options")) {
        return;
    }

    if (ImGui::MenuItem("Load")) {
        ActionLoadSnapshotChecked(snapshot->name);
    }

    if (ImGui::BeginMenu("Keybinding")) {
        for (int i = 0; i < 4; ++i) {
            char *item_name = g_strdup_printf("Bind to F%d", i + 5);

            if (ImGui::MenuItem(item_name)) {
                if (current_snapshot_binding >= 0) {
                    xemu_settings_set_string(g_snapshot_shortcut_index_key_map
                                                 [current_snapshot_binding],
                                             "");
                }
                xemu_settings_set_string(g_snapshot_shortcut_index_key_map[i],
                                         snapshot->name);
                current_snapshot_binding = i;

                ImGui::CloseCurrentPopup();
            }

            g_free(item_name);
        }

        if (current_snapshot_binding >= 0) {
            if (ImGui::MenuItem("Unbind")) {
                xemu_settings_set_string(
                    g_snapshot_shortcut_index_key_map[current_snapshot_binding],
                    "");
                current_snapshot_binding = -1;
            }
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();

    Error *err = NULL;

    if (ImGui::MenuItem("Replace")) {
        xemu_snapshots_save(snapshot->name, &err);
    }

    if (ImGui::MenuItem("Delete")) {
        xemu_snapshots_delete(snapshot->name, &err);
    }

    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
    }

    ImGui::EndPopup();
}

MainMenuSystemView::MainMenuSystemView() : m_dirty(false)
{
}

void MainMenuSystemView::Draw()
{
    const char *rom_file_filters =
        ".bin Files\0*.bin\0.rom Files\0*.rom\0All Files\0*.*\0";
    const char *qcow_file_filters = ".qcow2 Files\0*.qcow2\0All Files\0*.*\0";

    if (m_dirty) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1),
                           "Application restart required to apply settings");
    }

    if ((int)g_config.sys.avpack == CONFIG_SYS_AVPACK_NONE) {
        ImGui::TextColored(ImVec4(1,0,0,1), "Setting AV Pack to NONE disables video output.");
    }

    SectionTitle("System Configuration");

    if (ChevronCombo(
            "System Memory", &g_config.sys.mem_limit,
            "64 MiB (Default)\0"
            "128 MiB\0",
            "Increase to 128 MiB for debug or homebrew applications")) {
        m_dirty = true;
    }

    if (ChevronCombo(
            "AV Pack", &g_config.sys.avpack,
            "SCART\0HDTV (Default)\0VGA\0RFU\0S-Video\0Composite\0None\0",
            "Select the attached AV pack")) {
        m_dirty = true;
    }

    SectionTitle("Files");
    if (FilePicker("MCPX Boot ROM", &g_config.sys.files.bootrom_path,
                   rom_file_filters)) {
        m_dirty = true;
        g_main_menu.UpdateAboutViewConfigInfo();
    }
    if (FilePicker("Flash ROM (BIOS)", &g_config.sys.files.flashrom_path,
                   rom_file_filters)) {
        m_dirty = true;
        g_main_menu.UpdateAboutViewConfigInfo();
    }
    if (FilePicker("Hard Disk", &g_config.sys.files.hdd_path,
                   qcow_file_filters)) {
        m_dirty = true;
    }
    if (FilePicker("EEPROM", &g_config.sys.files.eeprom_path,
                   rom_file_filters)) {
        m_dirty = true;
    }
}

MainMenuAboutView::MainMenuAboutView() : m_config_info_text{ NULL }
{
}

void MainMenuAboutView::UpdateConfigInfoText()
{
    if (m_config_info_text) {
        g_free(m_config_info_text);
    }

    gchar *bootrom_checksum =
        GetFileMD5Checksum(g_config.sys.files.bootrom_path);
    if (!bootrom_checksum) {
        bootrom_checksum = g_strdup("None");
    }

    gchar *flash_rom_checksum =
        GetFileMD5Checksum(g_config.sys.files.flashrom_path);
    if (!flash_rom_checksum) {
        flash_rom_checksum = g_strdup("None");
    }

    m_config_info_text = g_strdup_printf("MCPX Boot ROM MD5 Hash:        %s\n"
                                         "Flash ROM (BIOS) MD5 Hash:     %s",
                                         bootrom_checksum, flash_rom_checksum);
    g_free(bootrom_checksum);
    g_free(flash_rom_checksum);
}

void MainMenuAboutView::Draw()
{
    static const char *build_info_text = NULL;
    if (build_info_text == NULL) {
        build_info_text =
            g_strdup_printf("Version:      %s\n"
                            "Commit:       %s\n"
                            "Date:         %s",
                            xemu_version, xemu_commit, xemu_date);
    }

    static const char *sys_info_text = NULL;
    if (sys_info_text == NULL) {
        const char *gl_shader_version =
            (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
        const char *gl_version = (const char *)glGetString(GL_VERSION);
        const char *gl_renderer = (const char *)glGetString(GL_RENDERER);
        const char *gl_vendor = (const char *)glGetString(GL_VENDOR);
        sys_info_text = g_strdup_printf(
            "CPU:          %s\nOS Platform:  %s\nOS Version:   "
            "%s\nManufacturer: %s\n"
            "GPU Model:    %s\nDriver:       %s\nShader:       %s",
            xemu_get_cpu_info(), xemu_get_os_platform(), xemu_get_os_info(),
            gl_vendor, gl_renderer, gl_version, gl_shader_version);
    }

    if (m_config_info_text == NULL) {
        UpdateConfigInfoText();
    }

    Logo();

    SectionTitle("Build Information");
    ImGui::PushFont(g_font_mgr.m_fixed_width_font);
    ImGui::InputTextMultiline("##build_info", (char *)build_info_text,
                              strlen(build_info_text) + 1,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::PopFont();

    SectionTitle("System Information");
    ImGui::PushFont(g_font_mgr.m_fixed_width_font);
    ImGui::InputTextMultiline("###systeminformation", (char *)sys_info_text,
                              strlen(sys_info_text) + 1,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::PopFont();

    SectionTitle("Config Information");
    ImGui::PushFont(g_font_mgr.m_fixed_width_font);
    ImGui::InputTextMultiline("##config_info", (char *)m_config_info_text,
                              strlen(build_info_text) + 1,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::PopFont();

    SectionTitle("Community");

    ImGui::Text("Visit");
    ImGui::SameLine();
    if (ImGui::SmallButton("https://xemu.app")) {
        SDL_OpenURL("https://xemu.app");
    }
    ImGui::SameLine();
    ImGui::Text("for more information");
}

MainMenuTabButton::MainMenuTabButton(std::string text, std::string icon)
    : m_icon(icon), m_text(text)
{
}

bool MainMenuTabButton::Draw(bool selected)
{
    ImGuiStyle &style = ImGui::GetStyle();

    ImU32 col = selected ?
                    ImGui::GetColorU32(style.Colors[ImGuiCol_ButtonHovered]) :
                    IM_COL32(0, 0, 0, 0);

    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          selected ? col : IM_COL32(32, 32, 32, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          selected ? col : IM_COL32(32, 32, 32, 255));
    int p = ImGui::GetTextLineHeight() * 0.5;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(p, p));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, 0.5));
    ImGui::PushFont(g_font_mgr.m_menu_font);

    ImVec2 button_size = ImVec2(-FLT_MIN, 0);
    auto text = string_format("%s %s", m_icon.c_str(), m_text.c_str());
    ImGui::PushID(this);
    bool status = ImGui::Button(text.c_str(), button_size);
    ImGui::PopID();
    ImGui::PopFont();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    return status;
}

MainMenuScene::MainMenuScene()
    : m_animation(0.12, 0.12), m_general_button("General", ICON_FA_GEARS),
      m_input_button("Input", ICON_FA_GAMEPAD),
      m_display_button("Display", ICON_FA_TV),
      m_audio_button("Audio", ICON_FA_VOLUME_HIGH),
      m_network_button("Network", ICON_FA_NETWORK_WIRED),
      m_snapshots_button("Snapshots", ICON_FA_CLOCK_ROTATE_LEFT),
      m_patches_button("Patches", ICON_FA_GEARS),
      m_system_button("System", ICON_FA_MICROCHIP),
      m_about_button("About", ICON_FA_CIRCLE_INFO)
{
    m_had_focus_last_frame = false;
    m_focus_view = false;
    m_tabs.push_back(&m_general_button);
    m_tabs.push_back(&m_input_button);
    m_tabs.push_back(&m_display_button);
    m_tabs.push_back(&m_audio_button);
    m_tabs.push_back(&m_network_button);
    m_tabs.push_back(&m_snapshots_button);
    m_tabs.push_back(&m_patches_button);
    m_tabs.push_back(&m_system_button);
    m_tabs.push_back(&m_about_button);

    m_views.push_back(&m_general_view);
    m_views.push_back(&m_input_view);
    m_views.push_back(&m_display_view);
    m_views.push_back(&m_audio_view);
    m_views.push_back(&m_network_view);
    m_views.push_back(&m_snapshots_view);
    m_views.push_back(&m_patches_view);
    m_views.push_back(&m_system_view);
    m_views.push_back(&m_about_view);

    m_current_view_index = 0;
    m_next_view_index = m_current_view_index;
}

void MainMenuScene::ShowSettings()
{
    SetNextViewIndexWithFocus(g_config.general.last_viewed_menu_index);
}

void MainMenuScene::ShowSnapshots()
{
    SetNextViewIndexWithFocus(5);
}

void MainMenuScene::ShowPatches()
{
    SetNextViewIndexWithFocus(6);
}

void MainMenuScene::ShowSystem()
{
    SetNextViewIndexWithFocus(7);
}

void MainMenuScene::ShowAbout()
{
    SetNextViewIndexWithFocus(8);
}

void MainMenuScene::SetNextViewIndexWithFocus(int i)
{
    m_focus_view = true;
    SetNextViewIndex(i);

    if (!g_scene_mgr.IsDisplayingScene()) {
        g_scene_mgr.PushScene(*this);
    }
}

void MainMenuScene::Show()
{
    m_background.Show();
    m_nav_control_view.Show();
    m_animation.EaseIn();
}

void MainMenuScene::Hide()
{
    m_views[m_current_view_index]->Hide();
    m_background.Hide();
    m_nav_control_view.Hide();
    m_animation.EaseOut();
}

bool MainMenuScene::IsAnimating()
{
    return m_animation.IsAnimating();
}

void MainMenuScene::SetNextViewIndex(int i)
{
    m_views[m_current_view_index]->Hide();
    m_next_view_index = i % m_tabs.size();
    g_config.general.last_viewed_menu_index = i;
}

void MainMenuScene::HandleInput()
{
    bool nofocus = !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow);
    bool focus = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows |
                                        ImGuiFocusedFlags_NoPopupHierarchy);

    // XXX: Ensure we have focus for two frames. If a user cancels a popup
    // window, we do not want to cancel main
    //      window as well.
    if (nofocus || (focus && m_had_focus_last_frame &&
                    (ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight) ||
                     ImGui::IsKeyDown(ImGuiKey_Escape)))) {
        Hide();
        return;
    }

    if (focus && m_had_focus_last_frame) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1)) {
            SetNextViewIndex((m_current_view_index + m_tabs.size() - 1) %
                             m_tabs.size());
        }

        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1)) {
            SetNextViewIndex((m_current_view_index + 1) % m_tabs.size());
        }
    }

    m_had_focus_last_frame = focus;
}

void MainMenuScene::UpdateAboutViewConfigInfo()
{
    m_about_view.UpdateConfigInfoText();
}

bool MainMenuScene::ConsumeRebindEvent(SDL_Event *event)
{
    return m_input_view.ConsumeRebindEvent(event);
}

bool MainMenuScene::IsInputRebinding()
{
    return m_input_view.IsInputRebinding();
}

bool MainMenuScene::Draw()
{
    m_animation.Step();
    m_background.Draw();
    m_nav_control_view.Draw();

    ImGuiIO &io = ImGui::GetIO();
    float t = m_animation.GetSinInterpolatedValue();
    float window_alpha = t;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, window_alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);

    ImVec4 extents = g_viewport_mgr.GetExtents();
    ImVec2 window_pos = ImVec2(io.DisplaySize.x / 2, extents.y);
    ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, ImVec2(0.5, 0));

    ImVec2 max_size = g_viewport_mgr.Scale(ImVec2(800, 0));
    float x = fmin(io.DisplaySize.x - extents.x - extents.z, max_size.x);
    float y = io.DisplaySize.y - extents.y - extents.w;
    ImGui::SetNextWindowSize(ImVec2(x, y));

    if (ImGui::Begin("###MainWindow", NULL,
                     ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_NoSavedSettings)) {
        //
        // Nav menu
        //

        float width = ImGui::GetWindowWidth();
        float nav_width = width * 0.3;
        float content_width = width - nav_width;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(26, 26, 26, 255));

        ImGui::BeginChild("###MainWindowNav", ImVec2(nav_width, -1), true,
                          ImGuiWindowFlags_NavFlattened);

        bool move_focus_to_tab = false;
        if (m_current_view_index != m_next_view_index) {
            m_current_view_index = m_next_view_index;
            if (!m_focus_view) {
                move_focus_to_tab = true;
            }
        }

        int i = 0;
        for (auto &button : m_tabs) {
            if (move_focus_to_tab && i == m_current_view_index) {
                ImGui::SetKeyboardFocusHere();
                move_focus_to_tab = false;
            }
            if (button->Draw(i == m_current_view_index)) {
                SetNextViewIndex(i);
            }
            if (i == m_current_view_index) {
                ImGui::SetItemDefaultFocus();
            }
            i++;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        //
        // Content
        //
        ImGui::SameLine();
        int s = ImGui::GetTextLineHeight() * 0.75;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(s, s));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(s, s));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,
                            6 * g_viewport_mgr.m_scale);

        ImGui::PushID(m_current_view_index);
        ImGui::BeginChild("###MainWindowContent", ImVec2(content_width, -1),
                          true,
                          ImGuiWindowFlags_AlwaysUseWindowPadding |
                              ImGuiWindowFlags_NavFlattened);

        if (!g_input_mgr.IsNavigatingWithController()) {
            // Close button
            ImGui::PushFont(g_font_mgr.m_menu_font);
            ImGuiStyle &style = ImGui::GetStyle();
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 128));
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32_BLACK_TRANS);
            ImVec2 pos = ImGui::GetCursorPos();
            ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x -
                                 style.FramePadding.x * 2.0f -
                                 ImGui::GetTextLineHeight());
            if (ImGui::Button(ICON_FA_XMARK)) {
                Hide();
            }
            ImGui::SetCursorPos(pos);
            ImGui::PopStyleColor(2);
            ImGui::PopFont();
        }

        ImGui::PushFont(g_font_mgr.m_default_font);
        if (m_focus_view) {
            ImGui::SetKeyboardFocusHere();
            m_focus_view = false;
        }
        m_views[m_current_view_index]->Draw();

        ImGui::PopFont();
        ImGui::EndChild();
        ImGui::PopID();
        ImGui::PopStyleVar(3);

        HandleInput();
    }
    ImGui::End();
    ImGui::PopStyleVar(5);

    return !m_animation.IsComplete();
}

// MainMenuPatchesView implementation
void MainMenuPatchesView::Draw()
{
    SectionTitle("Memory Patches");
    
    // Check if patches database is loaded
    int game_count = xemu_patches_get_game_count();
    XemuGamePatches *games = xemu_patches_get_games();
    
    if (!games || game_count == 0) {
        ImGui::TextColored(ImVec4(0.94f, 0.94f, 0.94f, 0.70f),
                          "No patches in database. Click 'Add Game' to get started.");
        ImGui::Dummy(g_viewport_mgr.Scale(ImVec2(0, 16)));
        
        // Disable Add Game button when no disc is present
        bool disc_present = g_disc_present;
        
        if (!disc_present) {
            ImGui::BeginDisabled();
        }
        
        if (ImGui::Button("Add Game", ImVec2(120, 0))) {
            m_show_add_game_dialog = true;
        }
        
        if (!disc_present) {
            ImGui::EndDisabled();
        }
    } else {
        // Display games table
        DrawGamesTable();
    }
    
    // Show dialogs
    if (m_show_add_game_dialog) {
        DrawAddGameDialog();
    }
    
    if (m_show_add_patch_dialog) {
        DrawAddPatchDialog();
    }
    

    

    
    if (m_show_edit_patch_dialog) {
        DrawEditPatchDialog();
    }
    
    // Show game details window
    DrawGameDetailsWindow();
}

void MainMenuPatchesView::DrawGameSection(XemuGamePatches *game, int game_index)
{
    SectionTitle(game->game_title);
    
    // Game info
    ImGui::PushFont(g_font_mgr.m_menu_font_small);
    ImGui::Text("Region: %s | Title ID: %s | Version: %s", 
               game->region, game->title_id, game->version);
    ImGui::PopFont();
    
    // Game controls
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, g_viewport_mgr.Scale(ImVec2(4, 4)));
    
    // Enable/disable toggle at the bottom
    ImGui::Separator();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, g_viewport_mgr.Scale(ImVec2(4, 4)));
    
    // Buttons at the bottom
    ImGui::Dummy(ImVec2(0, 8)); // Spacing
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));
    
    if (ImGui::Button("Add Patch", ImVec2(100, 0))) {
        // Implementation: show add patch dialog
        // For now, just show a simple input dialog
        ImGui::OpenPopup("Add Patch Dialog");
    }
    
    ImGui::SameLine();
    
    // Remove game button (styled as destructive)
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0, 0, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 0, 0, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 0, 0, 1));
    if (ImGui::Button("Remove Game", ImVec2(120, 0))) {
        if (ImGui::GetIO().KeyShift) {
            // Require Shift+Click for delete
            if (xemu_patches_remove_game(game_index)) {
                xemu_patches_on_ui_database_changed();
            }
        }
    }
    ImGui::PopStyleColor(3);
    
    ImGui::PopStyleVar(2); // item spacing
    
    // Add Patch Dialog (simplified for now)
    if (ImGui::BeginPopup("Add Patch Dialog")) {
        ImGui::Text("Add New Patch");
        ImGui::Separator();
        
        static char patch_title[128] = "";
        static char patch_address[32] = "";
        static char patch_value[32] = "";
        
        ImGui::InputText("Patch Title", patch_title, sizeof(patch_title));
        ImGui::InputText("Memory Address (hex)", patch_address, sizeof(patch_address));
        ImGui::InputText("Value (hex, variable length)", patch_value, sizeof(patch_value));
        ImGui::Text("Format: hexadecimal values (e.g., 0x1234, 388EE3, 388EE33F68000096)");
        
        if (ImGui::Button("Add Patch", ImVec2(80, 0))) {
            if (strlen(patch_title) > 0 && strlen(patch_address) > 0 && strlen(patch_value) > 0) {
                // Parse hex address and create address:value pair string
                char* endptr;
                uint32_t address = strtoul(patch_address, &endptr, 16);
                
                if (endptr != patch_address) {
                    // Create address:value pair string - preserve the original value format
                    char address_value[256];
                    snprintf(address_value, sizeof(address_value), "0x%08X:%s", address, patch_value);
                    
                    if (xemu_patches_add_patch(game_index, patch_title, "Uncategorized", "Unknown", "", address_value, false)) {
                        xemu_patches_on_ui_database_changed();
                        ImGui::CloseCurrentPopup();
                    }
                }
                
                // Clear inputs
                memset(patch_title, 0, sizeof(patch_title));
                memset(patch_address, 0, sizeof(patch_address));
                memset(patch_value, 0, sizeof(patch_value));
            }
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0))) {
            ImGui::CloseCurrentPopup();
            // Clear inputs
            memset(patch_title, 0, sizeof(patch_title));
            memset(patch_address, 0, sizeof(patch_address));
            memset(patch_value, 0, sizeof(patch_value));
        }
        
        ImGui::EndPopup();
    }
    
    ImGui::PopStyleVar();
    
    // Draw patches for this game first
    if (game->patch_count > 0) {
        ImGui::Separator();
        
        ImGui::Text("Patches (%d):", game->patch_count);
        ImGui::Dummy(ImVec2(0, 4));
        
        // Show all patches for this game
        for (int i = 0; i < game->patch_count; i++) {
            XemuMemoryPatch *patch = &game->patches[i];
            
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 2));
            
            // Patch title and enable toggle
            bool patch_enabled = patch->enabled;
            ImGui::Checkbox(("##patch_enable_" + std::to_string(i)).c_str(), &patch_enabled);
            if (patch_enabled != patch->enabled) {
                xemu_patches_set_patch_enabled(game_index, i, patch_enabled);
                xemu_patches_on_ui_database_changed();
            }
            
            ImGui::SameLine();
            ImGui::Text("%s", patch->name ? patch->name : "Unnamed Patch");
            
            // Patch actions
            ImGui::SameLine();
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
            if (ImGui::Button(("Edit##patch_edit_" + std::to_string(i)).c_str(), ImVec2(60, 20))) {
                // Show edit patch dialog
                // Implementation: open edit dialog for this patch
            }
            
            ImGui::SameLine();
            if (ImGui::Button(("Delete##patch_delete_" + std::to_string(i)).c_str(), ImVec2(60, 20))) {
                if (ImGui::GetIO().KeyShift) {
                    // Require Shift+Click for delete
                    if (xemu_patches_remove_patch(game_index, i)) {
                        xemu_patches_on_ui_database_changed();
                        break; // Exit loop as array has changed
                    }
                }
            }
            
            ImGui::PopStyleVar(); // item spacing
            ImGui::PopStyleVar(); // item spacing (patch actions)
            
            // Patch details
            ImGui::PushFont(g_font_mgr.m_menu_font_small);
            ImGui::Text("Address: 0x%08X", patch->address_values[0].address);
            // Display value as hex string
            ImGui::Text("Value: ");
            ImGui::SameLine();
            char value_str[64] = "";
            for (int b = 0; b < patch->address_values[0].value_length; b++) {
                snprintf(value_str + b * 2, sizeof(value_str) - b * 2, "%02X", patch->address_values[0].value_data[b]);
            }
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "0x%s", value_str);
            ImGui::PopFont();
            
            ImGui::Separator();
        }
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No patches defined for this game.");
    }
    
    ImGui::Dummy(g_viewport_mgr.Scale(ImVec2(0, 16)));
}

void MainMenuPatchesView::DrawPatchSection(XemuGamePatches *game, int game_index)
{
    ImGui::Text("Memory Patches:");
    
    for (int j = 0; j < game->patch_count; j++) {
        XemuMemoryPatch *patch = &game->patches[j];
        
        ImGui::PushID(j);
        
        // Right-click context menu for patch
        if (ImGui::BeginPopupContextItem("PatchContextMenu")) {
            if (ImGui::MenuItem("Edit Patch")) {
                // Set editing state for this patch

                m_selected_game_index = game_index;
                m_editing_patch_index = j;
                m_edit_error_message[0] = '\0'; // Clear any previous errors
            }
            if (ImGui::MenuItem("Delete Patch")) {
                // Store the patch index for deletion
                m_patch_to_delete = j;
                m_selected_game_index = game_index;
                // Open confirmation dialog
                ImGui::OpenPopup("Confirm Delete Patch");
                ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            }
            ImGui::EndPopup();
        }
        
        // Patch enable/disable
        bool enabled = patch->enabled;
        if (ImGui::Checkbox(patch->name ? patch->name : "Unnamed Patch", &enabled)) {
            xemu_patches_set_patch_enabled(game_index, j, enabled);
            xemu_patches_on_ui_database_changed();
        }
        
        ImGui::SameLine();
        
        // Show patch details
        ImGui::PushFont(g_font_mgr.m_menu_font_small);
        // Display address:value pair
        char value_str[64] = "";
        for (int b = 0; b < patch->address_values[0].value_length; b++) {
            snprintf(value_str + b * 2, sizeof(value_str) - b * 2, "%02X", patch->address_values[0].value_data[b]);
        }
        ImGui::Text("0x%08X = 0x%s", patch->address_values[0].address, value_str);
        ImGui::PopFont();
        
        ImGui::SameLine();
        
        // Edit button
        if (ImGui::Button("Edit")) {

            fflush(stdout);
            m_selected_game_index = game_index;
            m_editing_patch_index = j;
            m_edit_error_message[0] = '\0'; // Clear any previous errors
            // Load patch data into edit fields and show edit dialog
            XemuMemoryPatch *patch = &game->patches[j];
            
            // Load patch data into edit fields
            strncpy(m_patch_name, patch->name ? patch->name : "", sizeof(m_patch_name) - 1);
            m_patch_name[sizeof(m_patch_name) - 1] = '\0';
            strncpy(m_patch_category, patch->category ? patch->category : "", sizeof(m_patch_category) - 1);
            m_patch_category[sizeof(m_patch_category) - 1] = '\0';
            strncpy(m_patch_author, patch->author ? patch->author : "", sizeof(m_patch_author) - 1);
            m_patch_author[sizeof(m_patch_author) - 1] = '\0';
            strncpy(m_patch_notes, patch->notes ? patch->notes : "", sizeof(m_patch_notes) - 1);
            m_patch_notes[sizeof(m_patch_notes) - 1] = '\0';
            
            // Format address:value pairs as text - use original lines if available
            m_patch_address_value_pairs[0] = '\0';
            
            // If we have original address:value lines (with comments), use those
            if (patch->address_value_lines && patch->address_value_lines_count > 0) {
                for (int k = 0; k < patch->address_value_lines_count; k++) {
                    if (patch->address_value_lines[k] && strlen(patch->address_value_lines[k]) > 0) {
                        size_t current_len = strlen(m_patch_address_value_pairs);
                        size_t max_space = sizeof(m_patch_address_value_pairs) - current_len - 2;
                        if (max_space > 0) {
                            char *source_line = patch->address_value_lines[k];
                            size_t source_len = strlen(source_line);
                            size_t copy_len = (source_len < max_space) ? source_len : max_space - 1;
                            memcpy(m_patch_address_value_pairs + current_len, source_line, copy_len);
                            m_patch_address_value_pairs[current_len + copy_len] = '\0';
                            if (current_len + copy_len < sizeof(m_patch_address_value_pairs) - 2) {
                                strncat(m_patch_address_value_pairs, "\n", 
                                       sizeof(m_patch_address_value_pairs) - strlen(m_patch_address_value_pairs) - 1);
                            }
                        }
                    }
                }
            } else {
                // Fallback to rebuilding from parsed data (legacy patches)
                for (int k = 0; k < patch->address_value_count; k++) {
                    char hex_value[256] = "";
                    size_t hex_pos = 0;
                    for (int b = 0; b < patch->address_values[k].value_length && b < 64; b++) {
                        char hex_byte[4];
                        snprintf(hex_byte, sizeof(hex_byte), "%02X", patch->address_values[k].value_data[b]);
                        if (hex_pos + 2 < sizeof(hex_value) - 1) {
                            memcpy(hex_value + hex_pos, hex_byte, 2);
                            hex_pos += 2;
                        }
                    }
                    hex_value[hex_pos] = '\0';
                    
                    char pair_line[300];
                    snprintf(pair_line, sizeof(pair_line), "0x%08X:%s\n", 
                            patch->address_values[k].address, hex_value);
                    
                    size_t current_len = strlen(m_patch_address_value_pairs);
                    size_t max_space = sizeof(m_patch_address_value_pairs) - current_len - 1;
                    size_t pair_len = strlen(pair_line);
                    
                    if (pair_len < max_space) {
                        strncat(m_patch_address_value_pairs, pair_line, max_space);
                    }
                }
            }
            
            // Show the edit dialog
            m_show_edit_patch_dialog = true;
        }
        
        // Remove button
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0, 0, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0, 0, 1));
        if (ImGui::Button("Remove")) {
            if (xemu_patches_remove_patch(game_index, j)) {
                xemu_patches_on_ui_database_changed();
            }
        }
        ImGui::PopStyleColor(2);
        
        ImGui::PopID();
    }
}

void MainMenuPatchesView::DrawAddGameDialog(void)
{
    // Auto-populate fields if a game is currently loaded
    static bool fields_initialized = false;
    static uint32_t last_cert_title_id = 0; // Track certificate changes
    
    // Get current XBE info
    struct xbe *debug_xbe = xemu_get_xbe_info();
    
    // Reset fields_initialized when dialog is closed and reopened
    if (!m_show_add_game_dialog) {
        fields_initialized = false;
        last_cert_title_id = 0; // Reset certificate tracking
    }
    
    // 🔧 FIX: Reset fields_initialized when certificate data changes
    // This ensures the dialog gets fresh data when a new game is loaded via Load Disc
    uint32_t current_cert_title_id = 0;
    if (debug_xbe && debug_xbe->cert) {
        current_cert_title_id = ldl_le_p(&debug_xbe->cert->m_titleid);
    }
    
    if (current_cert_title_id != last_cert_title_id) {
        fields_initialized = false; // Force re-population
        last_cert_title_id = current_cert_title_id;
    }
    
    if (!fields_initialized && m_show_add_game_dialog) {
        const char* current_xbe = xemu_patches_get_current_xbe_path();
        
        // Try to get XBE info from runtime first (most reliable)
        struct xbe *xbe_info = xemu_get_xbe_info();
        if (xbe_info && xbe_info->cert) {
            
            // Use certificate data from current runtime
            char title[128], title_id[32], region[64], version[32];
            
            // Extract certificate data directly
            struct xbe_certificate *cert = xbe_info->cert;
            
            // Convert unicode title to UTF-8 (simplified - assumes ASCII characters)
            char title_utf8[256] = {0};
            int max_chars = (sizeof(title) - 1 < 40) ? (sizeof(title) - 1) : 40;
            for (int i = 0; i < max_chars && cert->m_title_name[i] != 0; i++) {
                title_utf8[i] = (char)cert->m_title_name[i]; // Simple ASCII conversion
            }
            strncpy(title, title_utf8, sizeof(title) - 1);
            title[sizeof(title) - 1] = '\0';

            
            // Format title ID as hex string
            snprintf(title_id, sizeof(title_id), "%08X", cert->m_titleid);

            
            // Determine region from game_region field
            uint32_t region_code = ldl_le_p(&cert->m_game_region);
            switch (region_code) {
                case 0x01: strncpy(region, "NTSC-U", sizeof(region) - 1); break; // North America
                case 0x02: strncpy(region, "NTSC-J", sizeof(region) - 1); break; // Japan
                case 0x04: strncpy(region, "PAL", sizeof(region) - 1); break;    // PAL/Europe
                case 0x05: strncpy(region, "NTSC-K", sizeof(region) - 1); break; // Korea
                default:   strncpy(region, "NTSC", sizeof(region) - 1); break;   // Default
            }
            region[sizeof(region) - 1] = '\0';

            
            // Format version (correct format: major.minor.patch.build)
            uint32_t version_num = ldl_le_p(&cert->m_version);
            uint8_t major = (version_num >> 24) & 0xFF;
            uint8_t minor = (version_num >> 16) & 0xFF;
            uint8_t patch = (version_num >> 8) & 0xFF;
            uint8_t build = version_num & 0xFF;
            snprintf(version, sizeof(version), "%d.%d.%d.%d", major, minor, patch, build);

            
            // Extract alternate title ID (use first alternate title ID if available)
            char alternate_title_id[32];
            uint32_t alt_title_id = ldl_le_p(&cert->m_alt_title_id[0]);
            snprintf(alternate_title_id, sizeof(alternate_title_id), "%08X", alt_title_id);

            
            // Extract timestamp and format as yyyy-mm-dd hh:mm:ss
            char time_date[32];
            uint32_t timedate = ldl_le_p(&cert->m_timedate);

            time_t timestamp = timedate; // XBE timedate is UNIX timestamp
            struct tm *timeinfo = localtime(&timestamp);
            if (timeinfo) {
                strftime(time_date, sizeof(time_date), "%Y-%m-%d %H:%M:%S", timeinfo);

            } else {
                snprintf(time_date, sizeof(time_date), "1970-01-01 00:00:00");

            }
            
            // Extract disc number
            char disc_number[8];
            uint32_t disk_num = ldl_le_p(&cert->m_disk_number);
            snprintf(disc_number, sizeof(disc_number), "%u", disk_num);

            
            // Validate extracted data before copying
            
            // Copy validated data to member variables
            strncpy(m_add_game_title, title, sizeof(m_add_game_title) - 1);
            m_add_game_title[sizeof(m_add_game_title) - 1] = '\0';

            
            strncpy(m_add_game_title_id, title_id, sizeof(m_add_game_title_id) - 1);
            m_add_game_title_id[sizeof(m_add_game_title_id) - 1] = '\0';
            strncpy(m_add_game_region, region, sizeof(m_add_game_region) - 1);
            m_add_game_region[sizeof(m_add_game_region) - 1] = '\0';
            strncpy(m_add_game_version, version, sizeof(m_add_game_version) - 1);
            m_add_game_version[sizeof(m_add_game_version) - 1] = '\0';
            strncpy(m_add_game_alternate_title_id, alternate_title_id, sizeof(m_add_game_alternate_title_id) - 1);
            m_add_game_alternate_title_id[sizeof(m_add_game_alternate_title_id) - 1] = '\0';
            strncpy(m_add_game_time_date, time_date, sizeof(m_add_game_time_date) - 1);
            m_add_game_time_date[sizeof(m_add_game_time_date) - 1] = '\0';
            strncpy(m_add_game_disc_number, disc_number, sizeof(m_add_game_disc_number) - 1);
            m_add_game_disc_number[sizeof(m_add_game_disc_number) - 1] = '\0';
            
            fields_initialized = true;
            last_cert_title_id = current_cert_title_id;
        } else if (current_xbe) {
            // Fallback to filename parsing
            char title[128], title_id[32], region[64], version[32];
            char alternate_title_id[32], time_date[32], disc_number[8];
            
            if (xemu_patches_parse_xbe_certificate(current_xbe, 
                                                  title, sizeof(title),
                                                  title_id, sizeof(title_id),
                                                  region, sizeof(region),
                                                  version, sizeof(version),
                                                  alternate_title_id, sizeof(alternate_title_id),
                                                  time_date, sizeof(time_date),
                                                  disc_number, sizeof(disc_number))) {
                
                // Copy parsed data to member variables
                strncpy(m_add_game_title, title, sizeof(m_add_game_title) - 1);
                m_add_game_title[sizeof(m_add_game_title) - 1] = '\0';
                strncpy(m_add_game_title_id, title_id, sizeof(m_add_game_title_id) - 1);
                m_add_game_title_id[sizeof(m_add_game_title_id) - 1] = '\0';
                strncpy(m_add_game_region, region, sizeof(m_add_game_region) - 1);
                m_add_game_region[sizeof(m_add_game_region) - 1] = '\0';
                strncpy(m_add_game_version, version, sizeof(m_add_game_version) - 1);
                m_add_game_version[sizeof(m_add_game_version) - 1] = '\0';
                strncpy(m_add_game_alternate_title_id, alternate_title_id, sizeof(m_add_game_alternate_title_id) - 1);
                m_add_game_alternate_title_id[sizeof(m_add_game_alternate_title_id) - 1] = '\0';
                strncpy(m_add_game_time_date, time_date, sizeof(m_add_game_time_date) - 1);
                m_add_game_time_date[sizeof(m_add_game_time_date) - 1] = '\0';
                strncpy(m_add_game_disc_number, disc_number, sizeof(m_add_game_disc_number) - 1);
                m_add_game_disc_number[sizeof(m_add_game_disc_number) - 1] = '\0';
            } else {

            }
            fields_initialized = true;
            last_cert_title_id = current_cert_title_id; // Update tracking
        } else {


            
            // No XBE available - clear fields
            m_add_game_title[0] = '\0';
            m_add_game_region[0] = '\0';
            m_add_game_title_id[0] = '\0';
            m_add_game_version[0] = '\0';
            m_add_game_alternate_title_id[0] = '\0';
            m_add_game_time_date[0] = '\0';
            m_add_game_disc_number[0] = '\0';
            fields_initialized = true;
            last_cert_title_id = current_cert_title_id; // Update tracking
        }
        

    }
    
    // Draw a modal dialog for adding a new game
    ImGui::SetNextWindowSize(g_viewport_mgr.Scale(ImVec2(480, 450)), ImGuiCond_Always);
    ImGui::Begin("Add Game", &m_show_add_game_dialog);
    
    // Auto-population notice - ONLY show XBE data when available
    struct xbe *current_xbe = xemu_get_xbe_info();
    if (current_xbe && current_xbe->cert) {
        ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.0f, 1.0f), "Auto-populated from loaded XBE certificate");
    } else {
        // User requested: NEVER show disc path auto-population
        // Only show XBE data when available, otherwise show empty state
        if (strlen(m_add_game_title) > 0) {
            // Fields have been populated but XBE is not currently available
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.0f, 1.0f), "Waiting for XBE certificate information to populate...");
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.0f, 0.0f, 1.0f), "No XBE certificate loaded - Add Game button disabled");
        }
    }
    
    // 🆕 CONTINUOUS XBE MONITORING - Update fields when XBE becomes available
    // Even if fields were already initialized, check if XBE data is now available
    // and update the fields accordingly
    
    // 🆕 DISABLE ADD GAME BUTTON UNTIL XBE CERTIFICATE DATA IS AVAILABLE
    // User requested: Button should be grayed out until XBE data is loaded

    
    // Monitor state for updates
    
    if (m_show_add_game_dialog) {
        struct xbe *current_xbe = xemu_get_xbe_info();
        bool should_update = false;
        
        // Check if we need to update: empty title OR title contains path separators
        if (strlen(m_add_game_title) == 0 || 
            strchr(m_add_game_title, '\\') != NULL || 
            strchr(m_add_game_title, '/') != NULL) {
            should_update = true;
        }
        
        if (current_xbe && current_xbe->cert && should_update) {
            // XBE became available but fields are empty or contain path - populate them
            
            // Use certificate data from current runtime
            char title[128], title_id[32], region[64], version[32];
            
            // Extract certificate data directly
            struct xbe_certificate *cert = current_xbe->cert;
            
            // Convert unicode title to UTF-8 (simplified - assumes ASCII characters)
            char title_utf8[256] = {0};
            int max_chars = (sizeof(title) - 1 < 40) ? (sizeof(title) - 1) : 40;
            for (int i = 0; i < max_chars && cert->m_title_name[i] != 0; i++) {
                title_utf8[i] = (char)cert->m_title_name[i]; // Simple ASCII conversion
            }
            strncpy(title, title_utf8, sizeof(title) - 1);
            title[sizeof(title) - 1] = '\0';
            
            // Format title ID as hex string
            snprintf(title_id, sizeof(title_id), "%08X", cert->m_titleid);
            
            // Determine region from game_region field
            uint32_t region_code = ldl_le_p(&cert->m_game_region);
            switch (region_code) {
                case 0x01: strncpy(region, "NTSC-U", sizeof(region) - 1); break; // North America
                case 0x02: strncpy(region, "NTSC-J", sizeof(region) - 1); break; // Japan
                case 0x04: strncpy(region, "PAL", sizeof(region) - 1); break;    // PAL/Europe
                case 0x05: strncpy(region, "NTSC-K", sizeof(region) - 1); break; // Korea
                default:   strncpy(region, "NTSC", sizeof(region) - 1); break;   // Default
            }
            region[sizeof(region) - 1] = '\0';
            
            // Format version (correct format: major.minor.patch.build)
            uint32_t version_num = ldl_le_p(&cert->m_version);
            uint8_t major = (version_num >> 24) & 0xFF;
            uint8_t minor = (version_num >> 16) & 0xFF;
            uint8_t patch = (version_num >> 8) & 0xFF;
            uint8_t build = version_num & 0xFF;
            snprintf(version, sizeof(version), "%d.%d.%d.%d", major, minor, patch, build);

            
            // Extract alternate title ID (use first alternate title ID if available)
            char alternate_title_id[32];
            uint32_t alt_title_id = ldl_le_p(&cert->m_alt_title_id[0]);
            snprintf(alternate_title_id, sizeof(alternate_title_id), "%08X", alt_title_id);
            
            // Extract timestamp and format as yyyy-mm-dd hh:mm:ss
            char time_date[32];
            uint32_t timedate = ldl_le_p(&cert->m_timedate);
            time_t timestamp = timedate; // XBE timedate is UNIX timestamp
            struct tm *timeinfo = localtime(&timestamp);
            if (timeinfo) {
                strftime(time_date, sizeof(time_date), "%Y-%m-%d %H:%M:%S", timeinfo);
            } else {
                snprintf(time_date, sizeof(time_date), "1970-01-01 00:00:00");
            }
            
            // Extract disc number
            char disc_number[8];
            uint32_t disk_num = ldl_le_p(&cert->m_disk_number);
            snprintf(disc_number, sizeof(disc_number), "%u", disk_num);
            
            // Update all fields with XBE data
            strncpy(m_add_game_title, title, sizeof(m_add_game_title) - 1);
            m_add_game_title[sizeof(m_add_game_title) - 1] = '\0';
            strncpy(m_add_game_title_id, title_id, sizeof(m_add_game_title_id) - 1);
            m_add_game_title_id[sizeof(m_add_game_title_id) - 1] = '\0';
            strncpy(m_add_game_region, region, sizeof(m_add_game_region) - 1);
            m_add_game_region[sizeof(m_add_game_region) - 1] = '\0';
            strncpy(m_add_game_version, version, sizeof(m_add_game_version) - 1);
            m_add_game_version[sizeof(m_add_game_version) - 1] = '\0';
            strncpy(m_add_game_alternate_title_id, alternate_title_id, sizeof(m_add_game_alternate_title_id) - 1);
            m_add_game_alternate_title_id[sizeof(m_add_game_alternate_title_id) - 1] = '\0';
            strncpy(m_add_game_time_date, time_date, sizeof(m_add_game_time_date) - 1);
            m_add_game_time_date[sizeof(m_add_game_time_date) - 1] = '\0';
            strncpy(m_add_game_disc_number, disc_number, sizeof(m_add_game_disc_number) - 1);
            m_add_game_disc_number[sizeof(m_add_game_disc_number) - 1] = '\0';
            // Fields updated from XBE certificate

        }
    }
    if (current_xbe && current_xbe->cert) {
        bool needs_update = false;
        

        
        // Check if fields are empty, contain path separators, or different from XBE data
        if (strlen(m_add_game_title) == 0 || 
            strlen(m_add_game_title_id) == 0 ||
            strcmp(m_add_game_title_id, "00000000") == 0 ||
            strchr(m_add_game_title, '\\') != NULL || 
            strchr(m_add_game_title, '/') != NULL) {
            needs_update = true;

        } else {
            // Fields appear OK, no update needed
        }
        
        if (needs_update) {

            
            // Extract certificate data from current runtime
            char title[128], title_id[32], region[64], version[32];
            char alternate_title_id[32], time_date[32], disc_number[8];
            
            // Extract certificate data directly
            struct xbe_certificate *cert = current_xbe->cert;
            
            // Convert unicode title to UTF-8 (simplified - assumes ASCII characters)
            char title_utf8[256] = {0};
            int max_chars = (sizeof(title) - 1 < 40) ? (sizeof(title) - 1) : 40;
            for (int i = 0; i < max_chars && cert->m_title_name[i] != 0; i++) {
                title_utf8[i] = (char)cert->m_title_name[i]; // Simple ASCII conversion
            }
            strncpy(title, title_utf8, sizeof(title) - 1);
            title[sizeof(title) - 1] = '\0';
            
            // Format title ID as hex string
            snprintf(title_id, sizeof(title_id), "%08X", cert->m_titleid);
            
            // Determine region from game_region field
            uint32_t region_code = ldl_le_p(&cert->m_game_region);
            switch (region_code) {
                case 0x01: strncpy(region, "NTSC-U", sizeof(region) - 1); break; // North America
                case 0x02: strncpy(region, "NTSC-J", sizeof(region) - 1); break; // Japan
                case 0x04: strncpy(region, "PAL", sizeof(region) - 1); break;    // PAL/Europe
                case 0x05: strncpy(region, "NTSC-K", sizeof(region) - 1); break; // Korea
                default:   strncpy(region, "NTSC", sizeof(region) - 1); break;   // Default
            }
            region[sizeof(region) - 1] = '\0';
            
            // Format version (correct format: major.minor.patch.build)
            uint32_t version_num = ldl_le_p(&cert->m_version);
            uint8_t major = (version_num >> 24) & 0xFF;
            uint8_t minor = (version_num >> 16) & 0xFF;
            uint8_t patch = (version_num >> 8) & 0xFF;
            uint8_t build = version_num & 0xFF;
            snprintf(version, sizeof(version), "%d.%d.%d.%d", major, minor, patch, build);
            
            // Extract alternate title ID (use first alternate title ID if available)
            uint32_t alt_title_id = ldl_le_p(&cert->m_alt_title_id[0]);
            snprintf(alternate_title_id, sizeof(alternate_title_id), "%08X", alt_title_id);
            
            // Extract timestamp and format as yyyy-mm-dd hh:mm:ss
            uint32_t timedate = ldl_le_p(&cert->m_timedate);
            time_t timestamp = timedate; // XBE timedate is UNIX timestamp
            struct tm *timeinfo = localtime(&timestamp);
            if (timeinfo) {
                strftime(time_date, sizeof(time_date), "%Y-%m-%d %H:%M:%S", timeinfo);
            } else {
                snprintf(time_date, sizeof(time_date), "1970-01-01 00:00:00");
            }
            
            // Extract disc number
            uint32_t disk_num = ldl_le_p(&cert->m_disk_number);
            snprintf(disc_number, sizeof(disc_number), "%u", disk_num);
            
            // Update fields with XBE data
            strncpy(m_add_game_title, title, sizeof(m_add_game_title) - 1);
            m_add_game_title[sizeof(m_add_game_title) - 1] = '\0';
            strncpy(m_add_game_title_id, title_id, sizeof(m_add_game_title_id) - 1);
            m_add_game_title_id[sizeof(m_add_game_title_id) - 1] = '\0';
            strncpy(m_add_game_region, region, sizeof(m_add_game_region) - 1);
            m_add_game_region[sizeof(m_add_game_region) - 1] = '\0';
            strncpy(m_add_game_version, version, sizeof(m_add_game_version) - 1);
            m_add_game_version[sizeof(m_add_game_version) - 1] = '\0';
            strncpy(m_add_game_alternate_title_id, alternate_title_id, sizeof(m_add_game_alternate_title_id) - 1);
            m_add_game_alternate_title_id[sizeof(m_add_game_alternate_title_id) - 1] = '\0';
            strncpy(m_add_game_time_date, time_date, sizeof(m_add_game_time_date) - 1);
            m_add_game_time_date[sizeof(m_add_game_time_date) - 1] = '\0';
            strncpy(m_add_game_disc_number, disc_number, sizeof(m_add_game_disc_number) - 1);
            m_add_game_disc_number[sizeof(m_add_game_disc_number) - 1] = '\0';
            // Secondary monitoring update completed
        } else {
            // No update performed
        }
    }
    // Final state summary completed
    
    ImGui::Separator();
    
    // Game title (read-only)
    ImGui::InputText("Game Title", m_add_game_title, sizeof(m_add_game_title), ImGuiInputTextFlags_ReadOnly);
    
    // Region (read-only)
    ImGui::InputText("Region", m_add_game_region, sizeof(m_add_game_region), ImGuiInputTextFlags_ReadOnly);
    
    // Title ID (read-only)
    ImGui::InputText("Title ID", m_add_game_title_id, sizeof(m_add_game_title_id), ImGuiInputTextFlags_ReadOnly);
    
    // Version (read-only)
    ImGui::InputText("Version", m_add_game_version, sizeof(m_add_game_version), ImGuiInputTextFlags_ReadOnly);
    
    // Alternate Title ID (read-only)
    ImGui::InputText("Alternate Title ID", m_add_game_alternate_title_id, sizeof(m_add_game_alternate_title_id), ImGuiInputTextFlags_ReadOnly);
    
    // Time Date (read-only)
    ImGui::InputText("Time Date", m_add_game_time_date, sizeof(m_add_game_time_date), ImGuiInputTextFlags_ReadOnly);
    
    // Disc Number (read-only)
    ImGui::InputText("Disc Number", m_add_game_disc_number, sizeof(m_add_game_disc_number), ImGuiInputTextFlags_ReadOnly);
    
    ImGui::Dummy(g_viewport_mgr.Scale(ImVec2(0, 10)));
    
    // Add spacing and green "Add Game" button
    ImGui::Dummy(g_viewport_mgr.Scale(ImVec2(200, 0))); // Push button to the right
    ImGui::SameLine();
    
    // Add Game button based only on disc presence
    bool disc_present = g_disc_present;  // INLINED: Replaced is_disc_present() call
    
    if (!disc_present) {
        ImGui::BeginDisabled();
    }
    
    // Style the "Add Game" button as green (when enabled)
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.7f, 0.0f, 1.0f)); // Green color
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.8f, 0.0f, 1.0f)); // Brighter green on hover
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.6f, 0.0f, 1.0f)); // Darker green when pressed
    
    // Add Game button - only clickable when XBE data is available
    if (ImGui::Button("Add Game", ImVec2(100, 0))) {

        
        // This should only execute when XBE data is available
        if (strlen(m_add_game_title) > 0 && strlen(m_add_game_title_id) > 0) {
            // Check if game already exists
            // 🆕 IMPROVED DUPLICATE CHECK: Check both Title ID AND Version
            int existing_game = xemu_patches_find_duplicate_game(m_add_game_title_id, m_add_game_version);
            if (existing_game >= 0) {
                XemuGamePatches *duplicate = &g_patches_db.games[existing_game];
                char error_msg[256];
                snprintf(error_msg, sizeof(error_msg), 
                        "Exact duplicate found: '%s' (Title ID: %s, Version: %s)\n"
                        "Same Title ID AND Version already exist in database",
                        duplicate->game_title ? duplicate->game_title : "Unknown",
                        duplicate->title_id ? duplicate->title_id : "NULL",
                        duplicate->version ? duplicate->version : "NULL");
                xemu_queue_error_message(error_msg);
            } else {
                // Add the game
                // Add the game
                
                if (xemu_patches_add_game(m_add_game_title, m_add_game_region, 
                                         m_add_game_title_id, m_add_game_version,
                                         m_add_game_alternate_title_id, m_add_game_time_date,
                                         m_add_game_disc_number)) {
                    // Game added successfully
                    
                    xemu_patches_on_ui_database_changed();
                    xemu_queue_notification("Added game to patches database");
                    m_show_add_game_dialog = false;
                    fields_initialized = false;  // Reset so next open will auto-populate
                    
                    // Clear fields
                    m_add_game_title[0] = '\0';
                    m_add_game_region[0] = '\0';
                    m_add_game_title_id[0] = '\0';
                    m_add_game_version[0] = '\0';
                    m_add_game_alternate_title_id[0] = '\0';
                    m_add_game_time_date[0] = '\0';
                    m_add_game_disc_number[0] = '\0';
                } else {

                    xemu_queue_error_message("Failed to add game");
                }

            }
        } else {
            xemu_queue_error_message("Game Title and Title ID are required");
        }
    }
    
    // Pop the style colors
    ImGui::PopStyleColor();
    ImGui::PopStyleColor();
    ImGui::PopStyleColor();
    
    // 🛡️ FIX: Ensure EndDisabled() is called to match BeginDisabled()
    if (!disc_present) {

        ImGui::EndDisabled();
    }
    
    ImGui::SameLine();
    
    // Cancel button
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        m_show_add_game_dialog = false;
        fields_initialized = false;
        // Clear fields
        m_add_game_title[0] = '\0';
        m_add_game_region[0] = '\0';
        m_add_game_title_id[0] = '\0';
        m_add_game_version[0] = '\0';
        m_add_game_alternate_title_id[0] = '\0';
        m_add_game_time_date[0] = '\0';
        m_add_game_disc_number[0] = '\0';
    }
    
    ImGui::End();
}

void MainMenuPatchesView::DrawAddPatchDialog(void)
{
    int game_count = xemu_patches_get_game_count();
    if (game_count <= 0 || m_selected_game_index < 0 || m_selected_game_index >= game_count) {
        m_show_add_patch_dialog = false;
        return;
    }
    
    XemuGamePatches *games = xemu_patches_get_games();
    if (!games) {
        return;
    }
    
    XemuGamePatches *game = &games[m_selected_game_index];
    
    // Show add patch dialog (smaller horizontally)
    ImGui::SetNextWindowSize(g_viewport_mgr.Scale(ImVec2(600, 750)), ImGuiCond_FirstUseEver);
    
    if (ImGui::Begin("Add New Patch", &m_show_add_patch_dialog, ImGuiWindowFlags_NoSavedSettings)) {
        // Show game info at the top
        char game_info[256];
        snprintf(game_info, sizeof(game_info), "%s (%s - %s) (%s)", 
                game->game_title, game->title_id, game->region, game->version);
        ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.0f, 1.0f), "%s", game_info);
        ImGui::Separator();
        
        // Patch Title (Required)
        ImGui::Text("Patch Name (Required):");
        ImGui::SetNextItemWidth(400.0f);
        if (ImGui::InputText("##PatchName", m_patch_name, sizeof(m_patch_name), ImGuiInputTextFlags_AllowTabInput)) {
        }
        
        if (strlen(m_patch_name) == 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Patch name is required");
        }
        
        ImGui::Spacing();
        
        // Author (Optional)
        ImGui::Text("Author (Optional):");
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::InputText("##Author", m_patch_author, sizeof(m_patch_author))) {
        }
        
        ImGui::Spacing();
        
        // Category (Required)
        ImGui::Text("Category (Required):");
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::InputText("##Category", m_patch_category, sizeof(m_patch_category))) {
        }
        
        if (strlen(m_patch_category) == 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Category is required (e.g., Cheat, Enhancement, Widescreen)");
        }
        
        ImGui::Spacing();
        
        // Patch Notes (Optional)
        ImGui::Text("Patch Notes (Optional):");
        ImGui::SetNextItemWidth(500.0f);
        if (ImGui::InputTextMultiline("##PatchNotes", m_patch_notes, sizeof(m_patch_notes), 
                                      ImVec2(-FLT_MIN, 80), 
                                      0)) {
        }
        
        ImGui::Spacing();
        
        // Memory Address:Value pairs (multiline with horizontal scroll)
        ImGui::Text("Memory Addresses and Values (Required):");
        ImGui::SetNextItemWidth(550.0f);
        ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput;
        ImGui::InputTextMultiline("##AddressValue", m_patch_address_value_pairs, 
                                  sizeof(m_patch_address_value_pairs),
                                  ImVec2(-FLT_MIN, 250), flags);
        
        ImGui::TextDisabled("Format: Each line should be 'address:value' (hex)");
        ImGui::TextDisabled("Example: 0x1234ABCD:5678EF90");
        ImGui::TextDisabled("Comments: Use # after the value (e.g., 0x1234ABCD:5678EF90 # Comment here)");
        
        ImGui::Spacing();
        
        // Save Replaced Values option
        if (ImGui::Checkbox("##SaveReplacedValues", &m_save_replaced_values)) {
        }
        ImGui::SameLine();
        ImGui::Text("Save Replaced Values (enables undo/redo functionality)");
        ImGui::TextDisabled("When enabled: Saves original memory values before applying patch");
        ImGui::TextDisabled("When disabled: Memory is not restored when patch is disabled");
        
        ImGui::Spacing();
        
        // Basic format feedback (comprehensive validation happens on button click)
        if (strlen(m_patch_address_value_pairs) > 0) {
            // Simple format check for display purposes
            const char* text = m_patch_address_value_pairs;
            int line_count = 0;
            
            // Count lines and check for basic syntax
            while (*text) {
                // Skip empty lines and whitespace
                while (*text && isspace(*text)) text++;
                if (!*text) break;
                
                // Check if line contains colon (basic requirement)
                const char* colon = strchr(text, ':');
                if (colon) {
                    line_count++;
                }
                
                // Move to next line
                text = strchr(text, '\n');
                if (!text) break;
                text++;
            }
            
            if (line_count > 0) {
                ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.0f, 1.0f), "Format preview: %d line(s) with address:value pairs", line_count);
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "No valid address:value pairs found");
            }
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "At least one address:value pair is required");
        }
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // Buttons
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.7f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.8f, 0.0f, 1.0f));
        
        // Display error message if any
        static char error_message[256] = "";
        if (error_message[0] != '\0') {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", error_message);
        }
        
        if (ImGui::Button("Add Patch", ImVec2(120, 0))) {
            char validation_error[256];
            bool validation_result = ValidatePatchData(m_patch_name, m_patch_category, m_patch_address_value_pairs, 
                                validation_error, sizeof(validation_error), false, -1, games, m_selected_game_index);
            if (validation_result) {
                // Add the patch
                // Add the patch
                
                bool add_result = xemu_patches_add_patch(m_selected_game_index, m_patch_name, m_patch_category, m_patch_author, m_patch_notes, m_patch_address_value_pairs, m_save_replaced_values);
                


                fflush(stdout);
                
                if (add_result) {

                    xemu_patches_on_ui_database_changed();
                    

                    xemu_queue_notification("Patch added successfully");
                    
                    // Clear fields
                    memset(m_patch_name, 0, sizeof(m_patch_name));
                    memset(m_patch_category, 0, sizeof(m_patch_category));
                    memset(m_patch_author, 0, sizeof(m_patch_author));
                    memset(m_patch_address_value_pairs, 0, sizeof(m_patch_address_value_pairs));
                    m_save_replaced_values = false; // Default: don't save replaced values
                    error_message[0] = '\0'; // Clear error message on success
                    m_show_add_patch_dialog = false;
                    

                } else {

                }
            } else {
                // Validation failed, display error message
                snprintf(error_message, sizeof(error_message), "%s", validation_error);
            }
        }
        
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            // Clear fields
            memset(m_patch_name, 0, sizeof(m_patch_name));
            memset(m_patch_category, 0, sizeof(m_patch_category));
            memset(m_patch_author, 0, sizeof(m_patch_author));
            memset(m_patch_notes, 0, sizeof(m_patch_notes));
            memset(m_patch_address_value_pairs, 0, sizeof(m_patch_address_value_pairs));
            m_save_replaced_values = false; // Default: don't save replaced values
            error_message[0] = '\0'; // Clear error message
            m_show_add_patch_dialog = false;
        }
        
        ImGui::End();
    }
}

void MainMenuPatchesView::DrawGamesTable(void)
{
    int game_count = xemu_patches_get_game_count();
    XemuGamePatches *games = xemu_patches_get_games();
    
    // Initialize table flags
    table_flags = ImGuiTableFlags_Borders | ImGuiTableFlags_Sortable | 
                  ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg;
    
    // Filter for games with active patches
    static bool filter_active_patches = false;
    
    // Add Game button and search
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));
    
    // Search bar
    static char search_text[128] = "";
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("Search", search_text, sizeof(search_text));
    
    ImGui::SameLine();
    
    // Add Game button based only on disc presence
    bool disc_present = g_disc_present;  // INLINED: Replaced is_disc_present() call
    
    // Make Add Game button green (when enabled)
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.7f, 0.0f, 1.0f)); // Green color
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.8f, 0.0f, 1.0f)); // Brighter green on hover
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.6f, 0.0f, 1.0f)); // Darker green when pressed
    
    // SIMPLE DISC PRESENCE CHECK: Disable Add Game button when no disc is present
    if (!disc_present) {

        ImGui::BeginDisabled();
    }
    
    if (ImGui::Button("Add Game", ImVec2(100, 0))) {

        m_show_add_game_dialog = true;
    }
    
    // 🛡️ DISC PRESENCE CHECK: Close disabled scope if needed
    if (!disc_present) {
        ImGui::EndDisabled();
    }
    
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    
    // Filter for active patches (moved below search bar)
    ImGui::Checkbox("Show only games with active patches", &filter_active_patches);
    
    ImGui::Dummy(g_viewport_mgr.Scale(ImVec2(0, 8)));
    
    // Filter games based on search
    int filtered_count = 0;
    XemuGamePatches *filtered_games = nullptr;
    int *filtered_to_original_index = nullptr; // Mapping from filtered index to original index
    
    // Performance optimization: Skip extensive debug logging for normal operation

    
    if (game_count > 0) {
        filtered_games = g_new(XemuGamePatches, game_count);
        filtered_to_original_index = g_new(int, game_count);
        
        // Apply search filter
        for (int i = 0; i < game_count; i++) {
            bool matches = true;
            if (strlen(search_text) > 0) {
                const char* title = games[i].game_title ? games[i].game_title : "";
                const char* region = games[i].region ? games[i].region : "";
                const char* title_id = games[i].title_id ? games[i].title_id : "";
                const char* version = games[i].version ? games[i].version : "";
                
                // Case-insensitive search for Windows compatibility
                bool title_match = (stristr(title, search_text) != nullptr);
                bool region_match = (stristr(region, search_text) != nullptr);
                bool title_id_match = (stristr(title_id, search_text) != nullptr);
                bool version_match = (stristr(version, search_text) != nullptr);
                
                matches = title_match || region_match || title_id_match || version_match;
            }
            
            // Filter for games with active patches if checkbox is checked
            if (filter_active_patches) {
                bool has_active_patches = false;
                for (int p = 0; p < games[i].patch_count; p++) {
                    if (games[i].patches[p].enabled) {
                        has_active_patches = true;
                        break;
                    }
                }
                matches = matches && has_active_patches;
            }
            
            if (matches) {
                // Copy game structure
                filtered_games[filtered_count] = games[i];
                
                // Copy string pointers (these point to the original data, which is fine)
                // The original data is managed by the patches database
                
                // Copy patches array pointers (also point to original data)
                // This is safe because we're not modifying the original data
                
                filtered_to_original_index[filtered_count] = i; // Store original index
                
                filtered_count++;
            }
        }

        
        // Check if no games match the criteria
        if (filtered_count == 0) {
            if (game_count == 0) {
                ImGui::Text("No games found in patches database.");
            } else {
                ImGui::Text("No games match the search criteria.");
            }
            g_free(filtered_games);
            g_free(filtered_to_original_index);
            return;
        }
    }
    
    // Check if we have filtered games to display
    if (game_count == 0 || filtered_count == 0) {
        g_free(filtered_games);
        g_free(filtered_to_original_index);
        return;
    }
    
    // Create table with sortable columns
    if (ImGui::BeginTable("GamesTable", 5, 
                         ImGuiTableFlags_Borders | ImGuiTableFlags_Sortable | 
                         ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg)) {
        
        // Setup columns
        ImGui::TableSetupColumn("Game Title", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Title ID", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Patches", ImGuiTableColumnFlags_WidthFixed);
        
        ImGui::TableHeadersRow();
        
        // Handle sorting
        ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs();
        if (sort_specs->SpecsDirty && sort_specs->SpecsCount > 0) {
            const ImGuiTableColumnSortSpecs* sort_spec = &sort_specs->Specs[0];
            int column = sort_spec->ColumnIndex;
            bool ascending = sort_spec->SortDirection == ImGuiSortDirection_Ascending;

            
            // Sort both filtered games array and original index mapping together
            if (filtered_count > 1) {
                // Create arrays to hold current order for stable sorting
                std::vector<int> indices(filtered_count);
                for (int i = 0; i < filtered_count; i++) {
                    indices[i] = i;
                }
                
                // Sort indices based on game data using stable sort
                std::stable_sort(indices.begin(), indices.end(),
                    [column, ascending, &filtered_games](int a, int b) {
                        XemuGamePatches& ga = filtered_games[a];
                        XemuGamePatches& gb = filtered_games[b];
                        int result = 0;
                        
                        switch (column) {
                            case 0: // Game Title
                                result = strcmp(ga.game_title ? ga.game_title : "", 
                                               gb.game_title ? gb.game_title : "");
                                break;
                            case 1: // Region
                                result = strcmp(ga.region ? ga.region : "", 
                                               gb.region ? gb.region : "");
                                break;
                            case 2: // Title ID
                                result = strcmp(ga.title_id ? ga.title_id : "", 
                                               gb.title_id ? gb.title_id : "");
                                break;
                            case 3: // Version
                                result = strcmp(ga.version ? ga.version : "", 
                                               gb.version ? gb.version : "");
                                break;
                            case 4: // Patches (sort by enabled patches first, then total patches)
                                {
                                    int a_enabled = 0;
                                    for (int i = 0; i < ga.patch_count; i++) {
                                        if (ga.patches[i].enabled) a_enabled++;
                                    }
                                    int b_enabled = 0;
                                    for (int i = 0; i < gb.patch_count; i++) {
                                        if (gb.patches[i].enabled) b_enabled++;
                                    }
                                    // Sort by enabled patches first, then total patches
                                    result = (b_enabled - a_enabled);
                                    if (result == 0) {
                                        result = (gb.patch_count - ga.patch_count);
                                    }
                                }
                                break;
                        }
                        
                        return ascending ? (result < 0) : (result > 0);
                    });
                
                // Create temporary arrays for reordering
                XemuGamePatches* temp_games = g_new(XemuGamePatches, filtered_count);
                int* temp_indices = g_new(int, filtered_count);
                
                // Reorder according to sorted indices
                for (int i = 0; i < filtered_count; i++) {
                    temp_games[i] = filtered_games[indices[i]];
                    temp_indices[i] = filtered_to_original_index[indices[i]];
                }
                
                // Copy back to original arrays
                for (int i = 0; i < filtered_count; i++) {
                    filtered_games[i] = temp_games[i];
                    filtered_to_original_index[i] = temp_indices[i];
                }
                
                // Clean up temporary arrays
                g_free(temp_games);
                g_free(temp_indices);

            } // closes filtered_count > 1
        } // closes sort_specs->SpecsDirty conditional
        
        // Display all filtered games
        
        // Performance optimization: Only get current XBE certificate once for all rows
        struct xbe *current_xbe = xemu_get_xbe_info();
        bool is_current_xbe_available = (current_xbe && current_xbe->cert);
        uint32_t current_title_id = 0, current_region = 0, current_version = 0;
        
        if (is_current_xbe_available) {
            current_title_id = ldl_le_p(&current_xbe->cert->m_titleid);
            current_region = ldl_le_p(&current_xbe->cert->m_game_region);
            current_version = ldl_le_p(&current_xbe->cert->m_version);
            // Skip verbose logging for performance
        }
        
        for (int i = 0; i < filtered_count; i++) {
            ImGui::TableNextRow();
            XemuGamePatches *game = &filtered_games[i];
            
            // Store the game index for right-click context menu
            int original_game_index = filtered_to_original_index[i];
            
            // Check if game has any enabled patches for highlighting
            bool has_enabled_patches = false;
            for (int j = 0; j < game->patch_count; j++) {
                if (game->patches[j].enabled) {
                    has_enabled_patches = true;
                    break;
                }
            }
            
            // Check if this game matches the currently running XBE
            bool is_currently_running = false;
            if (is_current_xbe_available && game->title_id && game->region && game->version) {
                uint32_t game_title_id, game_region, game_version;
                
                // Parse title ID from hex string
                sscanf(game->title_id, "%X", &game_title_id);

                // Convert region string to numeric value
                if (strcmp(game->region, "NTSC-U") == 0) game_region = 0x00000001;
                else if (strcmp(game->region, "NTSC-J") == 0) game_region = 0x00000002;
                else if (strcmp(game->region, "NTSC-K") == 0) game_region = 0x00000004;
                else if (strcmp(game->region, "PAL") == 0) game_region = 0x00000008;
                else sscanf(game->region, "%X", &game_region);
                
                // Parse version string (format: major.minor.patch.build)
                int major, minor, patch, build;
                if (sscanf(game->version, "%d.%d.%d.%d", &major, &minor, &patch, &build) == 4) {
                    game_version = (major << 24) | (minor << 16) | (patch << 8) | build;
                } else {
                    game_version = 0;
                }
                
                // Check if all three values match
                bool title_match = (game_title_id == current_title_id);
                bool region_match = (game_region == current_region);
                bool version_match = (game_version == current_version);
                is_currently_running = title_match && region_match && version_match;

            }
            
            // Highlight rows with enabled patches in faint green
            if (has_enabled_patches) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(0, 200, 0, 25)); // Faint green (RGBA: 0, 200, 0, 25)
            }
            
            // Game Title (clickable to select game)
            ImGui::TableNextColumn();
            ImGui::PushID(i * 100); // Unique ID for game selection
            
            // Right-click context menu
            if (ImGui::BeginPopupContextItem("GameContextMenu")) {
                if (ImGui::MenuItem("Delete Game")) {
                    m_selected_game_index = original_game_index;
                    ImGui::OpenPopup("Confirm Delete Game");
                }
                ImGui::EndPopup();
            }
            
            // Make text yellow if this is the currently running game
            if (is_currently_running) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f)); // Yellow/orange color
            }
            
            if (ImGui::Selectable(game->game_title ? game->game_title : "Unknown Game", false, ImGuiSelectableFlags_SpanAllColumns)) {
                // Store the selected game for persistent window (use original index)
                int selected_index = filtered_to_original_index[i];

                
                m_selected_game_index = selected_index;
                m_show_game_details_window = true;
            }
            
            // Restore text color if we made it yellow
            if (is_currently_running) {
                ImGui::PopStyleColor();
            }
            
            ImGui::PopID();
            
            // Region
            ImGui::TableNextColumn();
            if (is_currently_running) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f)); // Yellow/orange color
            }
            ImGui::Text("%s", game->region ? game->region : "Unknown");
            if (is_currently_running) {
                ImGui::PopStyleColor();
            }
            
            // Title ID
            ImGui::TableNextColumn();
            if (is_currently_running) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f)); // Yellow/orange color
            }
            ImGui::Text("%s", game->title_id ? game->title_id : "Unknown");
            if (is_currently_running) {
                ImGui::PopStyleColor();
            }
            
            // Version
            ImGui::TableNextColumn();
            if (is_currently_running) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f)); // Yellow/orange color
            }
            ImGui::Text("%s", game->version ? game->version : "Unknown");
            if (is_currently_running) {
                ImGui::PopStyleColor();
            }
            
            // Patches (count and enabled count)
            ImGui::TableNextColumn();
            if (game->patch_count > 0) {
                int enabled_count = 0;
                for (int p = 0; p < game->patch_count; p++) {
                    if (game->patches[p].enabled) enabled_count++;
                }
                
                if (enabled_count > 0) {
                    // Show total count, then enabled count in green
                    ImGui::Text("%d ", game->patch_count);
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.0f, 1.0f), "(%d)", enabled_count);
                } else {
                    ImGui::Text("%d", game->patch_count);
                }
            } else {
                ImGui::Text("0");
            }
            
        }
        
        ImGui::EndTable();
    }
    
    // Clean up filtered games array and index mapping
    g_free(filtered_games);
    g_free(filtered_to_original_index);
}

void MainMenuPatchesView::DrawGameDetailsWindow(void)
{
    // Game details window implementation
    if (!m_show_game_details_window) {
        return;
    }
    
    // Validate selected game index
    int game_count = xemu_patches_get_game_count();
    if (game_count <= 0 || m_selected_game_index < 0 || m_selected_game_index >= game_count) {
        // Reset invalid state
        m_show_game_details_window = false;
        m_selected_game_index = -1;
        return;
    }
    
    XemuGamePatches *games = xemu_patches_get_games();
    
    if (games && game_count > 0 && m_selected_game_index >= 0 && m_selected_game_index < game_count) {
            XemuGamePatches *game = &games[m_selected_game_index];
            
            ImGui::SetNextWindowSize(g_viewport_mgr.Scale(ImVec2(720, 800)), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Game Details & Patches", &m_show_game_details_window, ImGuiWindowFlags_NoSavedSettings)) {
                
                // Format game title with title id, region, and version
                char title_buffer[512];
                snprintf(title_buffer, sizeof(title_buffer), "%s (%s - %s) (%s)",
                        game->game_title ? game->game_title : "Unknown",
                        game->title_id ? game->title_id : "Unknown",
                        game->region ? game->region : "Unknown",
                        game->version ? game->version : "Unknown");
                
                // Title and Delete Game button in same row
                ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.0f, 1.0f), "%s", title_buffer);
                ImGui::SameLine(ImGui::GetWindowWidth() - 100); // Move to right side
                
                // Delete Game button (red styling, slightly bigger for better readability)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.0f, 0.0f, 1.0f));
                if (ImGui::Button("Delete Game", ImVec2(90, 28))) {
                    // Confirm deletion with user
                    ImGui::OpenPopup("Confirm Delete Game");
                }
                ImGui::PopStyleColor(3);
                
                ImGui::Separator();
                
                // Display additional game information
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Alternate Title ID: %s", game->alternate_title_id ? game->alternate_title_id : "N/A");
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Time Date: %s", game->time_date ? game->time_date : "N/A");
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Disc Number: %s", game->disc_number ? game->disc_number : "N/A");
                
                ImGui::Separator();
                
                // Confirmation dialog for game deletion
                if (ImGui::BeginPopupModal("Confirm Delete Game", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::Text("Are you sure you want to delete this game and all its patches?");
                    ImGui::Text("Game: %s", game->game_title);
                    ImGui::Text("This action cannot be undone.");
                    ImGui::Dummy(g_viewport_mgr.Scale(ImVec2(0, 10)));
                    
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
                    if (ImGui::Button("Delete Game", ImVec2(100, 0))) {
                        if (xemu_patches_remove_game(m_selected_game_index)) {
                            xemu_patches_on_ui_database_changed();
                            m_show_game_details_window = false; // Close the details window
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::PopStyleColor(2);
                    
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
                
                // Confirmation dialog for patch deletion from context menu
                if (ImGui::BeginPopupModal("Confirm Delete Patch", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::Text("Are you sure you want to delete this patch?");
                    if (m_patch_to_delete >= 0 && m_patch_to_delete < game->patch_count) {
                        XemuMemoryPatch *patch = &game->patches[m_patch_to_delete];
                        ImGui::Text("Patch: %s", patch->name ? patch->name : "Untitled");
                    }
                    ImGui::Text("This action cannot be undone.");
                    ImGui::Dummy(g_viewport_mgr.Scale(ImVec2(0, 10)));
                    
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
                    if (ImGui::Button("Delete Patch", ImVec2(100, 0))) {

                        
                        if (m_patch_to_delete >= 0) {

                            
                            bool delete_result = xemu_patches_remove_patch(m_selected_game_index, m_patch_to_delete);

                            
                            if (delete_result) {

                                fflush(stdout);
                                xemu_patches_on_ui_database_changed();
                                m_patch_to_delete = -1; // Reset the index
                                ImGui::CloseCurrentPopup();
                                printf("Patch deletion completed successfully!\n");
                                fflush(stdout);
                            } else {

                                fflush(stdout);
                                xemu_queue_notification("Failed to delete patch - check logs for details");
                            }
                        } else {

                            fflush(stdout);
                            m_patch_to_delete = -1;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::PopStyleColor(2);
                    
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                        m_patch_to_delete = -1; // Reset the index
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
                
                ImGui::Dummy(g_viewport_mgr.Scale(ImVec2(0, 15)));
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.0f, 1.0f), "Patch Management");
                ImGui::Separator();
                
                // Green "Add Patch" button
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.7f, 0.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.8f, 0.1f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.6f, 0.0f, 1.0f));
                if (ImGui::Button("Add Patch", ImVec2(120, 0))) {
                    // Clear form for adding new patch
                    m_patch_name[0] = '\0';
                    m_patch_category[0] = '\0';
                    m_patch_author[0] = '\0';
                    m_patch_notes[0] = '\0';
                    m_patch_address_value_pairs[0] = '\0';
                    m_save_replaced_values = false; // Default: don't save replaced values
                    m_editing_patch_index = -1;
                    m_edit_error_message[0] = '\0'; // Clear any previous errors
                    m_show_edit_patch_dialog = true;
                }
                ImGui::PopStyleColor(3);
                
                // Move Total and Active patches here
                ImGui::Text("Total Patches: %d", game->patch_count);
                ImGui::SameLine();
                
                // Calculate active patches count
                int active_patches = 0;
                for (int i = 0; i < game->patch_count; i++) {
                    if (game->patches[i].enabled) {
                        active_patches++;
                    }
                }
                
                ImGui::Text("  |  Active Patches: %d", active_patches);
                
                ImGui::Dummy(g_viewport_mgr.Scale(ImVec2(0, 10)));
                
                // Display patches in a sortable table with actions
                if (game->patch_count == 0) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No patches defined for this game.");
                } else {
                    // Table for patches with interactive elements
                    if (ImGui::BeginTable("PatchesTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable)) {
                        ImGui::TableSetupColumn("Patch Name", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                        ImGui::TableHeadersRow();
                        
                        // REMOVED: unused for loop with unused patch variable

                        
                        // Handle sorting
                        ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs();
                        if (sort_specs->SpecsDirty && sort_specs->SpecsCount > 0) {
                            const ImGuiTableColumnSortSpecs* sort_spec = &sort_specs->Specs[0];
                            int column = sort_spec->ColumnIndex;
                            bool ascending = sort_spec->SortDirection == ImGuiSortDirection_Ascending;
                            

                            

                            
                            // Sort patches array using stable sort
                            if (game->patch_count > 1) {
                                std::vector<int> indices(game->patch_count);
                                for (int i = 0; i < game->patch_count; i++) {
                                    indices[i] = i;
                                }
                                
                                // Sort indices based on patch data using stable sort
                                std::stable_sort(indices.begin(), indices.end(),
                                    [column, ascending, game](int a, int b) {
                                        XemuMemoryPatch& pa = game->patches[a];
                                        XemuMemoryPatch& pb = game->patches[b];
                                        int result = 0;
                                        
                                        switch (column) {
                                            case 0: // Patch Name
                                                result = strcmp(pa.name ? pa.name : "", 
                                                               pb.name ? pb.name : "");
                                                break;
                                            case 1: // Author
                                                result = strcmp(pa.author ? pa.author : "", 
                                                               pb.author ? pb.author : "");
                                                break;
                                            case 2: // Category
                                                result = strcmp(pa.category ? pa.category : "", 
                                                               pb.category ? pb.category : "");
                                                break;
                                            case 3: // Status (enabled/disabled)
                                                result = (pa.enabled ? 1 : 0) - (pb.enabled ? 1 : 0);
                                                break;
                                            default:
                                                result = 0;
                                                break;
                                        }
                                        
                                        return ascending ? result < 0 : result > 0;
                                    });
                                
                                // Apply the sorted order to the patches array
                                std::vector<XemuMemoryPatch> sorted_patches(game->patch_count);
                                for (int i = 0; i < game->patch_count; i++) {
                                    sorted_patches[i] = game->patches[indices[i]];
                                }
                                
                                // Copy sorted patches back to original array
                                for (int i = 0; i < game->patch_count; i++) {
                                    game->patches[i] = sorted_patches[i];
                                }
                                

                            }
                            
                            // Debug: Show patches after sorting

                            for (int i = 0; i < game->patch_count; i++) {
 

                            }

                            
                            // Clear the dirty flag
                            sort_specs->SpecsDirty = false;
                        }
                        
                        for (int i = 0; i < game->patch_count; i++) {
                            XemuMemoryPatch *patch = &game->patches[i];
                            
                            ImGui::TableNextRow();
                            
                            // Patch Name (with notes tooltip and right-click context menu)
                            ImGui::TableSetColumnIndex(0);
                            ImGui::PushID(i * 10); // Unique ID for patch context menu
                            
                            // Right-click context menu for patch
                            if (ImGui::BeginPopupContextItem("PatchContextMenu")) {
                                if (ImGui::MenuItem("Edit Patch")) {
                                    // Set editing state for this patch

                                    fflush(stdout);
                                    m_editing_patch_index = i;
                                }
                                if (ImGui::MenuItem("Delete Patch")) {
                                    // Store the patch index for deletion
                                    m_patch_to_delete = i;
                                    // Open confirmation dialog
                                    ImGui::OpenPopup("Confirm Delete Patch");
                                    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                                }
                                ImGui::EndPopup();
                            }
                            
                            ImGui::Text("%s", patch->name ? patch->name : "Untitled");
                            ImGui::PopID();
                            
                            // Author
                            ImGui::TableSetColumnIndex(1);
                            ImGui::Text("%s", patch->author ? patch->author : "Unknown");
                            
                            // Category
                            ImGui::TableSetColumnIndex(2);
                            ImGui::Text("%s", patch->category ? patch->category : "General");
                            
                            // Status with toggle
                            ImGui::TableSetColumnIndex(3);
                            bool patch_enabled = patch->enabled;
                            
                            // Patch checkbox
                            
                            ImGui::PushID(i);
                            if (ImGui::Checkbox("##enabled", &patch_enabled)) {
                                xemu_patches_set_patch_enabled(m_selected_game_index, i, patch_enabled);
                                xemu_patches_on_ui_database_changed();
                            }
                            ImGui::PopID();
                            
                            // Actions
                            ImGui::TableSetColumnIndex(4);
                            ImGui::PushID(i * 2);
                            if (ImGui::Button("Edit")) {
                                
                                // Load patch data into edit fields
                                strncpy(m_patch_name, patch->name ? patch->name : "", sizeof(m_patch_name) - 1);
                                m_patch_name[sizeof(m_patch_name) - 1] = '\0';
                                trim_string(m_patch_name);
                                strncpy(m_patch_category, patch->category ? patch->category : "", sizeof(m_patch_category) - 1);
                                m_patch_category[sizeof(m_patch_category) - 1] = '\0';
                                strncpy(m_patch_author, patch->author ? patch->author : "", sizeof(m_patch_author) - 1);
                                m_patch_author[sizeof(m_patch_author) - 1] = '\0';
                                strncpy(m_patch_notes, patch->notes ? patch->notes : "", sizeof(m_patch_notes) - 1);
                                m_patch_notes[sizeof(m_patch_notes) - 1] = '\0';
                                
                                m_save_replaced_values = patch->save_replaced_values;
                                m_patch_address_value_pairs[0] = '\0';
                                
                                // If we have original address:value lines (with comments), use those
                                if (patch->address_value_lines && patch->address_value_lines_count > 0) {
                                    for (int j = 0; j < patch->address_value_lines_count; j++) {
                                        if (j >= 0 && j < patch->address_value_lines_count && patch->address_value_lines[j]) {
                                            size_t line_len = strlen(patch->address_value_lines[j]);
                                            
                                            if (line_len > 0) {
                                                size_t current_len = strlen(m_patch_address_value_pairs);
                                                size_t max_space = sizeof(m_patch_address_value_pairs) - current_len - 2;
                                                
                                                if (max_space > 0) {
                                                    char *source_line = patch->address_value_lines[j];
                                                    
                                                    char trimmed_line[512];
                                                    strncpy(trimmed_line, source_line, sizeof(trimmed_line) - 1);
                                                    trimmed_line[sizeof(trimmed_line) - 1] = '\0';
                                                    trim_string(trimmed_line);
                                                    
                                                    size_t trimmed_len = strlen(trimmed_line);
                                                    size_t copy_len = (trimmed_len < max_space) ? trimmed_len : max_space - 1;
                                                    memcpy(m_patch_address_value_pairs + current_len, trimmed_line, copy_len);
                                                    m_patch_address_value_pairs[current_len + copy_len] = '\0';
                                                    
                                                    if (current_len + copy_len < sizeof(m_patch_address_value_pairs) - 2) {
                                                        strncat(m_patch_address_value_pairs, "\n", 
                                                               sizeof(m_patch_address_value_pairs) - strlen(m_patch_address_value_pairs) - 1);
                                                    }
                                                }
                                            }
                                        }
                                    }
                                } else {
                                    // Fallback to rebuilding from parsed data (legacy patches)
                                    for (int j = 0; j < patch->address_value_count; j++) {
                                        if (j >= 0 && j < patch->address_value_count && patch->address_values) {
                                            char hex_value[256] = "";
                                            size_t hex_pos = 0;
                                            
                                            for (int k = 0; k < patch->address_values[j].value_length && k < 64; k++) {
                                                char hex_byte[4];
                                                int written = snprintf(hex_byte, sizeof(hex_byte), "%02X", patch->address_values[j].value_data[k]);
                                                if (written > 0 && hex_pos + written < sizeof(hex_value) - 1) {
                                                    strcpy(hex_value + hex_pos, hex_byte);
                                                    hex_pos += written;
                                                }
                                            }
                                            hex_value[hex_pos] = '\0';
                                            
                                            char pair_line[300];
                                            int written = snprintf(pair_line, sizeof(pair_line), "0x%08X:%s\n", 
                                                                   patch->address_values[j].address, hex_value);
                                            if (written > 0) {
                                                size_t current_len = strlen(m_patch_address_value_pairs);
                                                size_t max_space = sizeof(m_patch_address_value_pairs) - current_len - 1;
                                                size_t pair_len = strlen(pair_line);
                                                
                                                if (pair_len < max_space) {
                                                    strncat(m_patch_address_value_pairs, pair_line, max_space);
                                                }
                                            }
                                        }
                                    }
                                }
                                
                                m_editing_patch_index = i;
                                m_show_edit_patch_dialog = true;
                            }
                            ImGui::PopID();
                            
                            // Delete button
                            ImGui::SameLine();
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0, 0, 1));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0, 0, 1));
                            ImGui::PushID(i * 2 + 1);
                            if (ImGui::Button("Delete")) {
                                // Validate indices and pointers before deletion
                                if (m_selected_game_index < 0 || m_selected_game_index >= xemu_patches_get_game_count()) {
                                    xemu_queue_error_message("Invalid game index for patch deletion");
                                } else if (i < 0 || i >= games[m_selected_game_index].patch_count) {
                                    xemu_queue_error_message("Invalid patch index for deletion");
                                } else {
                                    bool result = xemu_patches_remove_patch(m_selected_game_index, i);
                                    
                                    if (result) {
                                        xemu_patches_on_ui_database_changed();
                                        i--; // Adjust index after removal
                                    } else {
                                        xemu_queue_error_message("Failed to delete patch - check xemu.log for details");
                                    }
                                }
                            }
                            ImGui::PopID();
                            ImGui::PopStyleColor(2);
                        }
                        ImGui::EndTable();
                    }
                }
                

                

                
                ImGui::Dummy(g_viewport_mgr.Scale(ImVec2(0, 15)));
                
                if (ImGui::Button("Close", ImVec2(80, 0))) {
                    m_show_game_details_window = false;
                }
                
                ImGui::End(); // Close Game Details window
            }
        }
    }


// Comprehensive patch validation function
bool MainMenuPatchesView::ValidatePatchData(const char* patch_name, const char* patch_category, 
                                           const char* address_value_pairs, char* error_msg, 
                                           size_t error_msg_size, bool is_edit, int edit_index,
                                           XemuGamePatches* games, int selected_game_index) {
    error_msg[0] = '\0'; // Clear error message
    
    // Check required fields
    if (strlen(patch_name) == 0) {
        snprintf(error_msg, error_msg_size, "Error: Patch name is required");
        return false;
    }
    
    if (strlen(patch_category) == 0) {
        snprintf(error_msg, error_msg_size, "Error: Category is required");
        return false;
    }
    
    if (strlen(address_value_pairs) == 0) {
        snprintf(error_msg, error_msg_size, "Error: At least one address:value pair is required");
        return false;
    }
    
    // Check for duplicate patch name within the same game
    if (games && selected_game_index >= 0) {
        XemuGamePatches* current_game = &games[selected_game_index];
        for (int i = 0; i < current_game->patch_count; i++) {
            // For edit mode, exclude the current patch being edited
            if (is_edit && i == edit_index) {
                continue;
            }
            
            if (current_game->patches[i].name && strcmp(current_game->patches[i].name, patch_name) == 0) {
                snprintf(error_msg, error_msg_size, "Error: Patch name '%s' already exists for this game", patch_name);
                return false;
            }
        }
    }
    
    // Comprehensive syntax validation for address:value pairs
    const char* text = address_value_pairs;
    int line_count = 0;

    
    while (*text) {
        // Skip empty lines and whitespace
        while (*text && isspace(*text)) text++;
        if (!*text) break;
        
        // Find end of line
        const char* line_end = text;
        while (*line_end && *line_end != '\n' && *line_end != '\r') {
            line_end++;
        }
        
        // Find comment markers and colon within this line
        const char* colon = strchr(text, ':');
        const char* hash = strchr(text, '#');
        const char* slash_slash = strstr(text, "//");
        
        // Find the earliest comment marker to determine content end
        const char* comment_start = line_end;
        if (hash && hash < comment_start) comment_start = hash;
        if (slash_slash && slash_slash < comment_start) comment_start = slash_slash;
        
        // Check if line has any content before comments
        const char* content_end = comment_start;
        bool is_comment_only = true;
        for (const char* p = text; p < content_end; p++) {
            if (!isspace(*p)) {
                is_comment_only = false;
                break;
            }
        }
        
        if (is_comment_only) {
            text = line_end;
            if (*text) text++; // Skip line break
            continue;
        }
        
        // Check if line contains colon
        if (!colon || colon >= content_end) {
            snprintf(error_msg, error_msg_size, "Error: Invalid syntax at line %d - missing colon separator", line_count + 1);
            return false;
        }
        
        // Check if colon is after any comment marker
        if ((hash && hash < colon) || (slash_slash && slash_slash < colon)) {
            snprintf(error_msg, error_msg_size, "Error: Invalid syntax at line %d - colon must be before comment marker", line_count + 1);
            return false;
        }
        
        // Validate memory address format (before colon)
        const char* addr_start = text;
        const char* addr_end = colon;
        bool addr_valid = false;
        
        // Skip leading whitespace in address
        while (addr_start < addr_end && isspace(*addr_start)) {
            addr_start++;
        }
        
        // Skip trailing whitespace in address
        const char* addr_trimmed_end = addr_end;
        while (addr_trimmed_end > addr_start && isspace(*(addr_trimmed_end - 1))) {
            addr_trimmed_end--;
        }
        
        // Check if address starts with 0x or 0X
        if (addr_trimmed_end - addr_start >= 2 && addr_start[0] == '0' && (addr_start[1] == 'x' || addr_start[1] == 'X')) {
            addr_valid = true;
            // Check remaining characters are valid hex
            for (const char* p = addr_start + 2; p < addr_trimmed_end; p++) {
                if (!isxdigit(*p)) {
                    snprintf(error_msg, error_msg_size, "Error: Invalid character '%c' in address at line %d - only hexadecimal digits allowed", *p, line_count + 1);
                    addr_valid = false;
                    break;
                }
            }
        }
        
        if (!addr_valid) {
            snprintf(error_msg, error_msg_size, "Error: Invalid address format at line %d - addresses must start with 0x and contain only hexadecimal digits", line_count + 1);
            return false;
        }
        
        // Validate value format (after colon, before comment)
        const char* value_start = colon + 1;
        const char* value_end = content_end;
        
        // Skip leading whitespace in value
        while (value_start < value_end && isspace(*value_start)) {
            value_start++;
        }
        
        // Skip trailing whitespace in value
        const char* value_trimmed_end = value_end;
        while (value_trimmed_end > value_start && isspace(*(value_trimmed_end - 1))) {
            value_trimmed_end--;
        }
        
        // Check if value has any content
        bool has_value_content = false;
        for (const char* p = value_start; p < value_trimmed_end; p++) {
            if (!isspace(*p)) {
                has_value_content = true;
                break;
            }
        }
        
        if (!has_value_content) {
            snprintf(error_msg, error_msg_size, "Error: Missing value at line %d", line_count + 1);
            return false;
        }
        
        // Check for invalid characters in value (only allow hex digits)
        for (const char* p = value_start; p < value_trimmed_end; p++) {
            if (!isxdigit(*p)) {
                snprintf(error_msg, error_msg_size, "Error: Invalid character '%c' in value at line %d - only hexadecimal digits allowed (comments are allowed after the value)", *p, line_count + 1);
                return false;
            }
        }
        
        // Count this valid line
        line_count++;
        
        // Move to next line
        text = line_end;
        if (*text) text++; // Skip line break
    }
    
    if (line_count == 0) {
        snprintf(error_msg, error_msg_size, "Error: No valid address:value pairs found");
        return false;
    }
    
    return true;
}

void MainMenuPatchesView::DrawEditPatchDialog(void)
{
    // Validate state before proceeding
    if (!m_show_edit_patch_dialog) {
        return;
    }
    
    int game_count = xemu_patches_get_game_count();
    if (game_count <= 0 || m_selected_game_index < 0 || m_selected_game_index >= game_count) {
        // Reset invalid state
        m_show_edit_patch_dialog = false;
        m_selected_game_index = -1;
        m_editing_patch_index = -1;
        return;
    }
    
    XemuGamePatches *games = xemu_patches_get_games();
    if (!games || game_count == 0) {
        return;
    }
    
    XemuGamePatches *game = &games[m_selected_game_index];
    
    // Dynamic window title
    const char* window_title = (m_editing_patch_index >= 0) ? "Edit Patch" : "Add Patch";
    
    // Show patch dialog (smaller horizontally)
    ImGui::SetNextWindowSize(g_viewport_mgr.Scale(ImVec2(600, 750)), ImGuiCond_FirstUseEver);
    
    if (ImGui::Begin(window_title, &m_show_edit_patch_dialog, ImGuiWindowFlags_NoSavedSettings)) {
        // Show game info at the top
        char game_info[256];
        snprintf(game_info, sizeof(game_info), "%s (%s - %s) (%s)", 
                game->game_title, game->title_id, game->region, game->version);
        ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.0f, 1.0f), "%s", game_info);
        ImGui::Separator();
        
        // Patch Name (Required)
        ImGui::Text("Patch Name (Required):");
        ImGui::SetNextItemWidth(400.0f);
        if (ImGui::InputText("##PatchName", m_patch_name, sizeof(m_patch_name), ImGuiInputTextFlags_AllowTabInput)) {
        }
        
        if (strlen(m_patch_name) == 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Patch name is required");
        }
        
        ImGui::Spacing();
        
        // Author (Optional)
        ImGui::Text("Author (Optional):");
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::InputText("##Author", m_patch_author, sizeof(m_patch_author))) {
        }
        
        ImGui::Spacing();
        
        // Category (Required)
        ImGui::Text("Category (Required):");
        ImGui::SetNextItemWidth(250.0f);
        if (ImGui::BeginCombo("##Category", m_patch_category[0] ? m_patch_category : "Select Category", ImGuiComboFlags_HeightSmall)) {
            const char* categories[] = {"Cheat", "Enhancement", "Widescreen", "Other"};
            for (int i = 0; i < IM_ARRAYSIZE(categories); i++) {
                if (ImGui::Selectable(categories[i])) {
                    strncpy(m_patch_category, categories[i], sizeof(m_patch_category) - 1);
                    m_patch_category[sizeof(m_patch_category) - 1] = '\0';
                }
            }
            ImGui::EndCombo();
        }
        if (strlen(m_patch_category) == 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Category is required");
        }
        
        ImGui::Spacing();
        
        // Memory Addresses (Required)
        ImGui::Text("Memory Addresses (Required):");
        ImGui::TextDisabled("Format: 0x00000000:00000000 (one per line)");
        ImGui::TextDisabled("Use # or // for comments");
        
        ImGui::SetNextItemWidth(500.0f);
        
        ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput;
        ImGui::InputTextMultiline("##AddressValue", m_patch_address_value_pairs, 
                                  sizeof(m_patch_address_value_pairs), 
                                  ImVec2(-FLT_MIN, 150), flags);

        
        ImGui::Spacing();
        
        // Save Replaced Values option
        if (ImGui::Checkbox("##SaveReplacedValuesEdit", &m_save_replaced_values)) {
        }
        ImGui::SameLine();
        ImGui::Text("Save Replaced Values (enables undo/redo functionality)");
        ImGui::TextDisabled("When enabled: Saves original memory values before applying patch");
        ImGui::TextDisabled("When disabled: Memory is not restored when patch is disabled");
        
        ImGui::Spacing();
        
        // Real-time validation feedback
        if (strlen(m_patch_address_value_pairs) > 0) {
            // Perform real-time validation
            char validation_error[256] = "";
            bool validation_result = ValidatePatchData(m_patch_name, m_patch_category, m_patch_address_value_pairs, 
                                                     validation_error, sizeof(validation_error), 
                                                     (m_editing_patch_index >= 0), m_editing_patch_index, games, m_selected_game_index);
            
            if (validation_result) {
                // Count valid lines for display
                const char* text = m_patch_address_value_pairs;
                int line_count = 0;
                
                while (*text) {
                    // Skip empty lines and whitespace
                    while (*text && isspace(*text)) text++;
                    if (!*text) break;
                    
                    // Check if line contains colon (basic requirement for counting)
                    const char* colon = strchr(text, ':');
                    if (colon) {
                        line_count++;
                    }
                    
                    // Move to next line
                    text = strchr(text, '\n');
                    if (!text) break;
                    text++;
                }
                
                ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.0f, 1.0f), "Format preview: %d line(s) with address:value pairs", line_count);
                // Clear any previous validation errors on success
                if (m_edit_error_message[0] != '\0') {
                    m_edit_error_message[0] = '\0';
                }
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Format preview: Validation error found");
                // Update class error message for display below
                snprintf(m_edit_error_message, sizeof(m_edit_error_message), "%s", validation_error);
            }
            
            // Display validation error under format preview if any
            if (m_edit_error_message[0] != '\0') {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", m_edit_error_message);
            }
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "At least one address:value pair is required");
        }
        
        ImGui::Spacing();
        
        // Patch Notes (Optional)
        ImGui::Text("Patch Notes (Optional):");
        ImGui::SetNextItemWidth(550.0f);
        
        // Create a multi-line text input with horizontal scroll bar
        if (ImGui::InputTextMultiline("##PatchNotes", m_patch_notes, sizeof(m_patch_notes), 
                                      ImVec2(-FLT_MIN, 60), 
                                      0)) {
        }
        
        ImGui::Dummy(g_viewport_mgr.Scale(ImVec2(0, 10)));
        
        ImGui::Spacing();
        
        // Action buttons
        if (m_editing_patch_index >= 0) {
            // Update existing patch
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.7f, 0.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.8f, 0.0f, 1.0f));
            // Declare validation error buffer outside the if-else blocks
            char validation_error[256];
            bool validation_result = false;
            
            if (ImGui::Button("Update Patch", ImVec2(120, 0))) {

                
                // Use comprehensive validation
                validation_result = ValidatePatchData(m_patch_name, m_patch_category, m_patch_address_value_pairs, 
                                    validation_error, sizeof(validation_error), true, m_editing_patch_index, games, m_selected_game_index);
                if (validation_result) {

                    fflush(stdout);
                    

                    

                    
                    bool update_result = xemu_patches_update_patch(m_selected_game_index, m_editing_patch_index,
                                                m_patch_name, m_patch_category, m_patch_author,
                                                m_patch_notes, m_patch_address_value_pairs, m_save_replaced_values);
                    

                    
                    if (update_result) {

                        xemu_patches_on_ui_database_changed();
                        
    
                        fflush(stdout);
                        
                        // Clear form
                        m_patch_name[0] = '\0';
                        m_patch_category[0] = '\0';
                        m_patch_author[0] = '\0';
                        m_patch_notes[0] = '\0';
                        m_patch_address_value_pairs[0] = '\0';
                        m_save_replaced_values = false; // Default: don't save replaced values
                        m_editing_patch_index = -1;
                        m_edit_error_message[0] = '\0'; // Clear edit error message on success
                        m_show_edit_patch_dialog = false;
                        

                        fflush(stdout);
                        xemu_queue_notification("Patch updated successfully");
                    } else {

                        fflush(stdout);
                        xemu_queue_notification("Failed to update patch - check logs for details");
                    }
                } else {
                    // Validation failed, display error message

                    fflush(stdout);
                    snprintf(m_edit_error_message, sizeof(m_edit_error_message), "%s", validation_error);
                } // End of validation check
            } // End of button click if block
            else {
                // Button not clicked, ensure validation_result is false
                validation_result = false;
            }
            ImGui::PopStyleColor(2);
        } else {
            // Add new patch (when m_editing_patch_index < 0)
            // Add new patch button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.7f, 0.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.8f, 0.0f, 1.0f));
            
            // Declare validation error buffer outside the if-else blocks
            char validation_error[256];
            bool validation_result = false;
            
            if (ImGui::Button("Add Patch", ImVec2(120, 0))) {
                validation_result = ValidatePatchData(m_patch_name, m_patch_category, m_patch_address_value_pairs, 
                                    validation_error, sizeof(validation_error), false, -1, games, m_selected_game_index);
                if (validation_result) {
                    bool add_result = xemu_patches_add_patch(m_selected_game_index, m_patch_name, m_patch_category, m_patch_author, m_patch_notes, m_patch_address_value_pairs, m_save_replaced_values);
                    
                    if (add_result) {
                        xemu_patches_on_ui_database_changed();
                        xemu_queue_notification("Patch added successfully");
                        
                        // Clear fields
                        m_patch_name[0] = '\0';
                        m_patch_category[0] = '\0';
                        m_patch_author[0] = '\0';
                        m_patch_notes[0] = '\0';
                        m_patch_address_value_pairs[0] = '\0';
                        m_save_replaced_values = false;
                        m_editing_patch_index = -1;
                        m_edit_error_message[0] = '\0';
                        m_show_edit_patch_dialog = false;
                        
                        xemu_queue_notification("Patch added successfully");
                    } else {
                        xemu_queue_notification("Failed to add patch - check logs for details");
                    }
                } else {
                    snprintf(m_edit_error_message, sizeof(m_edit_error_message), "%s", validation_error);
                }
            } else {
                validation_result = false;
            }
            ImGui::PopStyleColor(2);
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(80, 0))) {
            m_show_edit_patch_dialog = false;
            m_editing_patch_index = -1;
            m_patch_name[0] = '\0';
            m_patch_category[0] = '\0';
            m_patch_author[0] = '\0';
            m_patch_notes[0] = '\0';
            m_patch_address_value_pairs[0] = '\0';
            m_save_replaced_values = false; // Default: don't save replaced values
            m_edit_error_message[0] = '\0'; // Clear edit error message
        }
        
    }
    
    ImGui::End();
}

// Implementation of CloseAddGameDialog method for MainMenuScene
void MainMenuScene::CloseAddGameDialog()
{
    m_patches_view.CloseAddGameDialog();
}



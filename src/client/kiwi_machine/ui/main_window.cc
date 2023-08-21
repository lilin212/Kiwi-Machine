// Copyright (C) 2023 Yisi Yu
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "ui/main_window.h"

#include <gflags/gflags.h>
#include <filesystem>
#include <queue>

#include "base/strings/string_number_conversions.h"
#include "debug/debug_roms.h"
#include "preset_roms/preset_roms.h"
#include "resources/image_resources.h"
#include "ui/application.h"
#include "ui/widgets/about_widget.h"
#include "ui/widgets/canvas.h"
#include "ui/widgets/controller_widget.h"
#include "ui/widgets/disassembly_widget.h"
#include "ui/widgets/export_widget.h"
#include "ui/widgets/frame_rate_widget.h"
#include "ui/widgets/group_widget.h"
#include "ui/widgets/kiwi_bg_widget.h"
#include "ui/widgets/kiwi_items_widget.h"
#include "ui/widgets/loading_widget.h"
#include "ui/widgets/memory_widget.h"
#include "ui/widgets/nametable_widget.h"
#include "ui/widgets/palette_widget.h"
#include "ui/widgets/pattern_widget.h"
#include "ui/widgets/stack_widget.h"
#include "ui/widgets/toast.h"
#include "utility/audio_effects.h"
#include "utility/key_mapping_util.h"
#include "utility/zip_reader.h"

namespace {

DEFINE_bool(has_menu, false, "Show a menu bar at the top of the window.");

kiwi::nes::Bytes ReadFromRawBinary(const kiwi::nes::Byte* data,
                                   size_t data_size) {
  kiwi::nes::Bytes bytes;
  bytes.resize(data_size);
  memcpy(bytes.data(), data, data_size);
  return bytes;
}

constexpr int kDefaultWindowWidth = Canvas::kNESFrameDefaultWidth;
constexpr int kDefaultWindowHeight = Canvas::kNESFrameDefaultHeight;
constexpr int kDefaultFontSize = 15;

const kiwi::base::RepeatingCallback<bool()> kNoCheck =
    kiwi::base::RepeatingCallback<bool()>();

int GetDefaultMenuHeight() {
  return kDefaultFontSize + ImGui::GetStyle().FramePadding.y * 2;
}

void FillLayout(WindowBase* window, Widget* widget) {
  SDL_Rect client_bounds = window->GetClientBounds();
  widget->set_bounds(SDL_Rect{0, 0, client_bounds.w, client_bounds.h});
}

void ToastGameControllersAddedOrRemoved(WindowBase* window,
                                        bool is_added,
                                        int which) {
  if (SDL_IsGameController(which)) {
    std::string name = SDL_GameControllerNameForIndex(which);
    Toast::ShowToast(
        window, name + (is_added ? " is connected." : " is disconnected."));
  }
}

bool ExportNES(const kiwi::base::FilePath& output_path,
               const std::string& title,
               const kiwi::nes::Byte* compressed_data,
               size_t uncompressed_raw_size,
               size_t compressed_data_size,
               const kiwi::nes::Byte* cover,
               size_t cover_size) {
  if (!std::filesystem::exists(output_path)) {
    std::filesystem::create_directories(output_path);
  }

  kiwi::nes::Bytes nes_raw_data = ReadFromZipBinary(
      compressed_data, uncompressed_raw_size, compressed_data_size);
  kiwi::base::FilePath nes_file_path = output_path.Append(title + ".nes");
  // Writes raw NES file.
  SDL_RWops* file_handle =
      SDL_RWFromFile(nes_file_path.AsUTF8Unsafe().c_str(), "w");
  if (!file_handle)
    return false;

  SDL_RWwrite(file_handle, nes_raw_data.data(), nes_raw_data.size(), 1);
  SDL_RWclose(file_handle);

  // Writes cover, if any.
  if (cover && cover_size > 0) {
    // Cover is always jpg.
    kiwi::base::FilePath cover_file_path = output_path.Append(title + ".jpg");
    SDL_RWops* cover_handle =
        SDL_RWFromFile(cover_file_path.AsUTF8Unsafe().c_str(), "w");
    if (!cover_handle)
      return false;

    SDL_RWwrite(cover_handle, cover, cover_size, 1);
    SDL_RWclose(cover_handle);
  }

  return true;
}

std::pair<bool, std::queue<preset_roms::PresetROM>> OnExportGameROM(
    const kiwi::base::FilePath& export_path,
    const std::queue<preset_roms::PresetROM>& roms_to_be_exported) {
  preset_roms::PresetROM rom = roms_to_be_exported.front();
  std::queue<preset_roms::PresetROM> remaining = roms_to_be_exported;
  remaining.pop();

  return std::make_pair(
      ExportNES(export_path, rom.name, rom.data, rom.raw_size,
                rom.compressed_size, rom.cover, rom.cover_size),
      remaining);
}

void OnGameROMExported(
    MainWindow* main_window,
    const kiwi::base::FilePath& export_path,
    const std::pair<bool, std::queue<preset_roms::PresetROM>>& result) {
  if (result.second.empty()) {
    // No more roms to be exported.
    main_window->ExportDone();
    return;
  }

  const std::string& next_rom_name = result.second.front().name;
  if (result.first) {
    main_window->ExportSucceeded(next_rom_name);
  } else {
    main_window->ExportFailed(next_rom_name);
  }

  scoped_refptr<kiwi::base::SequencedTaskRunner> io_task_runner =
      Application::Get()->GetIOTaskRunner();
  io_task_runner->PostTaskAndReplyWithResult(
      FROM_HERE,
      kiwi::base::BindOnce(&OnExportGameROM, export_path, result.second),
      kiwi::base::BindOnce(&OnGameROMExported, main_window, export_path));
}

}  // namespace

MainWindow::MainWindow(const std::string& title, NESRuntimeID runtime_id)
    : WindowBase(title) {
  Initialize(runtime_id);
  InitializeUI();
  InitializeIODevices();
}

MainWindow::~MainWindow() {
  SDL_assert(runtime_data_);
  SDL_assert(runtime_data_->emulator);
  SDL_assert(canvas_);
  runtime_data_->emulator->PowerOff();
  canvas_->RemoveObserver(this);
}

void MainWindow::ExportDone() {
  SDL_assert(export_widget_);
  export_widget_->Done();
}

void MainWindow::ExportSucceeded(const std::string& rom_name) {
  export_widget_->Succeeded(kiwi::base::FilePath::FromUTF8Unsafe(rom_name));
}

void MainWindow::ExportFailed(const std::string& rom_name) {
  export_widget_->Failed(kiwi::base::FilePath::FromUTF8Unsafe(rom_name));
}

SDL_Rect MainWindow::Scaled(const SDL_Rect& rect) {
  return SDL_Rect{
      static_cast<int>(rect.x * window_scale()),
      static_cast<int>(rect.y * window_scale()),
      static_cast<int>(rect.w * window_scale()),
      static_cast<int>(rect.h * window_scale()),
  };
}

ImVec2 MainWindow::Scaled(const ImVec2& vec2) {
  return ImVec2(vec2.x * window_scale(), vec2.y * window_scale());
}

int MainWindow::Scaled(int i) {
  return i * window_scale();
}

bool MainWindow::IsKeyDown(int controller_id,
                           kiwi::nes::ControllerButton button) {
  bool matched =
      pressing_keys_.find(runtime_data_->keyboard_mappings[controller_id]
                              .mapping[static_cast<int>(button)]) !=
      pressing_keys_.cend();

  if (matched)
    return true;

  // Key doesn't match. Try joysticks.
  NESRuntime::Data::JoystickMapping joystick_mapping =
      runtime_data_->joystick_mappings[controller_id];
  if (joystick_mapping.which) {
    // If mapping is available, cast it to game controller interface.
    SDL_GameController* game_controller =
        reinterpret_cast<SDL_GameController*>(joystick_mapping.which);
    matched = SDL_GameControllerGetButton(
        game_controller, static_cast<SDL_GameControllerButton>(
                             runtime_data_->joystick_mappings[controller_id]
                                 .mapping.mapping[static_cast<int>(button)]));

    if (matched)
      return true;

    // Not matched, try axis motion.
    constexpr Sint16 kDeadZoom = SDL_JOYSTICK_AXIS_MAX / 3;
    switch (button) {
      case kiwi::nes::ControllerButton::kLeft: {
        Sint16 x = SDL_GameControllerGetAxis(game_controller,
                                             SDL_CONTROLLER_AXIS_LEFTX);
        if (SDL_JOYSTICK_AXIS_MIN <= x && x <= -kDeadZoom)
          matched = true;
      } break;
      case kiwi::nes::ControllerButton::kRight: {
        Sint16 x = SDL_GameControllerGetAxis(game_controller,
                                             SDL_CONTROLLER_AXIS_LEFTX);
        if (kDeadZoom <= x && x <= SDL_JOYSTICK_AXIS_MAX)
          matched = true;
      } break;
      case kiwi::nes::ControllerButton::kUp: {
        Sint16 y = SDL_GameControllerGetAxis(game_controller,
                                             SDL_CONTROLLER_AXIS_LEFTY);
        if (SDL_JOYSTICK_AXIS_MIN <= y && y <= -kDeadZoom)
          matched = true;
      } break;
      case kiwi::nes::ControllerButton::kDown: {
        Sint16 y = SDL_GameControllerGetAxis(game_controller,
                                             SDL_CONTROLLER_AXIS_LEFTY);
        if (kDeadZoom <= y && y <= SDL_JOYSTICK_AXIS_MAX)
          matched = true;
      } break;
      default:
        break;
    }
  }

  return matched;
}

SDL_Rect MainWindow::GetClientBounds() {
  // Excludes menu bar's height;
  SDL_Rect render_bounds = WindowBase::GetClientBounds();
  if (menu_bar_) {
    if (menu_bar_->bounds().h > 0) {
      render_bounds.y += menu_bar_->bounds().h;
      render_bounds.h -= menu_bar_->bounds().h;
    } else {
      // Menu bar doesn't render yet. Use default value.
      render_bounds.y += GetDefaultMenuHeight();
      render_bounds.h -= GetDefaultMenuHeight();
    }
  }
  return render_bounds;
}

void MainWindow::HandleKeyEvents(SDL_KeyboardEvent* event) {
  if (!IsPause()) {
    // Do not handle emulator's key event when paused.
    if (event->type == SDL_KEYDOWN)
      pressing_keys_.insert(event->keysym.sym);
    else if (event->type == SDL_KEYUP)
      pressing_keys_.erase(event->keysym.sym);
  }

  WindowBase::HandleKeyEvents(event);
}

void MainWindow::OnControllerDeviceAdded(SDL_ControllerDeviceEvent* event) {
  ToastGameControllersAddedOrRemoved(this, true, event->which);
  UpdateGameControllerMapping();
}

void MainWindow::OnControllerDeviceRemoved(SDL_ControllerDeviceEvent* event) {
  ToastGameControllersAddedOrRemoved(this, false, event->which);
  UpdateGameControllerMapping();
}

void MainWindow::HandleResizedEvent() {
  if (bg_widget_) {
    SDL_Rect client_bounds = GetClientBounds();
    FillLayout(this, bg_widget_);
    FillLayout(this, main_group_widget_);
    FillLayout(this, stack_widget_);
  }

  if (in_game_menu_) {
    FillLayout(this, in_game_menu_);
  }

  if (is_fullscreen_) {
    // Calculate fullscreen's frame scale, and set.
    SDL_Rect client_bounds = GetClientBounds();
    window_scale_ = static_cast<float>(client_bounds.h) / kDefaultWindowWidth;
    OnScaleChanged();
  }
  WindowBase::HandleResizedEvent();
}

void MainWindow::HandlePostEvent() {
  SDL_assert(runtime_data_);
  runtime_data_->emulator->RunOneFrame();
}

void MainWindow::OnAboutToRenderFrame(Canvas* canvas,
                                      scoped_refptr<NESFrame> frame) {
  // Always adjusts the canvas to the middle of the render area (excludes menu
  // bar).
  SDL_Rect render_bounds = GetClientBounds();
  SDL_Rect src_rect = {0, 0, frame->width(), frame->height()};

  SDL_Rect dest_rect = {
      static_cast<int>((render_bounds.w - src_rect.w * canvas->frame_scale()) /
                       2) +
          render_bounds.x,
      static_cast<int>((render_bounds.h - src_rect.h * canvas->frame_scale()) /
                       2) +
          render_bounds.y,
      static_cast<int>(frame->width() * canvas->frame_scale()),
      static_cast<int>(frame->height() * canvas->frame_scale())};
  canvas->set_bounds(dest_rect);

  // Updates window size, to fit the frame.
  if (menu_bar_) {
    SDL_Rect menu_rect = menu_bar_->bounds();
    int desired_window_width = dest_rect.w;
    int desired_window_height = menu_rect.h + dest_rect.h;
    Resize(desired_window_width, desired_window_height);
  } else {
    Resize(dest_rect.w, dest_rect.h);
  }
}

void MainWindow::Initialize(NESRuntimeID runtime_id) {
  SDL_assert(runtime_id >= 0);
  runtime_id_ = runtime_id;
  runtime_data_ = NESRuntime::GetInstance()->GetDataById(runtime_id);

  audio_ = std::make_unique<NESAudio>(runtime_id_);
  audio_->Initialize();
  audio_->Start();

  SDL_assert(runtime_data_->debug_port);
  runtime_data_->debug_port->set_on_breakpoint_callback(
      kiwi::base::BindRepeating(&MainWindow::OnPause,
                                kiwi::base::Unretained(this)));
}

void MainWindow::ResetAudio() {
  audio_->Reset();
  if (runtime_data_->emulator && runtime_data_->emulator->GetIODevices()) {
    runtime_data_->emulator->GetIODevices()->set_audio_device(audio_.get());
  }
}

void MainWindow::InitializeUI() {
  if (FLAGS_has_menu) {
    // Menu bar
    std::unique_ptr<MenuBar> menu_bar = std::make_unique<MenuBar>(this);
    menu_bar_ = menu_bar.get();
    menu_bar_->set_flags(ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
    menu_bar->set_title("Kiwi Machine");
    std::vector<MenuBar::Menu> all_menu = GetMenuModel();
    for (const auto& menu : all_menu) {
      menu_bar->AddMenu(menu);
    }
    AddWidget(std::move(menu_bar));
  }

  // Background
  SDL_Rect client_bounds = GetClientBounds();
  std::unique_ptr<KiwiBgWidget> bg_widget =
      std::make_unique<KiwiBgWidget>(this);
  bg_widget_ = bg_widget.get();
  FillLayout(this, bg_widget_);

  // Stack widget
  std::unique_ptr<StackWidget> stack_widget =
      std::make_unique<StackWidget>(this);
  stack_widget_ = stack_widget.get();
  stack_widget_->set_bounds(client_bounds);

  // Main menu groups
  std::unique_ptr<GroupWidget> group_widget =
      std::make_unique<GroupWidget>(this, runtime_id_);
  main_group_widget_ = group_widget.get();
  FillLayout(this, main_group_widget_);
  stack_widget_->PushWidget(std::move(group_widget));
  bg_widget_->AddWidget(std::move(stack_widget));

  // Game items
  std::unique_ptr<KiwiItemsWidget> items_widget =
      std::make_unique<KiwiItemsWidget>(this, runtime_id_);

  for (int i = 0; i < sizeof(preset_roms::kPresetRoms) /
                          sizeof(preset_roms::kPresetRoms[0]);
       ++i) {
    items_widget->AddItem(
        preset_roms::kPresetRoms[i].name, preset_roms::kPresetRoms[i].cover,
        preset_roms::kPresetRoms[i].cover_size,
        kiwi::base::BindRepeating(&MainWindow::OnLoadPresetROM,
                                  kiwi::base::Unretained(this), i));
  }
  main_group_widget_->AddWidget(std::move(items_widget));

  // Game items (special)
  std::unique_ptr<KiwiItemsWidget> specials_item_widget =
      std::make_unique<KiwiItemsWidget>(this, runtime_id_);

  for (int i = 0; i < sizeof(preset_roms::specials::kPresetRoms) /
                          sizeof(preset_roms::specials::kPresetRoms[0]);
       ++i) {
    specials_item_widget->AddItem(
        preset_roms::specials::kPresetRoms[i].name,
        preset_roms::specials::kPresetRoms[i].cover,
        preset_roms::specials::kPresetRoms[i].cover_size,
        kiwi::base::BindRepeating(&MainWindow::OnLoadSpecialROM,
                                  kiwi::base::Unretained(this), i));
  }
  main_group_widget_->AddWidget(std::move(specials_item_widget));

  // About
  std::unique_ptr<KiwiItemsWidget> settings_widget =
      std::make_unique<KiwiItemsWidget>(this, runtime_id_);

  // Settings items
  std::unique_ptr<KiwiItemsWidget> controller_widget =
      std::make_unique<KiwiItemsWidget>(this, runtime_id_);
  settings_widget->AddItem(
      "Controller", image_resources::kJoystickLogo,
      image_resources::kJoystickLogoSize,
      kiwi::base::BindRepeating(
          [](MainWindow* window, StackWidget* stack_widget,
             NESRuntimeID runtime_id) {
            stack_widget->PushWidget(std::make_unique<ControllerWidget>(
                window, stack_widget, runtime_id));
          },
          this, stack_widget_, runtime_id_));

  settings_widget->AddItem(
      "Settings", image_resources::kSettingsLogo,
      image_resources::kSettingsLogoSize,
      kiwi::base::BindRepeating(
          [](MainWindow* window, StackWidget* stack_widget,
             NESRuntimeID runtime_id) {
            std::unique_ptr<InGameMenu> in_game_menu =
                std::make_unique<InGameMenu>(
                    window, runtime_id,
                    kiwi::base::BindRepeating(
                        [](StackWidget* stack_widget,
                           InGameMenu::MenuItem item) {
                          // Mapping button 'B' will trigger kContinue.
                          if (item == InGameMenu::MenuItem::kToGameSelection ||
                              item == InGameMenu::MenuItem::kContinue) {
                            stack_widget->PopWidget();
                          }
                        },
                        stack_widget),
                    kiwi::base::BindRepeating(
                        &MainWindow::OnInGameSettingsItemTrigger,
                        kiwi::base::Unretained(window)));
            in_game_menu->HideMenu(0);  // Hides 'Continue'
            in_game_menu->HideMenu(1);  // Hides 'Load State'
            in_game_menu->HideMenu(2);  // Hides 'Save State'
            FillLayout(window, in_game_menu.get());
            stack_widget->PushWidget(std::move(in_game_menu));
          },
          this, stack_widget_, runtime_id_));

  settings_widget->AddItem(
      "About Kiwi Machine", image_resources::kBackgroundLogo,
      image_resources::kBackgroundLogoSize,
      kiwi::base::BindRepeating(
          [](MainWindow* window, StackWidget* stack_widget,
             NESRuntimeID runtime_id) {
            stack_widget->PushWidget(std::make_unique<AboutWidget>(
                window, stack_widget, runtime_id));
          },
          this, stack_widget_, runtime_id_));

  settings_widget->AddItem(
      "Quit", image_resources::kExitLogo, image_resources::kExitLogoSize,
      kiwi::base::BindRepeating(&MainWindow::OnQuit,
                                kiwi::base::Unretained(this)));

  // End of settings items
  main_group_widget_->AddWidget(std::move(settings_widget));
  AddWidget(std::move(bg_widget));

  std::unique_ptr<Canvas> canvas = std::make_unique<Canvas>(this, runtime_id_);
  canvas_ = canvas.get();
  canvas_->set_visible(false);
  canvas_->AddObserver(this);
  canvas_->set_frame_scale(2.f);
  canvas_->set_in_menu_trigger_callback(kiwi::base::BindRepeating(
      &MainWindow::OnInGameMenuTrigger, kiwi::base::Unretained(this)));
  AddWidget(std::move(canvas));

  std::unique_ptr<InGameMenu> in_game_menu = std::make_unique<InGameMenu>(
      this, runtime_id_,
      kiwi::base::BindRepeating(&MainWindow::OnInGameMenuItemTrigger,
                                kiwi::base::Unretained(this)),
      kiwi::base::BindRepeating(&MainWindow::OnInGameSettingsItemTrigger,
                                kiwi::base::Unretained(this)));
  in_game_menu_ = in_game_menu.get();
  in_game_menu_->set_visible(false);
  AddWidget(std::move(in_game_menu));

  // Loading spinning widget.
  std::unique_ptr<LoadingWidget> loading_widget =
      std::make_unique<LoadingWidget>(this);
  loading_widget_ = loading_widget.get();
  loading_widget_->set_visible(false);
  AddWidget(std::move(loading_widget));

  // Debug widgets
  std::unique_ptr<PaletteWidget> palette_widget =
      std::make_unique<PaletteWidget>(this, runtime_data_->debug_port.get());
  palette_widget_ = palette_widget.get();
  palette_widget_->set_visible(false);
  AddWidget(std::move(palette_widget));

  std::unique_ptr<PatternWidget> pattern_widget =
      std::make_unique<PatternWidget>(this, runtime_data_->debug_port.get());
  pattern_widget_ = pattern_widget.get();
  pattern_widget_->set_visible(false);
  AddWidget(std::move(pattern_widget));

  std::unique_ptr<FrameRateWidget> frame_rate_widget =
      std::make_unique<FrameRateWidget>(this, canvas_->frame(),
                                        runtime_data_->debug_port.get());
  frame_rate_widget_ = frame_rate_widget.get();
  frame_rate_widget_->set_visible(false);
  AddWidget(std::move(frame_rate_widget));

  std::unique_ptr<ExportWidget> export_widget =
      std::make_unique<ExportWidget>(this);
  export_widget_ = export_widget.get();
  export_widget_->set_visible(false);
  AddWidget(std::move(export_widget));

  std::unique_ptr<MemoryWidget> memory_widget = std::make_unique<MemoryWidget>(
      this, runtime_id_,
      kiwi::base::BindRepeating(&MainWindow::OnTogglePause,
                                kiwi::base::Unretained(this)),
      kiwi::base::BindRepeating(&MainWindow::IsPause,
                                kiwi::base::Unretained(this)));
  memory_widget_ = memory_widget.get();
  memory_widget_->set_visible(false);
  AddWidget(std::move(memory_widget));

  std::unique_ptr<DisassemblyWidget> disassembly_widget =
      std::make_unique<DisassemblyWidget>(
          this, runtime_id_,
          kiwi::base::BindRepeating(&MainWindow::OnTogglePause,
                                    kiwi::base::Unretained(this)),
          kiwi::base::BindRepeating(&MainWindow::IsPause,
                                    kiwi::base::Unretained(this)));
  disassembly_widget_ = disassembly_widget.get();
  disassembly_widget_->set_visible(false);
  AddWidget(std::move(disassembly_widget));

  std::unique_ptr<NametableWidget> nametable_widget =
      std::make_unique<NametableWidget>(this, runtime_id_);
  nametable_widget_ = nametable_widget.get();
  nametable_widget_->set_visible(false);
  AddWidget(std::move(nametable_widget));

  // Set default scale, and move the window to the center.
  OnSetScreenScale(3.f);
  MoveToCenter();
}

void MainWindow::InitializeIODevices() {
  SDL_assert(runtime_data_);
  SDL_assert(runtime_data_->emulator);
  SDL_assert(canvas_);
  std::unique_ptr<kiwi::nes::IODevices> io_devices =
      std::make_unique<kiwi::nes::IODevices>();
  io_devices->set_input_device(this);
  io_devices->add_render_device(canvas_->render_device());
  io_devices->set_audio_device(audio_.get());
  runtime_data_->emulator->SetIODevices(std::move(io_devices));
}

std::vector<MenuBar::Menu> MainWindow::GetMenuModel() {
  std::vector<MenuBar::Menu> result;

  // Games
  {
    MenuBar::Menu games;
    games.title = "Games";
    games.menu_items.push_back(
        {"Reset ROM", kiwi::base::BindRepeating(&MainWindow::OnResetROM,
                                                kiwi::base::Unretained(this))});

    games.menu_items.push_back(
        {"Back To Main Menu",
         kiwi::base::BindRepeating(&MainWindow::OnBackToMainMenu,
                                   kiwi::base::Unretained(this))});

    games.menu_items.push_back(
        {"Quit", kiwi::base::BindRepeating(&MainWindow::OnQuit,
                                           kiwi::base::Unretained(this))});
    result.push_back(std::move(games));
  }

  // Emulator
  {
    MenuBar::Menu emulator;
    emulator.title = "Emulator";

    emulator.menu_items.push_back(
        {"Pause",
         kiwi::base::BindRepeating(&MainWindow::OnTogglePause,
                                   kiwi::base::Unretained(this)),
         kiwi::base::BindRepeating(&MainWindow::IsPause,
                                   kiwi::base::Unretained(this))});

    emulator.menu_items.push_back(
        {"Enable audio",
         kiwi::base::BindRepeating(&MainWindow::OnToggleAudioEnabled,
                                   kiwi::base::Unretained(this)),
         kiwi::base::BindRepeating(&MainWindow::IsAudioEnabled,
                                   kiwi::base::Unretained(this))});

    // Screen size
    {
      MenuBar::MenuItem screen_size;
      screen_size.title = "Screen size";
      for (int i = 2; i <= 4; ++i) {
        screen_size.sub_items.push_back(
            {kiwi::base::NumberToString(i) + "x",
             kiwi::base::BindRepeating(&MainWindow::OnSetScreenScale,
                                       kiwi::base::Unretained(this), i),
             kiwi::base::BindRepeating(&MainWindow::ScreenScaleIs,
                                       kiwi::base::Unretained(this), i)});
      }
      emulator.menu_items.push_back(std::move(screen_size));
    }

    // Save and load menu
    {
      MenuBar::MenuItem states;
      states.title = "States";
      states.sub_items.push_back(
          {"Save state",
           kiwi::base::BindRepeating(&MainWindow::OnSaveState,
                                     kiwi::base::Unretained(this)),
           kNoCheck,
           kiwi::base::BindRepeating(&MainWindow::CanSaveOrLoadState,
                                     kiwi::base::Unretained(this))});
      states.sub_items.push_back(
          {"Load state",
           kiwi::base::BindRepeating(&MainWindow::OnLoadState,
                                     kiwi::base::Unretained(this)),
           kNoCheck,
           kiwi::base::BindRepeating(&MainWindow::CanSaveOrLoadState,
                                     kiwi::base::Unretained(this))});
      emulator.menu_items.push_back(std::move(states));
    }

    result.push_back(std::move(emulator));
  }

  // Debug menu
  {
    MenuBar::Menu debug;
    debug.title = "Debug";

    {
      MenuBar::MenuItem debug_audio;
      debug_audio.title = "Audio";

      MenuBar::MenuItem debug_audio_square1 = {
          "Square 1",
          kiwi::base::BindRepeating(&MainWindow::OnToggleAudioChannelMasks,
                                    kiwi::base::Unretained(this),
                                    kiwi::nes::kSquare_1),
          kiwi::base::BindRepeating(&MainWindow::IsAudioChannelOn,
                                    kiwi::base::Unretained(this),
                                    kiwi::nes::kSquare_1)};
      MenuBar::MenuItem debug_audio_square2 = {
          "Square 2",
          kiwi::base::BindRepeating(&MainWindow::OnToggleAudioChannelMasks,
                                    kiwi::base::Unretained(this),
                                    kiwi::nes::kSquare_2),
          kiwi::base::BindRepeating(&MainWindow::IsAudioChannelOn,
                                    kiwi::base::Unretained(this),
                                    kiwi::nes::kSquare_2)};
      MenuBar::MenuItem debug_audio_triangle = {
          "Triangle",
          kiwi::base::BindRepeating(&MainWindow::OnToggleAudioChannelMasks,
                                    kiwi::base::Unretained(this),
                                    kiwi::nes::kTriangle),
          kiwi::base::BindRepeating(&MainWindow::IsAudioChannelOn,
                                    kiwi::base::Unretained(this),
                                    kiwi::nes::kTriangle)};
      MenuBar::MenuItem debug_audio_noise = {
          "Noise",
          kiwi::base::BindRepeating(&MainWindow::OnToggleAudioChannelMasks,
                                    kiwi::base::Unretained(this),
                                    kiwi::nes::kNoise),
          kiwi::base::BindRepeating(&MainWindow::IsAudioChannelOn,
                                    kiwi::base::Unretained(this),
                                    kiwi::nes::kNoise)};
      MenuBar::MenuItem debug_audio_dmc = {
          "DMC",
          kiwi::base::BindRepeating(&MainWindow::OnToggleAudioChannelMasks,
                                    kiwi::base::Unretained(this),
                                    kiwi::nes::kDMC),
          kiwi::base::BindRepeating(&MainWindow::IsAudioChannelOn,
                                    kiwi::base::Unretained(this),
                                    kiwi::nes::kDMC)};

      debug_audio.sub_items.push_back(std::move(debug_audio_square1));
      debug_audio.sub_items.push_back(std::move(debug_audio_square2));
      debug_audio.sub_items.push_back(std::move(debug_audio_triangle));
      debug_audio.sub_items.push_back(std::move(debug_audio_noise));
      debug_audio.sub_items.push_back(std::move(debug_audio_dmc));

      debug.menu_items.push_back(std::move(debug_audio));
    }

    if (HasDebugRoms()) {
      MenuBar::MenuItem debug_roms =
          CreateDebugRomsMenu(kiwi::base::BindRepeating(
              &MainWindow::OnLoadDebugROM, kiwi::base::Unretained(this)));
      debug.menu_items.push_back(std::move(debug_roms));
    }

    debug.menu_items.push_back(
        {"Palette",
         kiwi::base::BindRepeating(&MainWindow::OnTogglePaletteWidget,
                                   kiwi::base::Unretained(this)),
         kiwi::base::BindRepeating(&MainWindow::IsPaletteWidgetShown,
                                   kiwi::base::Unretained(this))});

    debug.menu_items.push_back(
        {"Patterns",
         kiwi::base::BindRepeating(&MainWindow::OnTogglePatternWidget,
                                   kiwi::base::Unretained(this)),
         kiwi::base::BindRepeating(&MainWindow::IsPatternWidgetShown,
                                   kiwi::base::Unretained(this))});

    debug.menu_items.push_back(
        {"Frame rate",
         kiwi::base::BindRepeating(&MainWindow::OnFrameRateWidget,
                                   kiwi::base::Unretained(this)),
         kiwi::base::BindRepeating(&MainWindow::IsFrameRateWidgetShown,
                                   kiwi::base::Unretained(this))});

    debug.menu_items.push_back(
        {"Memory", kiwi::base::BindRepeating(&MainWindow::OnDebugMemory,
                                             kiwi::base::Unretained(this))});

    debug.menu_items.push_back(
        {"Disassembly",
         kiwi::base::BindRepeating(&MainWindow::OnDebugDisassembly,
                                   kiwi::base::Unretained(this))});

    debug.menu_items.push_back(
        {"Nametable", kiwi::base::BindRepeating(&MainWindow::OnDebugNametable,
                                                kiwi::base::Unretained(this))});

    debug.menu_items.push_back(
        {"Export All Games",
         kiwi::base::BindRepeating(&MainWindow::OnExportGameROMs,
                                   kiwi::base::Unretained(this))});

    result.push_back(std::move(debug));
  }

  return result;
}

void MainWindow::SetLoading(bool is_loading) {
  SDL_assert(bg_widget_);
  SDL_assert(loading_widget_);
  bg_widget_->SetLoading(is_loading);
  loading_widget_->set_visible(is_loading);
}

void MainWindow::ShowMainMenu(bool show) {
  SDL_assert(bg_widget_);
  SDL_assert(canvas_);
  canvas_->set_visible(!show);
  bg_widget_->set_visible(show);
  SetLoading(false);
}

void MainWindow::OnScaleChanged() {
  if (!is_fullscreen_) {
    const int default_menu_height = GetDefaultMenuHeight();
    if (menu_bar_) {
      if (menu_bar_->bounds().h > 0) {
        // Menu bar is painted, we can get exact menu height here.
        Resize(kDefaultWindowWidth * window_scale_,
               kDefaultWindowHeight * window_scale_ + menu_bar_->bounds().h);
      } else {
        Resize(kDefaultWindowWidth * window_scale_,
               kDefaultWindowHeight * window_scale_ + default_menu_height);
      }
    } else {
      Resize(kDefaultWindowWidth * window_scale_,
             kDefaultWindowHeight * window_scale_);
    }
  }

  if (canvas_)
    canvas_->set_frame_scale(window_scale_);
}

void MainWindow::UpdateGameControllerMapping() {
  const auto& game_controllers = Application::Get()->game_controllers();
  int index = 0;
  for (auto* game_controller : game_controllers) {
    SetControllerMapping(runtime_data_, index++, game_controller, false);
  }
}

void MainWindow::CleanupState() {
  SDL_assert(runtime_data_);
  runtime_data_->saved_state.clear();
  runtime_data_->saved_state_thumbnail.clear();
  SDL_assert(in_game_menu_);
  in_game_menu_->NotifyThumbnailChanged();
}

void MainWindow::OnRomLoaded(const std::string& name) {
  CleanupState();
  SetLoading(false);
  ShowMainMenu(false);
  SetTitle(name);
}

void MainWindow::OnQuit() {
  SDL_Event quit_event;
  quit_event.type = SDL_QUIT;
  SDL_PushEvent(&quit_event);
}

void MainWindow::OnResetROM() {
  SDL_assert(runtime_data_->emulator);
  runtime_data_->emulator->Reset(kiwi::base::BindOnce(
      &MainWindow::ResetAudio, kiwi::base::Unretained(this)));
}

void MainWindow::OnBackToMainMenu() {
  // Unload ROM, and show main menu.
  SetTitle("Kiwi Machine");
  SetLoading(true);

  SDL_assert(runtime_data_->emulator);
  runtime_data_->emulator->Unload(
      kiwi::base::BindRepeating(&MainWindow::ShowMainMenu,
                                kiwi::base::Unretained(this), true)
          .Then(kiwi::base::BindRepeating(
              &LoadingWidget::set_visible,
              kiwi::base::Unretained(loading_widget_), false))
          .Then(kiwi::base::BindRepeating(&Canvas::Clear,
                                          kiwi::base::Unretained(canvas_))));
}

void MainWindow::OnSaveState() {
  SDL_assert(runtime_data_->emulator);
  runtime_data_->emulator->SaveState(kiwi::base::BindOnce(
      [](MainWindow* window, NESRuntime::Data* runtime_data,
         kiwi::nes::Bytes data) {
        if (data.empty())
          Toast::ShowToast(window, "State save failed.");
        else
          Toast::ShowToast(window, "State saved.");

        runtime_data->saved_state = data;
        runtime_data->saved_state_thumbnail =
            window->canvas_->frame()->buffer();
        window->in_game_menu_->NotifyThumbnailChanged();
      },
      this, runtime_data_));
}

void MainWindow::OnLoadState() {
  audio_->Reset();
  runtime_data_->emulator->LoadState(
      runtime_data_->saved_state,
      kiwi::base::BindOnce(
          [](MainWindow* window, bool success) {
            if (success)
              Toast::ShowToast(window, "State loaded.");
            else
              Toast::ShowToast(window, "State load failed.");
          },
          this));
  audio_->Start();
}

bool MainWindow::CanSaveOrLoadState() {
  SDL_assert(runtime_data_->emulator);
  return runtime_data_->emulator->GetRunningState() !=
         kiwi::nes::Emulator::RunningState::kStopped;
}

void MainWindow::OnTogglePause() {
  SDL_assert(runtime_data_->emulator);
  if (IsPause()) {
    OnResume();
  } else {
    OnPause();
  }
}

void MainWindow::OnPause() {
  // Cleanup all pressing keys when pausing.
  pressing_keys_.clear();
  runtime_data_->emulator->Pause();
  memory_widget_->UpdateMemory();
  disassembly_widget_->UpdateDisassembly();
}

void MainWindow::OnResume() {
  runtime_data_->emulator->Run();
}

void MainWindow::OnLoadPresetROM(int which) {
  SDL_assert(runtime_data_->emulator);
  SetLoading(true);

  if (preset_roms::kPresetRoms[which].compressed) {
    runtime_data_->emulator->LoadAndRun(
        ReadFromZipBinary(preset_roms::kPresetRoms[which].data,
                          preset_roms::kPresetRoms[which].raw_size,
                          preset_roms::kPresetRoms[which].compressed_size),
        kiwi::base::BindOnce(&MainWindow::OnRomLoaded,
                             kiwi::base::Unretained(this),
                             preset_roms::kPresetRoms[which].name));
  } else {
    runtime_data_->emulator->LoadAndRun(
        ReadFromRawBinary(preset_roms::kPresetRoms[which].data,
                          preset_roms::kPresetRoms[which].raw_size),
        kiwi::base::BindOnce(&MainWindow::OnRomLoaded,
                             kiwi::base::Unretained(this),
                             preset_roms::kPresetRoms[which].name));
  }
}

void MainWindow::OnLoadSpecialROM(int which) {
  SDL_assert(runtime_data_->emulator);
  SetLoading(true);

  if (preset_roms::specials::kPresetRoms[which].compressed) {
    runtime_data_->emulator->LoadAndRun(
        ReadFromZipBinary(preset_roms::specials::kPresetRoms[which].data,
                          preset_roms::specials::kPresetRoms[which].raw_size,
                          preset_roms::specials::kPresetRoms[which].compressed_size),
        kiwi::base::BindOnce(&MainWindow::OnRomLoaded,
                             kiwi::base::Unretained(this),
                             preset_roms::specials::kPresetRoms[which].name));
  } else {
    runtime_data_->emulator->LoadAndRun(
        ReadFromRawBinary(preset_roms::specials::kPresetRoms[which].data,
                          preset_roms::specials::kPresetRoms[which].raw_size),
        kiwi::base::BindOnce(&MainWindow::OnRomLoaded,
                             kiwi::base::Unretained(this),
                             preset_roms::specials::kPresetRoms[which].name));
  }
}

void MainWindow::OnLoadDebugROM(kiwi::base::FilePath nes_path) {
  SDL_assert(runtime_data_->emulator);
  SetLoading(true);

  runtime_data_->emulator->LoadAndRun(
      nes_path, kiwi::base::BindOnce(&MainWindow::OnRomLoaded,
                                     kiwi::base::Unretained(this),
                                     nes_path.BaseName().AsUTF8Unsafe()));
}

void MainWindow::OnToggleAudioEnabled() {
  SDL_assert(runtime_data_->emulator);
  runtime_data_->emulator->SetVolume(IsAudioEnabled() ? 0 : 1);
  SetEffectVolume(IsAudioEnabled() ? 0 : 1);
}

void MainWindow::OnSetAudioVolume(float volume) {
  SDL_assert(runtime_data_->emulator);
  runtime_data_->emulator->SetVolume(volume);
  SetEffectVolume(volume);
}

bool MainWindow::IsAudioEnabled() {
  return runtime_data_->emulator->GetVolume() > 0;
}

void MainWindow::OnToggleAudioChannelMasks(kiwi::nes::AudioChannel which_mask) {
  SDL_assert(runtime_data_->debug_port);
  int current_mask = runtime_data_->debug_port->GetAudioChannelMasks();
  runtime_data_->debug_port->SetAudioChannelMasks(current_mask ^ which_mask);
}

bool MainWindow::IsAudioChannelOn(kiwi::nes::AudioChannel which_mask) {
  SDL_assert(runtime_data_->debug_port);
  return runtime_data_->debug_port->GetAudioChannelMasks() & which_mask;
}

void MainWindow::OnSetScreenScale(float scale) {
  SDL_assert(canvas_);
  if (window_scale_ != scale) {
    window_scale_ = scale;
    OnScaleChanged();
  }
}

void MainWindow::OnSetFullscreen() {
  is_fullscreen_ = true;
  SDL_SetWindowFullscreen(native_window(), SDL_WINDOW_FULLSCREEN);
}

void MainWindow::OnUnsetFullscreen(float scale) {
  is_fullscreen_ = false;
  SDL_SetWindowFullscreen(native_window(), 0);
}

bool MainWindow::ScreenScaleIs(float scale) {
  SDL_assert(canvas_);
  SDL_assert(window_scale() == canvas_->frame_scale());
  return canvas_->frame_scale() == scale;
}

void MainWindow::OnTogglePaletteWidget() {
  SDL_assert(palette_widget_);
  palette_widget_->set_visible(!palette_widget_->visible());
}

bool MainWindow::IsPaletteWidgetShown() {
  SDL_assert(palette_widget_);
  return palette_widget_->visible();
}

void MainWindow::OnTogglePatternWidget() {
  SDL_assert(pattern_widget_);
  pattern_widget_->set_visible(!pattern_widget_->visible());
}

bool MainWindow::IsPatternWidgetShown() {
  SDL_assert(pattern_widget_);
  return pattern_widget_->visible();
}

void MainWindow::OnFrameRateWidget() {
  SDL_assert(frame_rate_widget_);
  frame_rate_widget_->set_visible(!frame_rate_widget_->visible());
}

bool MainWindow::IsFrameRateWidgetShown() {
  SDL_assert(frame_rate_widget_);
  return frame_rate_widget_->visible();
}

void MainWindow::OnExportGameROMs() {
  kiwi::base::FilePath export_path = kiwi::base::FilePath::FromUTF8Unsafe(
                                         SDL_GetPrefPath("Kiwi", "KiwiMachine"))
                                         .Append("nes");
  scoped_refptr<kiwi::base::SequencedTaskRunner> io_task_runner =
      Application::Get()->GetIOTaskRunner();

  std::queue<preset_roms::PresetROM> roms_to_be_exported;
  for (auto iter = std::begin(preset_roms::kPresetRoms);
       iter != std::end(preset_roms::kPresetRoms); ++iter) {
    roms_to_be_exported.push(*iter);
  }

  if (roms_to_be_exported.empty())
    return;

  export_widget_->Start(roms_to_be_exported.size(), export_path);
  export_widget_->SetCurrent(
      kiwi::base::FilePath::FromUTF8Unsafe(roms_to_be_exported.front().name));

  io_task_runner->PostTaskAndReplyWithResult(
      FROM_HERE,
      kiwi::base::BindOnce(&OnExportGameROM, export_path, roms_to_be_exported),
      kiwi::base::BindOnce(&OnGameROMExported, kiwi::base::Unretained(this),
                           export_path));
}

void MainWindow::OnDebugMemory() {
  memory_widget_->set_visible(true);
  memory_widget_->UpdateMemory();
}

void MainWindow::OnDebugDisassembly() {
  disassembly_widget_->set_visible(true);
  disassembly_widget_->UpdateDisassembly();
}

void MainWindow::OnDebugNametable() {
  nametable_widget_->set_visible(true);
}

void MainWindow::OnInGameMenuTrigger() {
  in_game_menu_->Show();
  OnPause();
}

void MainWindow::OnInGameMenuItemTrigger(InGameMenu::MenuItem item) {
  switch (item) {
    case InGameMenu::MenuItem::kContinue: {
      in_game_menu_->Close();
      OnResume();
    } break;
    case InGameMenu::MenuItem::kLoadState: {
      OnLoadState();
      OnResume();
      in_game_menu_->Close();
    } break;
    case InGameMenu::MenuItem::kSaveState: {
      OnSaveState();
    } break;
    case InGameMenu::MenuItem::kToGameSelection: {
      in_game_menu_->Close();
      OnBackToMainMenu();
    } break;
    default:
      break;
  }
}

void MainWindow::OnInGameSettingsItemTrigger(InGameMenu::SettingsItem item,
                                             bool is_left) {
  switch (item) {
    case InGameMenu::SettingsItem::kVolume: {
      PlayEffect(audio_resources::AudioID::kSelect);
      float volume = runtime_data_->emulator->GetVolume();
      volume = (is_left ? volume - .1f : volume + .1f);
      if (volume < 0)
        volume = 0;
      else if (volume > 1.f)
        volume = 1.f;
      OnSetAudioVolume(volume);
    } break;
    case InGameMenu::SettingsItem::kWindowSize: {
      if (is_fullscreen_ && !is_left)
        return;

      if (is_fullscreen_ && is_left) {
        OnUnsetFullscreen(4);
      } else {
        int scale = window_scale();
        scale = (is_left ? scale - 1 : scale + 1);
        if (scale < 2) {
          scale = 2;
          OnSetScreenScale(scale);
        } else if (scale > 4) {
          OnSetFullscreen();
        } else {
          OnSetScreenScale(scale);
        }
      }
    }
    default:
      break;
  }
}

bool MainWindow::IsPause() {
  SDL_assert(runtime_data_->emulator);
  return runtime_data_->emulator->GetRunningState() ==
         kiwi::nes::Emulator::RunningState::kPaused;
}

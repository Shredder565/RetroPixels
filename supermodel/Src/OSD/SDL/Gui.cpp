// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The settings window (Dear ImGui), shown when the program is started with
// no arguments: a list of the games in Games.xml, a tab per settings group
// whose controls come from each setting's range or list of choices
// (DefaultConfig()), and a tab to assign the controls. Starting a game
// returns ROMs/<set>.zip to the caller.

#include "SDLIncludes.h"
#include <GL/glew.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "GameLoader.h"
#include "Pkgs/imgui/imgui.h"
#include "Pkgs/imgui/imgui_impl_sdl2.h"
#include "Pkgs/imgui/imgui_impl_opengl3.h"
#include "Util/NewConfig.h"
#include "Util/ConfigBuilders.h"
#include "OSD/SDL/SDLInputSystem.h"
#include "Inputs/Inputs.h"
#include "Gui.h"
#include "Main.h"

#ifdef _WIN32
#include "OSD/Windows/DirectInputSystem.h"
#endif

namespace
{
  // Tab label, and the settings group it shows.
  struct Tab
  {
    const char *label;
    const char *group;
  };
  const Tab kTabs[] =
  {
    { "Core", "Core" }, { "Video", "Video" }, { "Audio", "Sound" }, { "Networking", "Network" },
    { "Misc", "Misc" }, { "ForceFeedback", "ForceFeedback" }, { "Sensitivity", "Sensitivity" },
  };

  using Choice = Util::ValueRange::Variant;

  // A numeric setting: slider with a range, drop-down with a list, else a
  // number box. A value not in the list becomes the list's first.
  template <typename T>
  void NumberControl(const char *key, T &value, Util::ValueRange &range, ImGuiDataType type)
  {
    if (range.HasMinMax())
    {
      T lo = std::get<T>(range.GetMin()), hi = std::get<T>(range.GetMax());
      ImGui::SliderScalar(key, type, &value, &lo, &hi);
      return;
    }
    std::vector<Choice> &list = range.GetList();
    if (list.empty())
    {
      ImGui::InputScalar(key, type, &value);
      return;
    }
    std::vector<std::string> names;
    int chosen = 0;
    for (size_t i = 0; i < list.size(); i++)
    {
      names.push_back(std::to_string(std::get<T>(list[i])));
      if (std::get<T>(list[i]) == value)
        chosen = int(i);
    }
    std::vector<const char *> items;
    for (const std::string &n : names)
      items.push_back(n.c_str());
    ImGui::Combo(key, &chosen, items.data(), int(items.size()));
    value = std::get<T>(list[size_t(chosen)]);
  }

  void TextControl(const char *key, std::string &value, Util::ValueRange &range)
  {
    std::vector<Choice> &list = range.GetList();
    if (list.empty())
    {
      char text[256];
      strncpy(text, value.c_str(), sizeof(text) - 1);
      text[sizeof(text) - 1] = '\0';
      ImGui::InputText(key, text, sizeof(text));
      value = text;
      return;
    }
    int chosen = 0;
    std::vector<const char *> items;
    for (size_t i = 0; i < list.size(); i++)
    {
      const std::string &item = std::get<std::string>(list[i]);
      if (item == value)
        chosen = int(i);
      items.push_back(item.c_str());
    }
    ImGui::Combo(key, &chosen, items.data(), int(items.size()));
    value = std::get<std::string>(list[size_t(chosen)]);
  }

  // Draws one setting and stores what it shows back as the setting's type
  // (a value read from Supermodel.ini as text becomes typed this way).
  template <typename T>
  void Edit(Util::Config::Node &config, const std::string &key, const std::shared_ptr<Util::ValueRange> &range, ImGuiDataType type)
  {
    T value = config[key].ValueAs<T>();
    if constexpr (std::is_same_v<T, bool>)
      ImGui::Checkbox(key.c_str(), &value);
    else if constexpr (std::is_same_v<T, std::string>)
      TextControl(key.c_str(), value, *range);
    else
      NumberControl<T>(key.c_str(), value, *range, type);
    config.Set(key, value);
    config[key].GetValue()->SetValueRange(range);   // setting a value drops its range
  }

  void SettingsTab(Util::Config::Node &config, const std::string &group)
  {
    // keys first: editing replaces the nodes' values
    std::vector<std::pair<std::string, std::shared_ptr<Util::ValueRange>>> settings;
    for (const auto &node : config)
    {
      if (!node.IsLeaf() || !node.Exists() || !node.GetValue())
        continue;
      std::shared_ptr<Util::ValueRange> range = node.GetValue()->GetValueRange();
      if (range && range->GetGroup() == group)
        settings.emplace_back(node.Key(), range);
    }
    for (auto &[key, range] : settings)
    {
      switch (range->GetIndex())   // the type of the range's values
      {
      case 0: Edit<bool>(config, key, range, ImGuiDataType_S32); break;
      case 1: Edit<unsigned>(config, key, range, ImGuiDataType_U32); break;
      case 2: Edit<int>(config, key, range, ImGuiDataType_S32); break;
      case 3: Edit<float>(config, key, range, ImGuiDataType_Float); break;
      case 4: Edit<std::string>(config, key, range, ImGuiDataType_S32); break;
      }
    }
  }

  // The input system the settings name, ready to configure.
  std::shared_ptr<CInputs> OpenInputs(Util::Config::Node &config, SDL_Window *window)
  {
    const std::string name = config["InputSystem"].ValueAs<std::string>();
    std::shared_ptr<CInputSystem> system;
    if (name == "sdl" || name == "sdlgamepad")
      system = std::make_shared<CSDLInputSystem>(config, name == "sdlgamepad");
#ifdef SUPERMODEL_WIN32
    else if (name == "dinput" || name == "xinput" || name == "rawinput")
      system = std::make_shared<CDirectInputSystem>(config, window, name == "rawinput", name == "xinput");
#endif
    if (!system)
      return nullptr;
    auto inputs = std::make_shared<CInputs>(system);
    inputs->Initialize();
    inputs->LoadFromConfig(config);
    int x = 0, y = 0, w = 0, h = 0;
    SDL_GetWindowPosition(window, &x, &y);
    SDL_GL_GetDrawableSize(window, &w, &h);
    system->SetDisplayGeom(x, y, w, h);
    return inputs;
  }

  // State of the settings window between frames.
  struct Window
  {
    Util::Config::Node &config;
    const std::map<std::string, Game> &games;
    SDL_Window *sdl = nullptr;
    int selected = -1;                 // row in the game list
    bool done = false;
    bool save = true;                  // write Supermodel.ini on the way out
    std::shared_ptr<CInputs> inputs;   // while the controls tab is shown

    // the control being assigned in the popup
    std::shared_ptr<CInput> binding;
    bool listening = false;
    int framesListening = 0;

    Window(Util::Config::Node &settings, const std::map<std::string, Game> &list, SDL_Window *window)
      : config(settings), games(list), sdl(window)
    {
    }

    const Game *SelectedGame() const
    {
      int row = 0;
      for (const auto &entry : games)
      {
        if (row++ == selected)
          return &entry.second;
      }
      return nullptr;
    }

    void GameList()
    {
      ImGui::BeginChild("TableRegion", ImVec2(0.0f, 200.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
      if (ImGui::BeginTable("Games", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
      {
        for (const char *heading : { "Title", "Rom Name", "Version", "Year", "Stepping" })
          ImGui::TableSetupColumn(heading);
        ImGui::TableHeadersRow();
        int row = 0;
        for (const auto &entry : games)
        {
          const Game &g = entry.second;
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::Text("%s", g.title.c_str());
          ImGui::TableSetColumnIndex(1);
          if (ImGui::Selectable(g.name.c_str(), selected == row, ImGuiSelectableFlags_SpanAllColumns))
            selected = row;
          if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
            done = true;   // play it
          ImGui::TableSetColumnIndex(2);
          ImGui::Text("%s", g.version.c_str());
          ImGui::TableSetColumnIndex(3);
          ImGui::Text("%d", g.year);
          ImGui::TableSetColumnIndex(4);
          ImGui::Text("%s", g.stepping.c_str());
          row++;
        }
        ImGui::EndTable();
      }
      ImGui::EndChild();
    }

    // A yes/no question; true when answered, with the answer in *yes.
    static bool Ask(const char *id, const char *question, const char *yesText, const char *noText, bool *yes)
    {
      bool answered = false;
      if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
      {
        ImGui::Text("%s", question);
        ImGui::Separator();
        if (ImGui::Button(yesText, ImVec2(120, 0)))
        {
          *yes = answered = true;
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(noText, ImVec2(120, 0)))
        {
          *yes = false;
          answered = true;
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
      return answered;
    }

    void Buttons()
    {
      if (ImGui::Button("Load game"))
      {
        if (selected < 0)
          ImGui::OpenPopup("Load game");
        else
          done = true;
      }
      if (ImGui::BeginPopupModal("Load game", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
      {
        ImGui::Text("No game selected");
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0)))
          ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }

      ImGui::SameLine();
      if (ImGui::Button("Load Defaults"))
        ImGui::OpenPopup("Confirm Load");
      bool yes = false;
      if (Ask("Confirm Load", "Are you sure you want to load defaults?", "Yes", "Cancel", &yes) && yes)
        config = DefaultConfig();

      ImGui::SameLine();
      if (ImGui::Button("Exit"))
        ImGui::OpenPopup("Save On Exit");
      if (Ask("Save On Exit", "Save settings upon exit?", "Yes", "No", &yes))
      {
        selected = -1;   // leave without playing
        save = yes;
        done = true;
      }
    }

    // The popup for one control: its keys, and buttons to replace, add to,
    // clear or reset them. Assigning waits a couple of frames so the popup
    // is drawn before the (blocking) wait for a key.
    void BindingPopup(bool open)
    {
      if (open)
        ImGui::OpenPopup("Key Binding");
      if (!ImGui::BeginPopupModal("Key Binding", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

      if (ImGui::BeginTable("ShortcutTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
      {
        ImGui::TableSetupColumn("Action");
        ImGui::TableSetupColumn("Keys");
        ImGui::TableHeadersRow();
        const std::string mapping = binding->GetMapping();
        size_t start = 0;
        while (true)   // one row per comma separated part
        {
          const size_t comma = mapping.find(',', start);
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::Text("%s", binding->label);
          ImGui::TableNextColumn();
          ImGui::Text("%s", mapping.substr(start, comma == std::string::npos ? std::string::npos : comma - start).c_str());
          if (comma == std::string::npos)
            break;
          start = comma + 1;
        }
        ImGui::EndTable();
      }
      ImGui::Spacing();
      if (listening)
      {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 0.73f));
        ImGui::Text("Waiting for key input. Press esc to cancel.");
        ImGui::PopStyleColor();
        framesListening++;
      }
      else
        ImGui::NewLine();
      ImGui::Spacing();

      bool listen = false, finish = false;
      if (listening)
        ImGui::BeginDisabled();
      const ImVec2 size(120, 0);
      if (ImGui::Button("Set", size))
      {
        binding->ClearMapping();
        listen = true;
      }
      if (ImGui::Button("Append", size))
        listen = true;
      if (ImGui::Button("Clear", size))
      {
        binding->ClearMapping();
        binding->StoreToConfig(&config);
      }
      if (ImGui::Button("Default", size))
      {
        const Util::Config::Node defaults = DefaultConfig();
        const Util::Config::Node *setting = defaults.TryGet(std::string("Input") + binding->id);
        if (setting && !setting->Empty())
          binding->SetMapping(setting->ValueAs<std::string>().c_str());
        else
          binding->ResetToDefaultMapping();   // the emulator's own keys
        binding->StoreToConfig(&config);
      }
      if (ImGui::Button("Finish", size))
      {
        ImGui::CloseCurrentPopup();
        finish = true;
      }
      if (listening)
        ImGui::EndDisabled();
      ImGui::EndPopup();

      if (framesListening > 2)
      {
        std::shared_ptr<CInputSystem> system = binding->GetInputSystem();
        system->UngrabMouse();
        binding->Configure(true);
        system->GrabMouse();
        binding->StoreToConfig(&config);
        framesListening = 0;
        listening = false;
      }
      if (listen)
        listening = true;
      if (finish)
      {
        binding = nullptr;
        listening = false;
        framesListening = 0;
      }
    }

    void ControlsTab()
    {
      if (!inputs)
        inputs = OpenInputs(config, sdl);
      if (!inputs)
        return;
      const Game *game = SelectedGame();
      bool open = false;
      if (ImGui::BeginTable("KeyTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
      {
        ImGui::TableSetupColumn("Group");
        ImGui::TableSetupColumn("Action");
        ImGui::TableSetupColumn("Keys");
        ImGui::TableHeadersRow();
        for (const std::shared_ptr<CInput> &input : inputs->GetGameInputs(game ? *game : Game()))
        {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::Text("%s", input->GetInputGroup());
          ImGui::TableSetColumnIndex(1);
          if (ImGui::Selectable(input->label, false, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns) && ImGui::IsMouseDoubleClicked(0))
          {
            binding = input;
            open = true;
          }
          ImGui::TableSetColumnIndex(2);
          ImGui::Text("%s", input->GetMapping());
        }
        BindingPopup(open);
        ImGui::EndTable();
      }
    }

    void Tabs()
    {
      if (!ImGui::BeginTabBar("MyTabBar", ImGuiTabBarFlags_FittingPolicyResizeDown))
        return;
      for (const Tab &tab : kTabs)
      {
        if (!ImGui::BeginTabItem(tab.label))
          continue;
        SettingsTab(config, tab.group);
        if (std::string(tab.group) == "Sensitivity" && ImGui::Button("Joystick calibration"))
        {
          if (!inputs)
            inputs = OpenInputs(config, sdl);
          if (inputs)
          {
            inputs->CalibrateJoysticks();
            inputs->StoreToConfig(&config);
          }
        }
        ImGui::EndTabItem();
        inputs = nullptr;   // made again for the controls tab
      }
      if (ImGui::BeginTabItem("Key bindings"))
      {
        ControlsTab();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

    void Frame()
    {
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplSDL2_NewFrame();
      ImGui::NewFrame();

      ImGui::SetNextWindowPos(ImVec2(0, 0));
      ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
      ImGui::Begin("Custom Window", nullptr, ImGuiWindowFlags_NoTitleBar);
      GameList();
      Buttons();
      ImGui::Dummy(ImVec2(0.0f, 20.0f));
      Tabs();
      ImGui::End();

      ImGui::Render();
      const ImVec2 size = ImGui::GetIO().DisplaySize;
      glViewport(0, 0, int(size.x), int(size.y));
      glClearColor(0.0f, 0.5f, 192 / 255.f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
  };
}

std::vector<std::string> RunGUI(const std::string &configPath, Util::Config::Node &config)
{
  if (SDL_Init(SDL_INIT_VIDEO) < 0)
  {
    std::cerr << "SDL could not initialize! Error: " << SDL_GetError() << std::endl;
    return {};
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  const Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_INPUT_FOCUS;
  SDL_Window *sdl = SDL_CreateWindow("SuperSetup", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 950, 600, flags);
  if (!sdl)
  {
    std::cerr << "Window could not be created! Error: " << SDL_GetError() << std::endl;
    SDL_Quit();
    return {};
  }
  SDL_GLContext gl = SDL_GL_CreateContext(sdl);
  if (!gl)
  {
    std::cerr << "OpenGL context could not be created! Error: " << SDL_GetError() << std::endl;
    SDL_DestroyWindow(sdl);
    SDL_Quit();
    return {};
  }
  SDL_GL_MakeCurrent(sdl, gl);
  SDL_GL_SetSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  io.Fonts->AddFontDefaultVector();
  io.IniFilename = nullptr;   // no window layout file
  ImGui::StyleColorsDark();
  ImGui_ImplSDL2_InitForOpenGL(sdl, gl);
  ImGui_ImplOpenGL3_Init("#version 410");

  GameLoader loader(config["GameXMLFile"].ValueAs<std::string>());
  Window window(config, loader.GetGames(), sdl);

  std::vector<std::string> romFiles;
  bool closed = false;   // window closed: leave without saving
  while (!window.done && !closed)
  {
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
      ImGui_ImplSDL2_ProcessEvent(&event);
      if (event.type == SDL_QUIT)
        closed = true;
    }
    if (closed)
      break;
    window.Frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    SDL_GL_SwapWindow(sdl);
  }

  if (!closed)
  {
    if (const Game *game = window.SelectedGame())
      romFiles.push_back((std::filesystem::path("ROMs") / (game->name + ".zip")).string());
    if (window.save)
      Util::Config::WriteINIFile(configPath, config, "");
  }

  window.inputs = nullptr;
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DeleteContext(gl);
  SDL_DestroyWindow(sdl);
  SDL_Quit();
  return romFiles;
}

#include "netplay_session.hpp"

#include "netplay_compatibility.hpp"

#include "Common/StringUtil.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/NetPlay/NetPlayServer.h"
#include "Core/PowerPC/PowerPC.h"
#include "UICommon/GameFile.h"
#include "UICommon/UICommon.h"
#include "runtime/dolphin_runtime_internal.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace moderngekko::frontend {
namespace {
struct ControllerOption {
  std::string label;
  std::string device;
};

std::vector<ControllerOption> EnumerateControllers() {
  std::vector<ControllerOption> result;
  std::unordered_map<std::string, int> device_ids;
  int count = 0;
  SDL_JoystickID *joystick_ids = SDL_GetJoysticks(&count);
  // SDL returns NULL on failure; don't index it even if count was left >0.
  if (joystick_ids == nullptr)
    return result;
  for (int i = 0; i < count; ++i) {
    const SDL_JoystickID joystick_id = joystick_ids[i];
    if (!SDL_IsGamepad(joystick_id))
      continue;
    const char *name_value = SDL_GetGamepadNameForID(joystick_id);
    const std::string name =
        name_value && *name_value ? name_value : "Unknown Controller";
    const int device_id = device_ids[name]++;
    result.push_back({device_id == 0
                          ? name
                          : name + " (" + std::to_string(device_id + 1) + ")",
                      "SDL/" + std::to_string(device_id) + "/" + name});
  }
  SDL_free(joystick_ids);
  return result;
}

class SessionUI final : public NetPlay::NetPlayUI {
public:
  explicit SessionUI(std::shared_ptr<UICommon::GameFile> game)
      : m_game(std::move(game)) {}

  void SetClient(NetPlay::NetPlayClient *client) {
    std::lock_guard lock(m_mutex);
    m_client = client;
  }

  void SetHosting(bool hosting) {
    std::lock_guard lock(m_mutex);
    m_hosting = hosting;
  }

  void SetRuntime(Runtime *runtime) {
    std::lock_guard lock(m_mutex);
    m_runtime = runtime;
  }

  bool TakeStartRequest() { return m_start_requested.exchange(false); }

  std::unique_ptr<BootSessionData> TakeBootData() {
    std::lock_guard lock(m_mutex);
    return std::move(m_boot_data);
  }

  std::string Error() {
    std::lock_guard lock(m_mutex);
    return m_error;
  }

  std::string Status() {
    std::lock_guard lock(m_mutex);
    return m_status;
  }

  u32 BufferSize() const { return m_buffer_size.load(); }

  bool ConnectionLost() const { return m_connection_lost.load(); }

  void ClearReadyState() {
    // Called from the UI thread while the client's network thread is alive;
    // snapshot under the mutex like every other m_client access.
    NetPlay::NetPlayClient *client;
    {
      std::lock_guard lock(m_mutex);
      client = m_client;
    }
    if (client)
      client->SetReady(false);
  }

  void BootGame(const std::string &,
                std::unique_ptr<BootSessionData> boot_session_data) override {
    std::lock_guard lock(m_mutex);
    m_boot_data = std::move(boot_session_data);
  }

  void StopGame() override {
    std::lock_guard lock(m_mutex);
    if (m_runtime)
      m_runtime->RequestStop();
  }

  bool IsHosting() const override {
    std::lock_guard lock(m_mutex);
    return m_hosting;
  }

  void Update() override {}

  void AppendChat(const std::string &msg) override {
    std::lock_guard lock(m_mutex);
    m_status = msg;
  }

  void OnMsgChangeGame(const NetPlay::SyncIdentifier &,
                       const std::string &name) override {
    std::lock_guard lock(m_mutex);
    m_status = "Game: " + name;
  }

  void OnMsgChangeGBARom(int, const NetPlay::GBAConfig &) override {}

  void OnMsgStartGame() override { m_start_requested = true; }

  void OnMsgStopGame() override { StopGame(); }

  void OnMsgPowerButton() override { StopGame(); }

  void OnPlayerConnect(const std::string &player) override {
    std::lock_guard lock(m_mutex);
    m_status = player + " joined";
  }

  void OnPlayerDisconnect(const std::string &player) override {
    std::lock_guard lock(m_mutex);
    m_status = player + " left";
  }

  void OnPadBufferChanged(u32 buffer) override { m_buffer_size = buffer; }

  void OnHostInputAuthorityChanged(bool) override {}

  void OnDesync(u32 frame, const std::string &player) override {
    {
      std::lock_guard lock(m_mutex);
      m_error = "Desync at frame " + std::to_string(frame);
      if (!player.empty())
        m_error += " reported for " + player;
    }
    StopGame();
  }

  void OnConnectionLost() override {
    m_connection_lost = true;
    {
      std::lock_guard lock(m_mutex);
      m_error = "Connection to the netplay host was lost";
    }
    StopGame();
  }

  void OnConnectionError(const std::string &message) override {
    std::lock_guard lock(m_mutex);
    m_error = message;
  }

  void OnTraversalError(Common::TraversalClient::FailureReason) override {
    OnConnectionError("Direct connection failed");
  }

  void OnTraversalStateChanged(Common::TraversalClient::State) override {}

  void OnGameStartAborted() override {
    std::lock_guard lock(m_mutex);
    m_error = "Game start was aborted";
  }

  void OnGolferChanged(bool, const std::string &) override {}
  void OnTtlDetermined(u8) override {}
  bool IsRecording() override { return false; }

  std::shared_ptr<const UICommon::GameFile>
  FindGameFile(const NetPlay::SyncIdentifier &sync_identifier,
               NetPlay::SyncIdentifierComparison *found) override {
    const auto comparison = m_game->CompareSyncIdentifier(sync_identifier);
    if (found)
      *found = comparison;
    return m_game;
  }

  std::string FindGBARomPath(const std::array<u8, 20> &, std::string_view,
                             int) override {
    return {};
  }

  void ShowGameDigestDialog(const std::string &) override {}
  void SetGameDigestProgress(int, int) override {}
  void SetGameDigestResult(int, const std::string &) override {}
  void AbortGameDigest() override {}
  void OnIndexAdded(bool, std::string) override {}
  void OnIndexRefreshFailed(std::string) override {}

  void ShowChunkedProgressDialog(const std::string &title, u64 data_size,
                                 std::span<const int>) override {
    std::lock_guard lock(m_mutex);
    m_status = title;
    m_progress_size = data_size;
    m_progress = 0;
  }

  void HideChunkedProgressDialog() override {
    std::lock_guard lock(m_mutex);
    m_progress_size = 0;
    m_progress = 0;
  }

  void SetChunkedProgress(int, u64 progress) override {
    std::lock_guard lock(m_mutex);
    m_progress = progress;
  }

  void SetHostWiiSyncData(std::vector<u64> titles,
                          std::string redirect_folder) override {
    std::lock_guard lock(m_mutex);
    if (m_client)
      m_client->SetWiiSyncData(nullptr, std::move(titles),
                               std::move(redirect_folder));
  }

private:
  std::shared_ptr<UICommon::GameFile> m_game;
  mutable std::mutex m_mutex;
  NetPlay::NetPlayClient *m_client = nullptr;
  Runtime *m_runtime = nullptr;
  bool m_hosting = false;
  std::atomic<bool> m_start_requested{false};
  std::atomic<bool> m_connection_lost{false};
  std::atomic<u32> m_buffer_size{5};
  std::unique_ptr<BootSessionData> m_boot_data;
  std::string m_error;
  std::string m_status;
  u64 m_progress_size = 0;
  u64 m_progress = 0;
};

class LobbyWindow {
public:
  bool Open(WindowSystem window_system) {
#if defined(__linux__)
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER,
                window_system == WindowSystem::Wayland ? "wayland" : "x11");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
      return false;
    // SDL_GetDisplayContentScale returns 0.0f on failure — a 0/NaN scale
    // would produce a 0x0 window and zero out every ImGui size below.
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (!(scale > 0.0f))
      scale = 1.0f;
    m_window = SDL_CreateWindow(
        "KirbyRecomp Netplay Lobby", static_cast<int>(820 * scale),
        static_cast<int>(600 * scale),
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!m_window) {
      SDL_Quit();
      return false;
    }
    m_renderer = SDL_CreateRenderer(m_window, "vulkan");
    if (!m_renderer)
      m_renderer = SDL_CreateRenderer(m_window, nullptr);
    if (!m_renderer) {
      // Close() is a no-op while m_open is false; clean up inline.
      SDL_DestroyWindow(m_window);
      m_window = nullptr;
      SDL_Quit();
      return false;
    }
    SDL_SetRenderVSync(m_renderer, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
    ImGui_ImplSDL3_InitForSDLRenderer(m_window, m_renderer);
    ImGui_ImplSDLRenderer3_Init(m_renderer);
    m_open = true;
    return true;
  }

  ~LobbyWindow() { Close(); }

  void Close() {
    if (!m_open)
      return;
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(m_renderer);
    SDL_DestroyWindow(m_window);
    SDL_Quit();
    m_renderer = nullptr;
    m_window = nullptr;
    m_open = false;
  }

  bool Frame() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT ||
          event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        return false;
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    return true;
  }

  void Present() {
    ImGui::Render();
    const ImGuiIO &io = ImGui::GetIO();
    SDL_SetRenderScale(m_renderer, io.DisplayFramebufferScale.x,
                       io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(m_renderer, 18, 20, 28, 255);
    SDL_RenderClear(m_renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), m_renderer);
    SDL_RenderPresent(m_renderer);
  }

private:
  SDL_Window *m_window = nullptr;
  SDL_Renderer *m_renderer = nullptr;
  bool m_open = false;
};

enum class LobbyOutcome {
  Closed,  // window closed or connection lost — clean exit
  Boot,    // boot data received — run the game
  Failed,  // window/renderer could not be created
};

LobbyOutcome RunLobbyWindow(RuntimeConfig &runtime_config,
                            ConfigResult &frontend_config,
                            NetplayOptions &options, SessionUI &ui,
                            NetPlay::NetPlayClient &client,
                            NetPlay::NetPlayServer *server) {
  LobbyWindow window;
  if (!window.Open(runtime_config.window_system)) {
    std::cerr << "netplay: could not create the lobby window\n";
    return LobbyOutcome::Failed;
  }

  std::vector<ControllerOption> available = EnumerateControllers();
  auto last_refresh = std::chrono::steady_clock::now();
  bool running = true;
  while (running) {
    if (!window.Frame())
      break;

    const auto now = std::chrono::steady_clock::now();
    if (now - last_refresh >= std::chrono::seconds(1)) {
      available = EnumerateControllers();
      last_refresh = now;
    }

    const std::vector<NetPlay::Player> players = client.GetPlayersSnapshot();
    const NetPlay::PadMappingArray wiimote_mapping =
        client.GetWiimoteMappingSnapshot();
    const u8 assigned = client.GetAssignedControllerCount();
    if (assigned > 0 && options.controllers.size() > assigned) {
      options.controllers.resize(assigned);
      frontend_config.controllers = options.controllers;
      frontend_config.controller = options.controllers.front();
      std::string ignored;
      SaveConfig(runtime_config.user_directory, frontend_config, &ignored);
    }

    if (ui.TakeStartRequest())
      client.StartGame(PathToString(runtime_config.game_root / "sys" /
                                    "main.dol"));
    auto boot_data = ui.TakeBootData();
    if (boot_data) {
      detail::SetBootSessionData(std::move(boot_data));
      window.Close();
      return LobbyOutcome::Boot;
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("KirbyRecomp Netplay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted(server ? "Hosting direct-IP netplay"
                                  : "Connected to direct-IP host");
    if (server) {
      ImGui::Text("UDP port: %u", server->GetPort());
      for (const std::string &interface_name : server->GetInterfaceSet())
        ImGui::Text("%s: %s", interface_name.c_str(),
                    server->GetInterfaceHost(interface_name).c_str());
    } else {
      ImGui::Text("Host: %s:%u", options.address.c_str(), options.port);
    }
    ImGui::Text("Input buffer: %u frames%s", ui.BufferSize(),
                server && options.buffer == "auto" ? " (auto)" : "");
    ImGui::Separator();

    if (ImGui::BeginTable("players", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("Player");
      ImGui::TableSetupColumn("Ping");
      ImGui::TableSetupColumn("Controllers");
      ImGui::TableSetupColumn("Game");
      ImGui::TableSetupColumn("Ready");
      ImGui::TableHeadersRow();
      for (const NetPlay::Player &player : players) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(player.name.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%u ms", player.ping);
        ImGui::TableSetColumnIndex(2);
        std::string slots;
        for (std::size_t i = 0; i < wiimote_mapping.size(); ++i) {
          if (wiimote_mapping[i] != player.pid)
            continue;
          if (!slots.empty())
            slots += ", ";
          slots += "Wii " + std::to_string(i + 1);
        }
        ImGui::TextUnformatted(slots.empty() ? "None" : slots.c_str());
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(
            player.game_status == NetPlay::SyncIdentifierComparison::SameGame
                ? "Match"
                : "Mismatch");
        ImGui::TableSetColumnIndex(4);
        ImGui::TextUnformatted(player.ready ? "Ready" : "Not ready");
      }
      ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Local sideways Wii Remotes");
    std::size_t total_assigned = 0;
    for (const NetPlay::Player &player : players)
      total_assigned += player.controller_count;
    // Other machines may already hold all four ports; keep the subtraction
    // saturating so a stale or over-reported snapshot can't wrap negative.
    const std::size_t local_limit =
        static_cast<std::size_t>(assigned) + 4 > total_assigned
            ? std::min<std::size_t>(
                  4, static_cast<std::size_t>(assigned) + 4 - total_assigned)
            : 0;
    for (const ControllerOption &controller : available) {
      bool selected =
          std::ranges::contains(options.controllers, controller.device);
      ImGui::BeginDisabled(!selected &&
                           options.controllers.size() >= local_limit);
      if (ImGui::Checkbox(controller.label.c_str(), &selected)) {
        if (selected)
          options.controllers.push_back(controller.device);
        else if (options.controllers.size() > 1)
          std::erase(options.controllers, controller.device);
        frontend_config.controllers = options.controllers;
        frontend_config.controller = options.controllers.front();
        std::string error;
        if (GenerateControllerConfig(runtime_config.user_directory,
                                     options.controllers, &error) &&
            SaveConfig(runtime_config.user_directory, frontend_config,
                       &error)) {
          client.SetLocalControllerCount(
              static_cast<u8>(options.controllers.size()));
          client.SetReady(false);
        }
      }
      ImGui::EndDisabled();
    }

    const auto local_player =
        std::ranges::find_if(players, [&](const NetPlay::Player &player) {
          return client.IsLocalPlayer(player.pid);
        });
    const bool local_ready =
        local_player != players.end() && local_player->ready;
    if (ImGui::Button(local_ready ? "Not Ready" : "Ready", ImVec2(140, 38)))
      client.SetReady(!local_ready);

    if (server) {
      ImGui::SameLine();
      const bool can_start = server->CanStart();
      ImGui::BeginDisabled(!can_start);
      if (ImGui::Button("Start", ImVec2(140, 38)))
        server->RequestStartGame();
      ImGui::EndDisabled();
      if (!can_start)
        ImGui::TextDisabled(
            "Two controller slots and every machine ready are required");
    }

    const std::string status = ui.Status();
    const std::string error = ui.Error();
    if (!status.empty())
      ImGui::TextWrapped("%s", status.c_str());
    if (!error.empty())
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", error.c_str());
    ImGui::End();
    window.Present();

    if (ui.ConnectionLost())
      running = false;
  }
  return LobbyOutcome::Closed;
}
} // namespace

int RunNetplayLobby(RuntimeConfig runtime_config, ConfigResult frontend_config,
                    NetplayOptions options) {
  if (options.controllers.empty())
    return static_cast<int>(NetplayExitCode::InvalidConfiguration);

  std::cerr << "netplay: initializing Dolphin services\n";
  UICommon::SetUserDirectory(PathToString(runtime_config.user_directory));
  UICommon::Init();
  detail::SetExternalUICommon(true);

  Config::SetBase(Config::MAIN_CPU_THREAD, true);
  Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::StaticRecomp);
  Config::SetBase(Config::NETPLAY_SAVEDATA_LOAD, true);
  Config::SetBase(Config::NETPLAY_SAVEDATA_WRITE, true);
  Config::SetBase(Config::NETPLAY_SAVEDATA_SYNC_ALL_WII, false);
  Config::SetBase(Config::NETPLAY_SYNC_CODES, false);
  Config::SetBase(Config::NETPLAY_STRICT_SETTINGS_SYNC, true);
  Config::SetBase(Config::NETPLAY_NETWORK_MODE, std::string("fixeddelay"));
  Config::SetBase(Config::NETPLAY_USE_INDEX, false);

  auto game = std::make_shared<UICommon::GameFile>(
      PathToString(runtime_config.game_root / "sys" / "main.dol"));
  if (!game->IsValid()) {
    detail::SetExternalUICommon(false);
    UICommon::Shutdown();
    return static_cast<int>(NetplayExitCode::InvalidConfiguration);
  }

  const GameInspectResult inspected = InspectGame(runtime_config.game_root);
  if (!inspected) {
    std::cerr << "netplay: game inspection failed: " << inspected.error
              << '\n';
    detail::SetExternalUICommon(false);
    UICommon::Shutdown();
    return static_cast<int>(NetplayExitCode::InvalidConfiguration);
  }
  // Reuse this inspection on every Runtime::Create in the lobby loop —
  // each game boot would otherwise re-hash the whole files/ tree.
  runtime_config.preinspected_metadata = inspected.metadata;
  std::cerr << "netplay: validating compatibility\n";
  NetPlay::SetCompatibilityFingerprint(
      CompatibilityFingerprint(runtime_config, *inspected.metadata));
  std::cerr << "netplay: compatibility validated\n";

  SessionUI ui(game);
  const NetPlay::NetTraversalConfig direct{};
  std::unique_ptr<NetPlay::NetPlayServer> server;
  std::unique_ptr<NetPlay::NetPlayClient> client;
  if (options.role == NetplayRole::Host) {
    std::cerr << "netplay: opening host on UDP port " << options.port << '\n';
    ui.SetHosting(true);
    server = std::make_unique<NetPlay::NetPlayServer>(options.port, false, &ui,
                                                      direct);
    if (!server->is_connected) {
      server.reset();
      detail::SetExternalUICommon(false);
      UICommon::Shutdown();
      return static_cast<int>(NetplayExitCode::Failed);
    }
    server->SetHostInputAuthority(false);
    server->SetAdaptiveBuffer(options.buffer == "auto");
    if (options.buffer != "auto")
      server->AdjustPadBufferSize(
          static_cast<unsigned int>(std::stoul(options.buffer)));
    server->ChangeGame(game->GetSyncIdentifier(),
                       inspected.metadata->game_name);
    client = std::make_unique<NetPlay::NetPlayClient>(
        "127.0.0.1", server->GetPort(), &ui, options.nickname, direct,
        static_cast<u8>(options.controllers.size()));
  } else {
    std::cerr << "netplay: connecting to " << options.address << ':'
              << options.port << '\n';
    client = std::make_unique<NetPlay::NetPlayClient>(
        options.address, options.port, &ui, options.nickname, direct,
        static_cast<u8>(options.controllers.size()));
  }

  if (!client->IsConnected()) {
    const NetPlay::ConnectionError connection_error =
        client->GetConnectionError();
    const std::string error = ui.Error();
    if (!error.empty())
      std::cerr << "netplay: " << error << '\n';
    client.reset();
    server.reset();
    detail::SetExternalUICommon(false);
    UICommon::Shutdown();
    switch (connection_error) {
    case NetPlay::ConnectionError::VersionMismatch:
      return static_cast<int>(NetplayExitCode::VersionMismatch);
    case NetPlay::ConnectionError::CompatibilityMismatch:
      return static_cast<int>(NetplayExitCode::CompatibilityMismatch);
    case NetPlay::ConnectionError::RoomFull:
      return static_cast<int>(NetplayExitCode::RoomFull);
    case NetPlay::ConnectionError::GameRunning:
      return static_cast<int>(NetplayExitCode::GameRunning);
    case NetPlay::ConnectionError::ServerFull:
      return static_cast<int>(NetplayExitCode::ServerFull);
    case NetPlay::ConnectionError::NameTooLong:
      return static_cast<int>(NetplayExitCode::NicknameRejected);
    case NetPlay::ConnectionError::NoError:
      return static_cast<int>(NetplayExitCode::HostUnavailable);
    default:
      // The error byte comes from the wire; a host can send values outside
      // the enum. Don't fall through to the lobby on a dead client.
      return static_cast<int>(NetplayExitCode::Failed);
    }
  }
  ui.SetClient(client.get());
  std::cerr << "netplay: connected; opening lobby\n";

  int result = 0;
  bool remain = true;
  while (remain) {
    const LobbyOutcome outcome =
        RunLobbyWindow(runtime_config, frontend_config, options, ui, *client,
                       server.get());
    if (outcome == LobbyOutcome::Failed) {
      result = static_cast<int>(NetplayExitCode::Failed);
      break;
    }
    if (outcome != LobbyOutcome::Boot)
      break;

    auto created = Runtime::Create(runtime_config);
    if (!created) {
      result = 1;
      break;
    }
    ui.SetRuntime(created.runtime.get());
    const RuntimeRunResult run_result = created.runtime->Run();
    ui.SetRuntime(nullptr);
    client->Stop();
    client->StopGame();
    created.runtime.reset();
    ui.ClearReadyState();
    if (run_result.error)
      result = 1;
    remain = !ui.ConnectionLost();
  }

  client.reset();
  server.reset();
  detail::SetExternalUICommon(false);
  UICommon::Shutdown();
  return result;
}
} // namespace moderngekko::frontend

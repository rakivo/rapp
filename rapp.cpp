#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <array>
#include <memory>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <string_view>
#include <unordered_set>

#include "raylib.h"

namespace fs = std::filesystem;

#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define shift(argc, argv) (assert(argc), argc--, *argv++)

struct file_t {
  char *ptr;
  size_t size;

  inline constexpr file_t(void) noexcept
    : ptr(NULL), size(0) {}

  inline constexpr file_t(char *ptr, off_t size) noexcept
    : ptr(ptr), size(size) {}

  inline constexpr char
  operator[](off_t offset) const noexcept
  {
    if (offset < 0 or (size_t) offset >= size) [[unlikely]] {
      assert(0 && "unreachable");
    }
    return ptr[offset];
  };

  inline constexpr ~file_t(void)
  {
    if (ptr == 0 or size == 0) return;

    if (munmap(ptr, size) == -1) [[unlikely]] {
      eprintf("could not unmap file\n");
      exit(EXIT_FAILURE);
    }

#if not defined(NDEBUG)
    printf("unmapped %zu bytes from %p\n", size, ptr);
#endif
  }

  static const file_t read(const char *file_path, bool *ok);
};

struct app_t {
  std::string name, exec;

  ~app_t(void) = default;
  
  static const app_t parse(const char *file_path, bool *ok);
};

std::vector<std::string_view>
split(const char *str, size_t size, char delim);

constexpr Color TEXT_COLOR              = {209, 184, 151, 0xFF};
constexpr Color ACCENT_COLOR            = {100, 150, 170, 0xFF};
constexpr Color HIGHLIGHT_COLOR         = { 30,  50,  57, 0xFF};
constexpr Color SCROLLBAR_COLOR         = { 50,  70,  80, 0xFF};
constexpr Color BACKGROUND_COLOR        = {  6,  35,  41, 0xFF};
constexpr Color PROMPT_BACKGROUND_COLOR = { 30,  30,  30, 0xFF};

constexpr int PADDING = 20;
constexpr int FONT_SIZE = 20;
constexpr int LINE_H = FONT_SIZE + 10;

constexpr float SCROLL_SPEED = 50.0;

constexpr int WINDOW_W = 800;
constexpr int WINDOW_H = 600;

constexpr float PROMPT_H = 40.0;

constexpr float INITIAL_KEY_DELAY = 0.5;
constexpr float REPEAT_KEY_INTERVAL = 0.125;

const file_t file_t::read(const char *file_path, bool *ok)
{
  int fd = open(file_path, O_RDONLY, (mode_t) 0400);
  if (fd == -1) {
    *ok = false;
    return {};
  }

  struct stat file_info = {0};
  if (fstat(fd, &file_info) == -1) {
    *ok = false;
    return {};
  }

  const off_t size = file_info.st_size;
  if (size == 0) {
    return {};
  }

  char *ptr = (char *) mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
  if (ptr == MAP_FAILED) {
    close(fd);
    *ok = false;
    return {};
  }

  close(fd);

  return file_t{ptr, size};
}

const app_t app_t::parse(const char *file_path, bool *ok)
{
  auto ok_ = true;
  const auto file = file_t::read(file_path, &ok_);

  if (file.size == 0) {
    if (!ok) {
      eprintf("could not read file: %s\n", file_path);
    }
    *ok = ok_;
    return {};
  }

  std::string exec, name;
  for (auto &line: split(file.ptr, file.size, '\n')) {
    if (line.find("Name=") == 0) {
      name = line.substr(5);
    } else if (line.find("Exec=") == 0) {
      exec = line.substr(5);
    }
  }

  return app_t{name, exec};
}

std::vector<std::string_view>
split(const char *str, size_t size, char delim)
{
  std::vector<std::string_view> ret = {};

  const char *start = str, *ptr = str, *const end = str + size;
  for (; ptr != end; ptr++) {
    if (*ptr == delim) {
      ret.emplace_back(start, ptr - start);
      start = ptr + 1;
    }
  }

  if (start < ptr) {
    ret.emplace_back(start, ptr - start);
  }

  return ret;
}

void launch_application(const std::string &command)
{
  std::string cleaned_command = command;
  size_t pos = cleaned_command.find('%');
  while (pos != std::string::npos) {
    cleaned_command.erase(pos, 2);
    pos = cleaned_command.find('%');
  }

  std::string arg = {};
  std::vector<std::string> args = {};
  for (char c: cleaned_command) {
    if (c == ' ') {
      if (!arg.empty()) {
        args.emplace_back(arg);
        arg.clear();
      }
    } else {
      arg += c;
    }
  }

  if (!arg.empty()) {
    args.emplace_back(arg);
  }

  pid_t pid = fork();
  if (pid == 0) {
    if (setsid() < 0) {
      perror("setsid failed");
      exit(EXIT_FAILURE);
    }

    int fd = open("/dev/null", O_RDWR);
    if (fd < 0) {
      perror("open /dev/null failed");
      exit(EXIT_FAILURE);
    }

    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);

    std::vector<char *> argv(args.size() + 1);
    for (size_t i = 0; i < args.size(); ++i) {
      argv[i] = const_cast<char *>(args[i].c_str());
    }

    argv[args.size()] = NULL;
    execvp(argv[0], argv.data());
    perror("execvp failed");
    exit(EXIT_FAILURE);
  } else if (pid < 0) {
    perror("fork failed");
  }
}

static std::string input;

static std::vector<app_t> apps;
static std::vector<size_t> filtered_apps;

static bool no_matches;
static bool draw_all_apps;

static off_t lcursor;
static off_t visible_start_idx;
static off_t visible_end_idx;

static bool lcursor_visible;

static float scroll_offset;

static size_t apps_len;

static bool backspace_repeat_active = false;
static double last_backspace_press_time = 0.0;

static bool n_repeat_active = false;
static double last_n_press_time = 0.0;

static bool p_repeat_active = false;
static double last_p_press_time = 0.0;

static inline const app_t &get_app(size_t idx)
{
  return draw_all_apps ? apps[idx] : apps[filtered_apps[idx]];
}

static inline void filter_apps(void)
{
  if (!input.empty()) {
    filtered_apps.clear();
    for (size_t i = 0; i < apps.size(); ++i) {
      const auto &[name, exec] = apps[i];
      if (name.find(input) != std::string::npos) {
        filtered_apps.emplace_back(i);
      }
    }

    no_matches = filtered_apps.empty();
  } else {
    no_matches = false;
    filtered_apps.clear();
  }
}

static inline void handle_backspace(void)
{
  if (!input.empty()) {
    input.pop_back();
    filter_apps();
  }
}

static inline void go_prev(void)
{
  if (!lcursor_visible) {
    lcursor = visible_start_idx;
  } else {
    lcursor = std::max(0, (int)lcursor - 1);
    if (lcursor < visible_start_idx) {
      scroll_offset -= LINE_H;
    }
  }
}

static inline void go_next(void)
{
  if (!lcursor_visible) {
    lcursor = visible_start_idx;
  } else {
    lcursor = std::min((off_t) apps_len - 1, lcursor + 1);
    if (lcursor > visible_end_idx) {
      scroll_offset += LINE_H;
    }
  }
}

static inline void handle_key_repeat(bool key_down,
                                     bool key_pressed,
                                     double &last_press_time,
                                     bool &repeat_active,
                                     void (*action)(void))
{
  const auto time = GetTime();

  if (key_down) {
    if (!repeat_active) {
      if (time - last_press_time > INITIAL_KEY_DELAY) {
        repeat_active = true;
      }
    } else {
      if (time - last_press_time > REPEAT_KEY_INTERVAL) {
        last_press_time = time;
        action();
      }
    }
  } else {
    repeat_active = false;
  }

  if (key_pressed) {
    last_press_time = time;
    action();
  }
}

static bool handle_keys(void)
{
  auto ch = GetCharPressed();
  while (ch > 0) {
    if (ch >= 32 && ch <= 125) {
      input += tolower((char) (ch));
    }

    ch = GetCharPressed();
    filter_apps();
  }

  visible_start_idx = (int) (scroll_offset / LINE_H);
  visible_end_idx = (int) ((scroll_offset + (WINDOW_H - PROMPT_H - LINE_H)) / LINE_H);
  lcursor_visible = (lcursor >= visible_start_idx && lcursor <= visible_end_idx);

  handle_key_repeat(IsKeyDown(KEY_BACKSPACE),
                    IsKeyPressed(KEY_BACKSPACE),
                    last_backspace_press_time,
                    backspace_repeat_active,
                    handle_backspace);

  if (IsKeyDown(KEY_LEFT_CONTROL) or IsKeyDown(KEY_CAPS_LOCK)) {
    handle_key_repeat(IsKeyDown(KEY_N),
                      IsKeyPressed(KEY_N),
                      last_n_press_time,
                      n_repeat_active,
                      go_next);

    handle_key_repeat(IsKeyDown(KEY_P),
                      IsKeyPressed(KEY_P),
                      last_p_press_time,
                      p_repeat_active,
                      go_prev);
  }

  if (IsKeyPressed(KEY_ENTER)) {
    const auto &exec = get_app(lcursor).exec;
    launch_application(exec);
    return true;
  }

  return false;
}

static void parse_apps(void)
{
  size_t apps_count = 0;
  std::unordered_set<std::string> seen_names;

  for (const auto &dir: {
    "/usr/share/applications",
    "/usr/local/share/applications",
    "~/.local/share/applications"
  }) {
    auto path = fs::absolute(fs::path(dir));
    if (!fs::is_directory(path)) continue;
    for (const auto &e: fs::directory_iterator(path)) {
      if (e.path().extension() == ".desktop") {
        auto ok = true;
        auto [name, exec] = app_t::parse(e.path().c_str(), &ok);
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        if (ok && !name.empty() && !exec.empty()) {
          if (seen_names.count(name) == 0) {
            apps.emplace_back(name, exec);
            seen_names.insert(name);
            filtered_apps.emplace_back(apps_count);
            apps_count++;
          }
        }
      }
    }
  }
}

int main(void)
{
  SetTargetFPS(60);
  SetConfigFlags(FLAG_MSAA_4X_HINT);
  InitWindow(WINDOW_W, WINDOW_H, "rapp");
  SetTargetFPS(GetMonitorRefreshRate(GetCurrentMonitor()));

  const int m = GetCurrentMonitor();
  const int monitor_w = GetMonitorWidth(m), monitor_h = GetMonitorHeight(m);

  SetWindowPosition((monitor_w - WINDOW_W) / 2, (monitor_h - WINDOW_H) / 2);

  parse_apps();

  input.reserve(256);

  while (!WindowShouldClose()) {
    apps_len = draw_all_apps ? apps.size() : filtered_apps.size();
    draw_all_apps = filtered_apps.empty() && !no_matches;

    if (handle_keys()) goto end;

    // handle mouse wheel
    {
      scroll_offset -= GetMouseWheelMove() * SCROLL_SPEED;
      scroll_offset = std::max(scroll_offset, 0.0f);
      scroll_offset = std::min(scroll_offset, (float) ((apps_len * LINE_H) - (WINDOW_H - PROMPT_H) + PADDING));
    }

    BeginDrawing();
    ClearBackground(BACKGROUND_COLOR);

    DrawRectangle(0, 0, WINDOW_W, PROMPT_H, PROMPT_BACKGROUND_COLOR);

    const char *prompt = "search: ";
    auto prompt_text_color = TEXT_COLOR;

    if (!input.empty()) {
      prompt = input.c_str();
      prompt_text_color = RAYWHITE;
    }

    DrawText(prompt, PADDING, (PROMPT_H - FONT_SIZE) / 2, FONT_SIZE, prompt_text_color);

    int y = PROMPT_H + PADDING - (int) ((int) scroll_offset % LINE_H);

    if (no_matches) {
      DrawRectangle(0, y, WINDOW_W, LINE_H, BACKGROUND_COLOR);
      DrawText("[no matches]", PADDING, y, FONT_SIZE, TEXT_COLOR);
    } else {
	    const int start_idx = std::max(0, (int) (scroll_offset / LINE_H));
	    const int end_idx = std::min((int) (apps_len), (int) ((scroll_offset + (WINDOW_H - PROMPT_H)) / LINE_H));
	
	    for (int i = start_idx; i < end_idx; ++i) {
	      const auto &[name, exec] = get_app(i);
	      const auto hovered = GetMouseY() > y && GetMouseY() < y + LINE_H;
	      if (lcursor == i or hovered) {
	        DrawRectangle(0, y, WINDOW_W, LINE_H, HIGHLIGHT_COLOR);
	        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
	          launch_application(exec);
	          goto end;
	        }
	      }
	
	      DrawText(name.c_str(), PADDING, y, FONT_SIZE, TEXT_COLOR);
	      y += LINE_H;
	    }
    }

    if (apps_len * LINE_H > (WINDOW_H - PROMPT_H)) {
      const float scrollbar_h = (WINDOW_H - PROMPT_H) / (float) (apps_len * LINE_H) * (WINDOW_H - PROMPT_H);
      const float scrollbar_y = scroll_offset / (float) ((apps_len * LINE_H) - (WINDOW_H - PROMPT_H)) * ((WINDOW_H - PROMPT_H) - scrollbar_h);
      DrawRectangle(WINDOW_W - 20, PROMPT_H + scrollbar_y, 10, scrollbar_h, SCROLLBAR_COLOR);
    }

    EndDrawing();
  }

end:
  CloseWindow();

  return 0;
}

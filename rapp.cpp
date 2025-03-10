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
    if (offset < 0 || (size_t) offset >= size) [[unlikely]] {
      assert(0 && "unreachable");
    }
    return ptr[offset];
  };

  constexpr ~file_t(void)
  {
    if (ptr == 0 || size == 0) return;

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
  const std::string name, exec;

  ~app_t(void) = default;
  
  static const app_t parse(const char *file_path, bool *ok);
};

std::vector<std::string_view>
split(const char *str, size_t size, char delim);

constexpr std::array<const char *, 3> SEARCH_PATHS = {
  "/usr/share/applications",
  "/usr/local/share/applications",
  "~/.local/share/applications"
};

constexpr Color TEXT_COLOR        = {209, 184, 151, 255};
constexpr Color ACCENT_COLOR      = {100, 150, 170, 255};
constexpr Color HIGHLIGHT_COLOR   = { 30,  50,  57, 255};
constexpr Color SCROLLBAR_COLOR   = { 50,  70,  80, 255};
constexpr Color BACKGROUND_COLOR  = {  6,  35,  41, 255};

static std::string input_text;
static std::vector<app_t> apps;
static std::vector<size_t> filtered_apps;

const file_t file_t::read(const char *file_path, bool *ok)
{
  int fd = open(file_path, O_RDONLY, (mode_t) 0400);
  if (fd == -1) {
    *ok = false;
    return file_t{};
  }

  struct stat file_info = {0};
  if (fstat(fd, &file_info) == -1) {
    *ok = false;
    return file_t{};
  }

  const off_t size = file_info.st_size;
  if (size == 0) {
    return file_t{};
  }

  char *ptr = (char *) mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
  if (ptr == MAP_FAILED) {
    close(fd);
    *ok = false;
    return file_t{};
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
    return app_t{};
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

void filter_apps(void)
{
  if (!input_text.empty()) {
    filtered_apps.clear();
    std::string lower_input = input_text;
    std::transform(lower_input.begin(), lower_input.end(), lower_input.begin(), ::tolower);
    for (size_t i = 0; i < apps.size(); ++i) {
      const auto &[name, exec] = apps[i];
      std::string lower_name = name;
      std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
      if (lower_name.find(lower_input) != std::string::npos) {
        filtered_apps.emplace_back(i);
      }
    }
  } else {
    filtered_apps.clear();
    for (size_t i = 0; i < apps.size(); ++i) {
      filtered_apps.emplace_back(i);
    }
  }
}

int main(void)
{
  size_t apps_count = 0;
  std::unordered_set<std::string> seen_names;

  for (const auto &dir: SEARCH_PATHS) {
    auto path = fs::absolute(fs::path(dir));
    if (!fs::is_directory(path)) continue;
    for (const auto &e: fs::directory_iterator(path)) {
      if (e.path().extension() == ".desktop") {
        auto ok = true;
        const auto [name, exec] = app_t::parse(e.path().c_str(), &ok);
        if (ok && !name.empty() && !exec.empty()) {
          const app_t app{name, exec};
          // NOTE: this is slow asf
          if (seen_names.count(name) == 0) {
            apps.emplace_back(app);
            seen_names.insert(name);
            filtered_apps.emplace_back(apps_count);
            apps_count++;
          }
        }
      }
    }
  }

  constexpr int w = 800, h = 600;

  SetTargetFPS(60);
  SetConfigFlags(FLAG_MSAA_4X_HINT);
  InitWindow(w, h, "rapp");
  SetTargetFPS(GetMonitorRefreshRate(GetCurrentMonitor()));

  int m = GetCurrentMonitor();
  int mw = GetMonitorWidth(m), mh = GetMonitorHeight(m);

  SetWindowPosition((mw - w) / 2, (mh - h) / 2);

  constexpr int padding = 20;
  constexpr int font_size = 20;
  constexpr int line_h = font_size + 10;

  float scroll_offset = 0.0f;
  constexpr float scroll_speed = 50.0f;

  constexpr float prompt_h = 40.0f;
  constexpr Color PROMPT_BACKGROUND_COLOR = {30, 30, 30, 255};

  input_text.reserve(256);

  while (!WindowShouldClose()) {
    auto key = GetCharPressed();
    while (key > 0) {
      if (key >= 32 && key <= 125) {
        input_text += (char) (key);
      }

      key = GetCharPressed();
      filter_apps();
    }

    if (IsKeyPressed(KEY_BACKSPACE) && !input_text.empty()) {
      input_text.pop_back();
      filter_apps();
    }

    const auto apps_len = filtered_apps.size();

    scroll_offset -= GetMouseWheelMove() * scroll_speed;
    scroll_offset = std::max(scroll_offset, 0.0f);
    scroll_offset = std::min(scroll_offset, (float) ((apps_len * line_h) - (h - prompt_h) + padding));

    BeginDrawing();
    ClearBackground(BACKGROUND_COLOR);

    DrawRectangle(0, 0, w, prompt_h, PROMPT_BACKGROUND_COLOR);

    const char *prompt = "search: ";
    auto prompt_text_color = TEXT_COLOR;

    if (!input_text.empty()) {
      prompt = input_text.c_str();
      prompt_text_color = RAYWHITE;
    }

    DrawText(prompt, padding, (prompt_h - font_size) / 2, font_size, prompt_text_color);

    const int start_idx = std::max(0, (int) (scroll_offset / line_h));
    const int end_idx = std::min((int) (apps_len), (int) ((scroll_offset + (h - prompt_h)) / line_h));

    int y = prompt_h + padding - (int) ((int) scroll_offset % line_h);
    for (int i = start_idx; i < end_idx; ++i) {
      const auto &[name, exec] = apps[filtered_apps[i]];
      if (GetMouseY() > y && GetMouseY() < y + line_h) {
        DrawRectangle(0, y, w, line_h, HIGHLIGHT_COLOR);

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
          launch_application(exec);
          goto end;
        }
      }

      DrawText(name.c_str(), padding, y, font_size, TEXT_COLOR);
      y += line_h;
    }

    if (apps_len * line_h > (h - prompt_h)) {
      const float scrollbar_h = (h - prompt_h) / (float) (apps_len * line_h) * (h - prompt_h);
      const float scrollbar_y = scroll_offset / (float) ((apps_len * line_h) - (h - prompt_h)) * ((h - prompt_h) - scrollbar_h);
      DrawRectangle(w - 20, prompt_h + scrollbar_y, 10, scrollbar_h, SCROLLBAR_COLOR);
    }

    EndDrawing();
  }

end:
  CloseWindow();

  return 0;
}

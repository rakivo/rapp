#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <tuple>
#include <array>
#include <vector>
#include <iostream>
#include <filesystem>
#include <string_view>

#include "raylib.h"

namespace fs = std::filesystem;

#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define shift(argc, argv) (assert(argc), argc--, *argv++)

constexpr std::array<const char *, 3> SEARCH_PATHS = {
  "/usr/share/applications",
  "/usr/local/share/applications",
  "~/.local/share/applications"
};

constexpr Color TEXT_COLOR        = {209, 184, 151, 255};
constexpr Color ACCENT_COLOR      = {100, 150, 170, 255};
constexpr Color HIGHLIGHT_COLOR    = {30, 50, 57, 255};
constexpr Color SCROLLBAR_COLOR    = {50, 70, 80, 255};
constexpr Color BACKGROUND_COLOR  = {6, 35, 41, 255};

struct file_t {
  char *ptr;
  off_t size;

  inline constexpr file_t(void) noexcept
    : ptr(NULL), size(0) {}

  inline constexpr file_t(char *ptr, off_t size) noexcept
    : ptr(ptr), size(size) {}

  inline constexpr char
  operator[](off_t offset) const noexcept
  {
    if (offset < 0 || offset >= size) [[unlikely]] {
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
};

static std::vector<file_t> files = {};
static std::vector<std::pair<std::string, std::string>> apps = {};

file_t read_file(const char *file_path, bool *ok)
{
  int fd = open(file_path, O_RDONLY, (mode_t) 0400);
  if (fd == -1) {
    *ok = false;
    return file_t {};
  }

  struct stat file_info = {0};
  if (fstat(fd, &file_info) == -1) {
    *ok = false;
    return file_t {};
  }

  const off_t size = file_info.st_size;
  if (size == 0) {
    return file_t {};
  }

  char *ptr = (char *) mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
  if (ptr == MAP_FAILED) {
    close(fd);
    *ok = false;
    return file_t {};
  }

  close(fd);

  return file_t(ptr, size);
}

std::vector<std::string_view>
split(const char *str, off_t size, char delim)
{
  std::vector<std::string_view> ret = {};

  const char *start = str, *ptr = str, *end = str + size;
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

void parse_desktop_file(const char *file_path)
{
  bool ok = true;
  file_t file = read_file(file_path, &ok);

  if (file.size == 0) {
    if (!ok) {
      eprintf("could not read file: %s\n", file_path);
    }
    return;
  }

  std::string app_name, exec;
  for (auto &line: split(file.ptr, file.size, '\n')) {
    if (line.find("Name=") == 0) {
      app_name = line.substr(5);
    } else if (line.find("Exec=") == 0) {
      exec = line.substr(5);
    }
  }

  if (!app_name.empty() && !exec.empty()) {
    apps.emplace_back(app_name, exec);
  }

  files.emplace_back(file);
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
  for (char c : cleaned_command) {
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

int main(void)
{
  for (const auto &dir: SEARCH_PATHS) {
    auto path = fs::absolute(fs::path(dir));
    if (!fs::is_directory(path)) continue;
    for (const auto &e: fs::directory_iterator(path)) {
      if (e.path().extension() == ".desktop") {
        parse_desktop_file(e.path().c_str());
      }
    }
  }

  const int w = 800, h = 600;

  SetTargetFPS(60);
  SetConfigFlags(FLAG_MSAA_4X_HINT);
  InitWindow(w, h, "rapp");
  SetTargetFPS(GetMonitorRefreshRate(GetCurrentMonitor()));

  int m = GetCurrentMonitor();
  int mw = GetMonitorWidth(m), mh = GetMonitorHeight(m);

  SetWindowPosition((mw - w) / 2, (mh - h) / 2);

  const int padding = 20;
  const int font_size = 20;
  const int line_h = font_size + 10;

  float scroll_offset = 0.0f;
  const float scroll_speed = 20.0f;

  while (!WindowShouldClose()) {
    scroll_offset -= GetMouseWheelMove() * scroll_speed;
    scroll_offset = std::max(scroll_offset, 0.0f);
    scroll_offset = std::min(scroll_offset, (float) ((apps.size() * line_h) - h + padding));

    BeginDrawing();
    ClearBackground(BACKGROUND_COLOR);

    int start_idx = std::max(0, (int) (scroll_offset / line_h));
    int end_idx = std::min((int) (apps.size()), (int) ((scroll_offset + h) / line_h));

    int y = padding - (int) (scroll_offset) + start_idx * line_h;
    for (int i = start_idx; i < end_idx; ++i) {
      const auto &[app_name, exec] = apps[i];
      if (GetMouseY() > y && GetMouseY() < y + line_h) {
        DrawRectangle(0, y, w, line_h, HIGHLIGHT_COLOR);

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
          launch_application(exec);
          goto end;
        }
      }

      DrawText(app_name.c_str(), padding, y, font_size, TEXT_COLOR);
      y += line_h;
    }

    if (apps.size() * line_h > h) {
      float scrollbar_h = h / (float) (apps.size() * line_h) * h;
      float scrollbar_y = scroll_offset / (float) ((apps.size() * line_h) - h) * (h - scrollbar_h);
      DrawRectangle(w - 20, scrollbar_y, 10, scrollbar_h, SCROLLBAR_COLOR);
    }

    EndDrawing();
  }

end:
  CloseWindow();

  return 0;
}

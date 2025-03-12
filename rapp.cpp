#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>

#define Font XFont
  #include <X11/Xlib.h>
  #include <X11/Xatom.h>
#undef Font

#include <vector>
#include <filesystem>
#include <string_view>
#include <unordered_set>

#include "raylib.h"
#include "font.h"
#include "prompt-font.h"

namespace fs = std::filesystem;

#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define shift(argc, argv) (assert(argc), argc--, *argv++)

struct file_t {
  const std::string_view sv;
  size_t size;

  inline constexpr
	file_t(void) noexcept
    : sv(""), size(0) {}

  inline constexpr
	file_t(const std::string_view sv, off_t size) noexcept
    : sv(sv), size(size) {}

  inline constexpr char
  operator[](off_t offset) const noexcept
  {
    return sv[offset];
  };

  inline constexpr ~file_t(void)
  {
    char *ptr = const_cast<char *>(sv.data());

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

constexpr Color TEXT_COLOR              = {209, 184, 151, 0xFF};
constexpr Color PCURSOR_COLOR           = {209, 184, 151, 0xAA};
constexpr Color ACCENT_COLOR            = {100, 150, 170, 0xFF};
constexpr Color HIGHLIGHT_COLOR         = { 30,  50,  57, 0xFF};
constexpr Color SCROLLBAR_COLOR         = { 50,  70,  80, 0xFF};
constexpr Color BACKGROUND_COLOR        = {  6,  35,  41, 0xFF};
constexpr Color PROMPT_BACKGROUND_COLOR = { 30,  30,  30, 0xFF};

constexpr int PADDING = 20;
constexpr float SPACING = 1.0;

constexpr int FONT_SIZE = 30;
constexpr int PROMPT_FONT_SIZE = 22;

constexpr int WINDOW_W = 800;
constexpr int WINDOW_H = 600;
constexpr float PROMPT_H = 40.0;
constexpr int LINE_H = FONT_SIZE + 10;
constexpr int PCURSOR_W = PROMPT_FONT_SIZE / 2;
constexpr int PCURSOR_H = PROMPT_FONT_SIZE / 0.9;

constexpr float SCROLL_SPEED = 50.0;
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

static inline std::vector<std::string_view>
split(const std::string_view &sv, char delim)
{
  std::vector<std::string_view> ret;
  size_t start = 0, pos = sv.find(delim);
  while (pos != std::string::npos) {
    ret.emplace_back(sv.data() + start, pos - start);
    start = pos + 1;
    pos = sv.find(delim, start);
  }

  if (start < sv.size()) {
    ret.emplace_back(sv.data() + start, sv.size() - start);
  }

  return ret;
}

const app_t app_t::parse(const char *file_path, bool *ok)
{
  auto ok_ = true;
  const auto file = file_t::read(file_path, &ok_);

  if (file.size == 0) {
    if (!ok_) {
      eprintf("could not read file: %s\n", file_path);
    }
    *ok = ok_;
    return {};
  }

  std::string exec, name;
  for (const auto &line: split(file.sv, '\n')) {
    if (!name.empty() && !exec.empty()) break;

    if (line.find("Name=") == 0) {
      name = line.substr(5);
    } else if (line.find("Exec=") == 0) {
      exec = line.substr(5);
    }
  }

  return app_t{name, exec};
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
	args.reserve(cleaned_command.size());

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

static Window window;
static Display *display;

static std::string prompt;

static std::vector<app_t> apps;
static std::vector<size_t> filtered_apps;

static bool no_matches, draw_all_apps;

static size_t lcursor, pcursor;
static size_t visible_start_idx, visible_end_idx;

static bool lcursor_visible;

static float scroll_offset;

static size_t apps_len;

#define KEYS_OR X(KEY_A) | X(KEY_E) | X(KEY_B) | X(KEY_F) | X(KEY_P) | X(KEY_N)
#define ACTIONS X(delete) X(paste) X(delete_word) X(start) X(end) X(left) X(right) X(prev) X(next)
#define MOVEMENTS X(KEY_A, start) X(KEY_E, end) X(KEY_B, left) X(KEY_F, right) X(KEY_P, prev) X(KEY_N, next)

#define DEFINE_REPEAT_KEY(action) \
	static bool pcursor_##action##_repeat_active; \
	static double last_pcursor_##action##_press_time;

#define X DEFINE_REPEAT_KEY
	ACTIONS
#undef X

static inline const app_t &get_app(size_t idx)
{
  return no_matches ? apps[idx] : apps[filtered_apps[idx]];
}

static inline void filter_apps(void)
{
  if (!prompt.empty()) {
    filtered_apps.clear();
    for (size_t i = 0; i < apps.size(); ++i) {
      const auto &[name, exec] = apps[i];
      if (name.find(prompt) != std::string::npos) {
        filtered_apps.emplace_back(i);
      }
    }

    no_matches = filtered_apps.empty();
  } else {
    no_matches = false;
    filtered_apps.clear();
  }
}

static std::string_view get_clipboard(bool *ok)
{
  Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
  Atom UTF8_string = XInternAtom(display, "UTF8_STRING", False);
  Atom target_property = XInternAtom(display, "XSEL_DATA", False);

  Window owner = XGetSelectionOwner(display, clipboard);
  if (!owner) {
    eprintf("no clipboard owner\n");
    XCloseDisplay(display);
		*ok = false;
    return {};
  }

  XConvertSelection(display, clipboard, UTF8_string, target_property, window, CurrentTime);
  XFlush(display);

  XEvent event;
  auto success = false;
  std::string_view ret;
  while (true) {
    XNextEvent(display, &event);
    if (event.type == SelectionNotify && event.xselection.selection == clipboard && event.xselection.property) {
      Atom type;
      int format;
      unsigned long itemCount, bytesAfter;
      unsigned char *data = NULL;

      XGetWindowProperty(display, window, target_property, 0, ~0, False,
                         AnyPropertyType, &type, &format, &itemCount,
                         &bytesAfter, &data);

      if (data && type == UTF8_string) {
        ret = (char *) data;
        success = true;
      }
    }
    break;
  }

  if (!success) {
    eprintf("failed to retrieve clipboard text\n");
		*ok = false;
    return {};
  }

  return ret;
}

static inline void pcursor_paste(void)
{
	auto ok = true;
	const auto clipboard = get_clipboard(&ok);
	if (ok) {
		prompt.insert(pcursor, clipboard);
		pcursor += clipboard.size();
		free(const_cast<char *>(clipboard.data()));
		filter_apps();
	}
}

static inline void pcursor_delete(void)
{
  if (!prompt.empty()) {
		if (pcursor == prompt.size()) {
	    prompt.pop_back();
		} else {
			prompt.erase(pcursor, 1);
		}

		pcursor--;
    filter_apps();
  }
}

static inline void pcursor_delete_word(void)
{
	int r = pcursor;
	while (r --> 1) {
		if (isspace(prompt[r])) break;
	}
	prompt.erase(r, pcursor);
	pcursor = r;
  filter_apps();
}

static inline void pcursor_start(void)
{
	pcursor ^= pcursor;
}

static inline void pcursor_end(void)
{
	pcursor = prompt.size();
}

static inline void pcursor_left(void)
{
	if (pcursor > 0) pcursor--;
}

static inline void pcursor_right(void)
{
	if (pcursor < prompt.size()) pcursor++;
}

static inline void pcursor_prev(void)
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

static inline void pcursor_next(void)
{
  if (!lcursor_visible) {
    lcursor = visible_start_idx;
  } else {
    lcursor = std::min(apps_len - 1, lcursor + 1);
    if (lcursor > visible_end_idx) {
      scroll_offset += LINE_H;
    }
  }
}

static inline void handle_key_repeat(int key,
                                     double &last_press_time,
                                     bool &repeat_active,
                                     void (*action)(void))
{
  const auto time = GetTime();
  const auto key_down = IsKeyDown(key);
  const auto key_pressed = IsKeyPressed(key);

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
      prompt += tolower((char) (ch));
    }

    ch = GetCharPressed();
    filter_apps();

		if (pcursor == 256) {
			pcursor = 1;
		} else {
			pcursor++;
		}
  }

  visible_start_idx = (size_t) (scroll_offset / LINE_H);
  visible_end_idx = (size_t) ((scroll_offset + (WINDOW_H - PROMPT_H - LINE_H)) / LINE_H);
  lcursor_visible = (lcursor >= visible_start_idx && lcursor <= visible_end_idx);

#define HANDLE_KEY_REPEAT(key, action) \
  handle_key_repeat(key, \
                    last_pcursor_##action##_press_time, \
                    pcursor_##action##_repeat_active, \
                    pcursor_##action);

	HANDLE_KEY_REPEAT(KEY_BACKSPACE, delete);

  if (IsKeyDown(KEY_LEFT_CONTROL) or IsKeyDown(KEY_CAPS_LOCK)) {
		HANDLE_KEY_REPEAT(KEY_Y, paste);
		HANDLE_KEY_REPEAT(KEY_BACKSPACE, delete_word);
#define X HANDLE_KEY_REPEAT
		MOVEMENTS
#undef X
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
      if (e.path().extension() != ".desktop") continue;

      auto ok = true;
      auto [name, exec] = app_t::parse(e.path().c_str(), &ok);

			for (auto &c: name) c = tolower(c);

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

int main(void)
{
  display = XOpenDisplay(NULL);
	window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);

  if (!display) {
    eprintf("could not open X display\n");
    return 1;
  }

  SetTargetFPS(60);
  SetConfigFlags(FLAG_MSAA_4X_HINT);
  InitWindow(WINDOW_W, WINDOW_H, "rapp");
  SetTargetFPS(GetMonitorRefreshRate(GetCurrentMonitor()));

  const Font font = LoadFont_Default();
  const Font prompt_font = LoadFont_Prompt();

  const int m = GetCurrentMonitor();
  const int monitor_w = GetMonitorWidth(m), monitor_h = GetMonitorHeight(m);

  SetWindowPosition((monitor_w - WINDOW_W) / 2, (monitor_h - WINDOW_H) / 2);

  parse_apps();

  prompt.reserve(256);

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

    const char *prompt_text = "search: ";
    auto prompt_text_color = TEXT_COLOR;

    if (!prompt.empty()) {
      prompt_text = prompt.c_str();
      prompt_text_color = RAYWHITE;
    }

		const auto mid_prompt_y = (PROMPT_H - PROMPT_FONT_SIZE) / 2;

		// cursor
		DrawRectangle(PADDING + PCURSOR_W * pcursor, mid_prompt_y, PCURSOR_W, PCURSOR_H, PCURSOR_COLOR);

    DrawTextEx(prompt_font, prompt_text, {PADDING, mid_prompt_y}, PROMPT_FONT_SIZE, SPACING, prompt_text_color);

    int y = PROMPT_H + PADDING / 3;

    if (no_matches) {
      DrawRectangle(0, y, WINDOW_W, LINE_H, BACKGROUND_COLOR);
      DrawTextEx(font, "[no matches]", {PADDING, (float) y}, FONT_SIZE, SPACING, TEXT_COLOR);
    } else {
	    const int start_idx = std::max(0, (int) (scroll_offset / LINE_H));
	    const int end_idx = std::min((int) apps_len, (int) ((scroll_offset + (WINDOW_H - PROMPT_H)) / LINE_H));
	
	    for (int i = start_idx; i < end_idx; ++i) {
	      const auto &[name, exec] = get_app(i);
	      const auto hovered = GetMouseY() > y && GetMouseY() < y + LINE_H;
	      if (lcursor == (size_t) i or hovered) {
	        DrawRectangle(0, y - PADDING / 3, WINDOW_W, LINE_H, HIGHLIGHT_COLOR);
	        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
	          launch_application(exec);
	          goto end;
	        }
	      }
	
        DrawTextEx(font, name.c_str(), {PADDING, (float) y}, FONT_SIZE, SPACING, TEXT_COLOR);
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
	XDestroyWindow(display, window);
  XCloseDisplay(display);

  return 0;
}

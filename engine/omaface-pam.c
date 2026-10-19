// omaface-pam — the pam_exec side of omaFace.
//
// Exit status is the whole contract with PAM: 0 means the face matched, any
// other status means "not by face" and the stack moves on to the password.
// It only returns 0 when the root-owned omaface.socket answers "RESULT ok";
// every error, timeout or doubt falls through to the password.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SOCKET_PATH "/run/omaface.sock"
#define DEADLINE_MS 20000
// Waiting for the person to choose omaFace or the password (the engine's
// timeout_choose is at most 120 s).
#define CHOOSE_DEADLINE_MS 125000

static int tty_fd = -1;
static struct termios tty_saved;
static int tty_changed = 0;

static void restore_tty(void) {
  if (tty_changed) {
    tcflush(tty_fd, TCIFLUSH);
    tcsetattr(tty_fd, TCSANOW, &tty_saved);
    tty_changed = 0;
  }
}

static void on_signal(int sig) {
  (void)sig;
  restore_tty();
  _exit(1);
}

static void say(const char* text, int final) {
  if (tty_fd < 0) return;
  dprintf(tty_fd, "\r\033[K\033[2momaFace\033[0m  %s%s", text, final ? "\r\n" : "");
}

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// A face is something you show to the machine you sit at; refuse anything
// that arrived over the network.
static int launched_remotely(void) {
  const char* rhost = getenv("PAM_RHOST");
  if (rhost && *rhost) return 1;
  pid_t pid = getppid();
  for (int depth = 0; pid > 1 && depth < 64; depth++) {
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    char* open_paren = strchr(buf, '(');
    char* close_paren = strrchr(buf, ')');
    if (!open_paren || !close_paren) return 0;
    *close_paren = 0;
    const char* comm = open_paren + 1;
    if (!strcmp(comm, "sshd") || !strncmp(comm, "sshd-", 5) || !strcmp(comm, "mosh-server") ||
        !strcmp(comm, "tailscaled") || !strcmp(comm, "telnetd") || !strcmp(comm, "in.telnetd"))
      return 1;
    int ppid = 0;
    char state;
    if (sscanf(close_paren + 1, " %c %d", &state, &ppid) != 2) return 0;
    pid = ppid;
  }
  return 0;
}

// At the login screen pam_exec hands over whatever was typed (expose_authtok).
// A typed password means the person chose the password: skip the camera.
static int password_was_typed(void) {
  struct pollfd p = {STDIN_FILENO, POLLIN, 0};
  if (poll(&p, 1, 300) <= 0) return 0;
  char c = 0;
  ssize_t n = read(STDIN_FILENO, &c, 1);
  int typed = n == 1 && c != '\0' && c != '\n';
  c = 0;
  // Drain the rest so nothing lingers in the pipe buffer.
  char sink[256];
  while (poll(&p, 1, 0) > 0 && read(STDIN_FILENO, sink, sizeof sink) > 0) {}
  explicit_bzero(sink, sizeof sink);
  return typed;
}

int main(void) {
  // Die with whoever asked (lock screen, polkit, sudo), so an aborted prompt
  // never leaves the camera running. Checked again in case the parent was
  // already gone before the death signal was armed.
  pid_t parent = getppid();
  prctl(PR_SET_PDEATHSIG, SIGTERM);
  if (getppid() != parent) return 1;

  const char* user = getenv("PAM_USER");
  const char* service = getenv("PAM_SERVICE");
  const char* type = getenv("PAM_TYPE");
  if (!user || !*user || !service || !*service) return 1;
  if (type && strcmp(type, "auth") != 0) return 1;
  if (strpbrk(user, " \t\r\n") || strpbrk(service, " \t\r\n")) return 1;
  if (strlen(user) > 64 || strlen(service) > 64) return 1;

  if (!strcmp(service, "sddm") && password_was_typed()) return 1;
  if (launched_remotely()) return 1;

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  sigaction(SIGQUIT, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);

  int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (sock < 0) return 1;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, SOCKET_PATH, sizeof addr.sun_path - 1);
  if (connect(sock, (struct sockaddr*)&addr, sizeof addr) != 0) return 1;

  char request[160];
  int len = snprintf(request, sizeof request, "VERIFY %s %s\n", user, service);
  if (len <= 0 || len >= (int)sizeof request) return 1;
  if (send(sock, request, len, MSG_NOSIGNAL) != len) return 1;

  // sudo in a terminal: ask omaFace or password with a single key, show
  // progress, keep typed-ahead keys from echoing in the clear while the
  // camera looks, and let any key skip to the password.
  const char* tty = getenv("PAM_TTY");
  if (!strcmp(service, "sudo") && tty && !strncmp(tty, "/dev/", 5)) {
    tty_fd = open(tty, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (tty_fd >= 0 && isatty(tty_fd) && tcgetattr(tty_fd, &tty_saved) == 0) {
      struct termios quiet = tty_saved;
      quiet.c_lflag &= ~(ECHO | ECHONL | ICANON);
      quiet.c_cc[VMIN] = 1;
      quiet.c_cc[VTIME] = 0;
      if (tcsetattr(tty_fd, TCSANOW, &quiet) == 0) tty_changed = 1;
    } else if (tty_fd >= 0) {
      close(tty_fd);
      tty_fd = -1;
    }
  }

  int ok = 0, done = 0, said_nomatch = 0, said_attention = 0, choosing = 0;
  char buf[512];
  size_t used = 0;
  long deadline = now_ms() + DEADLINE_MS;
  while (!done) {
    long left = deadline - now_ms();
    if (left <= 0) break;
    struct pollfd fds[2] = {{sock, POLLIN, 0}, {tty_fd, POLLIN, 0}};
    int nfds = tty_fd >= 0 && tty_changed ? 2 : 1;
    int r = poll(fds, nfds, (int)left);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;

    if (nfds == 2 && (fds[1].revents & POLLIN)) {
      char key[16];
      ssize_t k = read(tty_fd, key, sizeof key);  // a whole key, escape sequences included
      if (choosing && k > 0 && (key[0] == '\n' || key[0] == '\r')) {
        if (send(sock, "CHOICE face\n", 12, MSG_NOSIGNAL) != 12) break;
        choosing = 0;
        deadline = now_ms() + DEADLINE_MS;
        continue;
      }
      if (choosing) {
        (void)!send(sock, "CHOICE password\n", 16, MSG_NOSIGNAL);
        say("using your password", 1);
      } else {
        say("skipped", 1);
      }
      break;
    }
    if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
    ssize_t n = read(sock, buf + used, sizeof buf - 1 - used);
    if (n <= 0) break;
    used += (size_t)n;
    buf[used] = 0;

    char* line;
    while ((line = memchr(buf, '\n', used)) != NULL) {
      *line = 0;
      if (!strcmp(buf, "STATUS choose")) {
        // Nothing is scanned until the person asks for it.
        choosing = 1;
        deadline = now_ms() + CHOOSE_DEADLINE_MS;
        say("Enter to use your face  \033[2m·  Esc or any other key for your password\033[0m", 0);
      } else if (!strncmp(buf, "STATUS turn-", 12)) {
        // The webcam's head-turn check: each prompt gets its own time.
        deadline = now_ms() + DEADLINE_MS;
        if (!strcmp(buf, "STATUS turn-left")) say("turn your head to the left…", 0);
        else if (!strcmp(buf, "STATUS turn-right")) say("turn your head to the right…", 0);
        else say("now face the camera again…", 0);
      } else if (!strcmp(buf, "STATUS scanning")) {
        say("looking at the camera…  \033[2m(any key for your password)\033[0m", 0);
      }
      else if (!strcmp(buf, "STATUS attention") && !said_attention) {
        say("look at the camera with your eyes open…", 0);
        said_attention = 1;
      } else if (!strcmp(buf, "STATUS nomatch") && !said_nomatch) {
        say("not recognized yet…", 0);
        said_nomatch = 1;
      } else if (!strncmp(buf, "RESULT ", 7)) {
        const char* result = buf + 7;
        ok = !strcmp(result, "ok");
        if (ok) say("\033[32mrecognized\033[0m", 1);
        else if (!strcmp(result, "fail")) say("not recognized", 1);
        else if (!strcmp(result, "attention")) say("eyes closed or not looking at the camera", 1);
        else if (!strcmp(result, "timeout")) say("no face found", 1);
        else if (!strcmp(result, "locked-out")) say("too many attempts, use your password", 1);
        else if (!strcmp(result, "timeout choose")) say("no choice made, using your password", 1);
        else if (!strcmp(result, "busy")) say("camera busy", 1);
        else if (!strncmp(result, "unavailable camera", 18)) say("camera unavailable", 1);
        done = 1;
        break;
      }
      size_t consumed = (size_t)(line - buf) + 1;
      memmove(buf, buf + consumed, used - consumed);
      used -= consumed;
    }
    if (used >= sizeof buf - 1) break;
  }
  if (!done && tty_fd >= 0) say("", 0);
  close(sock);
  restore_tty();
  return ok ? 0 : 1;
}

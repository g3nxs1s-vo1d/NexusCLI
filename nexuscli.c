#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>

#define MAX_SOLVES 10000
#define SCRAMBLE_LEN 160
#define DATA_DIR ".nexuscli"
#define DATA_FILE "solves.tsv"
#define CONFIG_FILE "config.conf"
#define APP_VERSION "0.5.0"

typedef enum { PENALTY_NONE = 0, PENALTY_PLUS2 = 1, PENALTY_DNF = 2 } Penalty;
typedef struct {
    long long ms;
    int penalty;
    long long epoch;
    int session_id;
    int bookmarked;
    char scramble[SCRAMBLE_LEN];
} Solve;

typedef struct {
    int colors;
    char theme[16];
    int inspection_seconds;
    int inspection_dnf_seconds;
    int hold_ms;
    int scramble_length;
    int recent_solves;
} AppConfig;

static AppConfig config;
static int ansi_enabled = 0;
static Solve solves[MAX_SOLVES];
static int solve_count = 0;
static int current_session_id = 1;
static int config_has_session = 0;
static char current_scramble[SCRAMBLE_LEN] = "";
static int running = 0;
static long long start_ns = 0;
static long long stopped_ms = 0;
static int inspection_active = 0;
static long long inspection_start_ns = 0;
static int next_penalty = PENALTY_NONE;
static int space_pending = 0;
static int space_repeat_seen = 0;
static long long space_first_ns = 0;
static long long space_last_ns = 0;
static long long ignore_space_until_ns = 0;
static struct termios original_termios;
static int terminal_raw = 0;
static char data_path[1024];
static char data_dir_path[1024];
static char config_path[1024];

static void restore_terminal(void);
static int save_all(void);
static int save_config(void);
static void wait_for_key(void);
static int setup_terminal(void);

static void config_defaults(void) {
    config.colors = 1;
    snprintf(config.theme, sizeof(config.theme), "cyan");
    config.inspection_seconds = 15;
    config.inspection_dnf_seconds = 17;
    config.hold_ms = 500;
    config.scramble_length = 20;
    config.recent_solves = 5;
}

static const char *color(int role) {
    if (!ansi_enabled) return "";
    if (role == 0) return "\033[0m";
    if (role == 3) return "\033[90m";
    if (role == 4) return "\033[1;32m";
    if (role == 5) return "\033[1;33m";
    if (role == 6) return "\033[1;31m";
    if (role == 7) return "\033[1;34m";
    if (strcmp(config.theme, "green") == 0) return role == 1 ? "\033[1;32m" : "\033[32m";
    if (strcmp(config.theme, "amber") == 0) return role == 1 ? "\033[1;33m" : "\033[33m";
    if (strcmp(config.theme, "blue") == 0) return role == 1 ? "\033[1;34m" : "\033[34m";
    if (strcmp(config.theme, "magenta") == 0) return role == 1 ? "\033[1;35m" : "\033[35m";
    return role == 1 ? "\033[1;36m" : "\033[36m";
}

static void update_ansi_enabled(void) {
    const char *term = getenv("TERM");
    ansi_enabled = config.colors && getenv("NO_COLOR") == NULL &&
                   (!term || strcmp(term, "dumb") != 0);
}

static const char *theme_names[] = {"cyan", "green", "amber", "blue", "magenta"};

static int save_config(void) {
    if (!config_path[0]) return 0;
    char temp_path[1100];
    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp", config_path) >= (int)sizeof(temp_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    FILE *f = fopen(temp_path, "w");
    if (!f) return -1;
    (void)fchmod(fileno(f), 0600);
    int failed = 0;
    if (fprintf(f, "# NexusCLI C configuration\n# Edit these values, or press C in the app for the settings menu.\n") < 0 ||
        fprintf(f, "# colors: 0/1; theme: cyan, green, amber, blue, magenta\n") < 0 ||
        fprintf(f, "colors=%d\ntheme=%s\n", config.colors, config.theme) < 0 ||
        fprintf(f, "inspection_seconds=%d\ninspection_dnf_seconds=%d\n",
                config.inspection_seconds, config.inspection_dnf_seconds) < 0 ||
        fprintf(f, "hold_ms=%d\nscramble_length=%d\nrecent_solves=%d\ncurrent_session_id=%d\n",
                config.hold_ms, config.scramble_length, config.recent_solves, current_session_id) < 0) failed = 1;
    if (fflush(f) != 0) failed = 1;
    if (!failed && fsync(fileno(f)) != 0) failed = 1;
    if (fclose(f) != 0) failed = 1;
    if (failed) {
        int saved_errno = errno ? errno : EIO;
        unlink(temp_path); errno = saved_errno; return -1;
    }
    if (rename(temp_path, config_path) != 0) {
        int saved_errno = errno; unlink(temp_path); errno = saved_errno; return -1;
    }
    return 0;
}

static int parse_config_int(const char *value, int *out) {
    char *end = NULL;
    errno = 0;
    long v = strtol(value, &end, 10);
    while (end && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) end++;
    if (errno || end == value || !end || *end != '\0' || v < INT_MIN || v > INT_MAX) return -1;
    *out = (int)v;
    return 0;
}

static void load_config(void) {
    config_defaults();
    FILE *f = fopen(config_path, "r");
    if (!f) {
        save_config();
        update_ansi_enabled();
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *key = line;
        while (*key == ' ' || *key == '\t') key++;
        if (*key == '#' || *key == '\n' || *key == '\0') continue;
        char *eq = strchr(key, '=');
        if (!eq) continue;
        *eq++ = '\0';
        key[strcspn(key, " \t\r\n")] = '\0';
        while (*eq == ' ' || *eq == '\t') eq++;
        eq[strcspn(eq, "\r\n")] = '\0';
        int v;
        if (strcmp(key, "theme") == 0) {
            for (size_t i = 0; i < sizeof(theme_names) / sizeof(theme_names[0]); i++) {
                if (strcmp(eq, theme_names[i]) == 0) snprintf(config.theme, sizeof(config.theme), "%s", eq);
            }
        } else if (parse_config_int(eq, &v) == 0) {
            if (strcmp(key, "colors") == 0) config.colors = v != 0;
            else if (strcmp(key, "inspection_seconds") == 0) config.inspection_seconds = v;
            else if (strcmp(key, "inspection_dnf_seconds") == 0) config.inspection_dnf_seconds = v;
            else if (strcmp(key, "hold_ms") == 0) config.hold_ms = v;
            else if (strcmp(key, "scramble_length") == 0) config.scramble_length = v;
            else if (strcmp(key, "recent_solves") == 0) config.recent_solves = v;
            else if (strcmp(key, "current_session_id") == 0 && v >= 1 && v <= MAX_SOLVES) {
                current_session_id = v;
                config_has_session = 1;
            }
        }
    }
    fclose(f);
    if (config.inspection_seconds < 0 || config.inspection_seconds > 60) config.inspection_seconds = 15;
    if (config.inspection_dnf_seconds <= config.inspection_seconds || config.inspection_dnf_seconds > 120) config.inspection_dnf_seconds = config.inspection_seconds + 2;
    if (config.hold_ms < 250 || config.hold_ms > 1500) config.hold_ms = 500;
    if (config.scramble_length < 10 || config.scramble_length > 40) config.scramble_length = 20;
    if (config.recent_solves < 0 || config.recent_solves > 10) config.recent_solves = 5;
    save_config();
    update_ansi_enabled();
}


static void restore_terminal(void) {
    if (terminal_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
        terminal_raw = 0;
        printf("\033[?25h\033[0m\n");
        fflush(stdout);
    }
}

static void on_signal(int sig) {
    (void)sig;
    (void)save_all();
    save_config();
    restore_terminal();
    exit(0);
}

static long long monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static long long elapsed_ms(void) {
    if (!running) return stopped_ms;
    return (monotonic_ns() - start_ns) / 1000000LL;
}

static void format_time(long long ms, char *out, size_t size) {
    if (ms < 0) { snprintf(out, size, "DNF"); return; }
    long long centis = ms / 10;
    long long minutes = centis / 6000;
    long long seconds = (centis / 100) % 60;
    long long hundredths = centis % 100;
    if (minutes > 0) snprintf(out, size, "%lld:%02lld.%02lld", minutes, seconds, hundredths);
    else snprintf(out, size, "%lld.%02lld", centis / 100, hundredths);
}

static long long effective_ms(const Solve *s) {
    if (s->penalty == PENALTY_DNF) return -1;
    return s->ms + (s->penalty == PENALTY_PLUS2 ? 2000 : 0);
}

static const char *penalty_name(int p) {
    if (p == PENALTY_PLUS2) return "+2";
    if (p == PENALTY_DNF) return "DNF";
    return "OK";
}

static int save_all(void) {
    char temp_path[1100];
    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp", data_path) >= (int)sizeof(temp_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    FILE *f = fopen(temp_path, "w");
    if (!f) return -1;
    (void)fchmod(fileno(f), 0600);
    int failed = 0;
    for (int i = 0; i < solve_count; i++) {
        Solve *s = &solves[i];
        if (fprintf(f, "%lld\t%lld\t%d\t%d\t%d\t%s\n", s->epoch, s->ms,
                    s->penalty, s->session_id, s->bookmarked, s->scramble) < 0) {
            failed = 1; break;
        }
    }
    if (fflush(f) != 0) failed = 1;
    if (!failed && fsync(fileno(f)) != 0) failed = 1;
    if (fclose(f) != 0) failed = 1;
    if (failed) { int saved_errno = errno ? errno : EIO; unlink(temp_path); errno = saved_errno; return -1; }
    if (rename(temp_path, data_path) != 0) { int saved_errno = errno; unlink(temp_path); errno = saved_errno; return -1; }
    return 0;
}

static void load_all(void) {
    FILE *f = fopen(data_path, "r");
    if (!f) return;
    int max_loaded_session_id = 1;
    char line[512];
    while (fgets(line, sizeof(line), f) && solve_count < MAX_SOLVES) {
        Solve s;
        memset(&s, 0, sizeof(s));
        char *p = line;
        char *end;
        errno = 0;
        s.epoch = strtoll(p, &end, 10); if (errno || end == p || *end != '\t') continue;
        p = end + 1;
        s.ms = strtoll(p, &end, 10); if (errno || end == p || *end != '\t') continue;
        p = end + 1;
        s.penalty = (int)strtol(p, &end, 10); if (errno || end == p || *end != '\t') continue;
        p = end + 1;
        /* Backward compatible with v0.1/v0.2 rows, where scramble followed penalty. */
        char *candidate_end = NULL;
        errno = 0;
        long parsed_session = strtol(p, &candidate_end, 10);
        if (!errno && candidate_end != p && *candidate_end == '\t') {
            s.session_id = (parsed_session >= 1 && parsed_session <= MAX_SOLVES) ? (int)parsed_session : 1;
            p = candidate_end + 1;
            long parsed_bookmark = strtol(p, &candidate_end, 10);
            if (candidate_end != p && *candidate_end == '\t') {
                s.bookmarked = parsed_bookmark != 0;
                p = candidate_end + 1;
            }
        } else {
            s.session_id = 1;
        }
        p[strcspn(p, "\r\n")] = '\0';
        snprintf(s.scramble, sizeof(s.scramble), "%s", p);
        if (s.ms < 0 || s.penalty < 0 || s.penalty > 2) continue;
        if (s.session_id < 1) s.session_id = 1;
        if (s.session_id > max_loaded_session_id) max_loaded_session_id = s.session_id;
        solves[solve_count++] = s;
    }
    fclose(f);
    if (!config_has_session) current_session_id = max_loaded_session_id;
}
static unsigned int random_u32(void) {
    unsigned int v = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, &v, sizeof(v));
        close(fd);
        if (n == (ssize_t)sizeof(v)) return v;
    }
    return (unsigned int)rand();
}

static void make_scramble(void) {
    static const char *faces[] = {"R", "L", "U", "D", "F", "B"};
    static const char *suffix[] = {"", "'", "2"};
    char buf[SCRAMBLE_LEN] = "";
    int last_face = -1;
    for (int i = 0; i < config.scramble_length; i++) {
        int face;
        do { face = (int)(random_u32() % 6); } while (face == last_face);
        last_face = face;
        const char *suf = suffix[random_u32() % 3];
        char move[8];
        snprintf(move, sizeof(move), "%s%s", faces[face], suf);
        if (i) strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, move, sizeof(buf) - strlen(buf) - 1);
    }
    snprintf(current_scramble, sizeof(current_scramble), "%s", buf);
}

static int session_solve_count(int session_id) {
    int count = 0;
    for (int i = 0; i < solve_count; i++) if (solves[i].session_id == session_id) count++;
    return count;
}

static long long average_window(int end_exclusive, int n) {
    if (n < 3 || end_exclusive < n) return -2;
    long long values[100];
    if (n > 100) return -2;
    for (int i = 0; i < n; i++) {
        long long v = effective_ms(&solves[end_exclusive - n + i]);
        values[i] = (v < 0) ? LLONG_MAX : v;
    }
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) {
        if (values[j] < values[i]) { long long t = values[i]; values[i] = values[j]; values[j] = t; }
    }
    int trim = n < 5 ? 0 : (n + 19) / 20;
    long long sum = 0;
    int kept = n - 2 * trim;
    for (int i = trim; i < n - trim; i++) {
        if (values[i] == LLONG_MAX) return -1;
        sum += values[i];
    }
    return kept > 0 ? sum / kept : -2;
}

static long long average_last(int n) {
    return average_window(solve_count, n);
}

static long long best_average(int n, int session_id) {
    long long best = LLONG_MAX;
    int indices[MAX_SOLVES];
    int count = 0;
    for (int i = 0; i < solve_count; i++) {
        if (session_id == 0 || solves[i].session_id == session_id) indices[count++] = i;
    }
    if (count < n || n < 3 || n > 100) return -2;
    long long values[100];
    int trim = n < 5 ? 0 : (n + 19) / 20;
    for (int start = 0; start + n <= count; start++) {
        for (int j = 0; j < n; j++) {
            long long v = effective_ms(&solves[indices[start + j]]);
            values[j] = v < 0 ? LLONG_MAX : v;
        }
        for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) {
            if (values[j] < values[i]) { long long t = values[i]; values[i] = values[j]; values[j] = t; }
        }
        long long sum = 0; int valid = 1;
        for (int j = trim; j < n - trim; j++) {
            if (values[j] == LLONG_MAX) { valid = 0; break; }
            sum += values[j];
        }
        if (valid) {
            long long avg = sum / (n - 2 * trim);
            if (avg < best) best = avg;
        }
    }
    return best == LLONG_MAX ? -2 : best;
}

static long long best_single(void) {
    long long best = LLONG_MAX;
    for (int i = 0; i < solve_count; i++) {
        long long v = effective_ms(&solves[i]);
        if (v >= 0 && v < best) best = v;
    }
    return best == LLONG_MAX ? -2 : best;
}

static long long session_mean(void) {
    long long sum = 0; int count = 0;
    for (int i = 0; i < solve_count; i++) {
        if (solves[i].session_id != current_session_id || solves[i].penalty == PENALTY_DNF) continue;
        sum += effective_ms(&solves[i]); count++;
    }
    return count ? sum / count : -2;
}

static long long session_best_single(void) {
    long long best = LLONG_MAX;
    for (int i = 0; i < solve_count; i++) {
        if (solves[i].session_id != current_session_id) continue;
        long long v = effective_ms(&solves[i]);
        if (v >= 0 && v < best) best = v;
    }
    return best == LLONG_MAX ? -2 : best;
}

static double session_deviation(void) {
    long long sum = 0; int count = 0;
    for (int i = 0; i < solve_count; i++) if (solves[i].session_id == current_session_id && solves[i].penalty != PENALTY_DNF) { sum += effective_ms(&solves[i]); count++; }
    if (count < 2) return 0.0;
    double mean = (double)sum / count, squared = 0.0;
    for (int i = 0; i < solve_count; i++) if (solves[i].session_id == current_session_id && solves[i].penalty != PENALTY_DNF) {
        double d = (double)effective_ms(&solves[i]) - mean; squared += d * d;
    }
    return sqrt(squared / count);
}
static void print_formatted_or_na(long long ms, char *out, size_t size) {
    if (ms == -2) snprintf(out, size, "--");
    else if (ms == -1) snprintf(out, size, "DNF");
    else format_time(ms, out, size);
}

static const char *digit_glyph(char ch, int row) {
    static const char *digits[10][5] = {
        {"###", "# #", "# #", "# #", "###"}, {"  #", "  #", "  #", "  #", "  #"},
        {"###", "  #", "###", "#  ", "###"}, {"###", "  #", "###", "  #", "###"},
        {"# #", "# #", "###", "  #", "  #"}, {"###", "#  ", "###", "  #", "###"},
        {"###", "#  ", "###", "# #", "###"}, {"###", "  #", "  #", "  #", "  #"},
        {"###", "# #", "###", "# #", "###"}, {"###", "# #", "###", "  #", "###"}
    };
    static const char *dot[5] = {"   ", "   ", "   ", "   ", "  #"};
    static const char *colon[5] = {"   ", "  #", "   ", "  #", "   "};
    if (ch >= '0' && ch <= '9') return digits[ch - '0'][row];
    if (ch == '.') return dot[row];
    if (ch == ':') return colon[row];
    return "   ";
}

static void print_big_timer(const char *text) {
    if (strcmp(text, "DNF") == 0) {
        printf("%s      D N F      %s\n", color(6), color(0));
        return;
    }
    if (strspn(text, "0123456789.:") != strlen(text)) {
        printf("      %s%s%s\n", color(5), text, color(0));
        return;
    }
    for (int row = 0; row < 5; row++) {
        printf("      %s", color(running ? 4 : (inspection_active ? 5 : 2)));
        for (const char *p = text; *p; p++) printf("%s ", digit_glyph(*p, row));
        printf("%s\n", color(0));
    }
}

static void print_rule(void) {
    printf("%s+------------------------------------------------------------------------------+%s\n", color(1), color(0));
}

static void print_panel_title(const char *title) {
    printf("%s%s%s\n", color(1), title, color(0));
}

static long long current_session_average(int n) {
    long long idx[MAX_SOLVES];
    int count = 0;
    for (int i = 0; i < solve_count; i++) if (solves[i].session_id == current_session_id) idx[count++] = i;
    if (count < n || n < 3 || n > 100) return -2;
    long long values[100];
    int trim = n < 5 ? 0 : 1;
    for (int j = 0; j < n; j++) {
        long long v = effective_ms(&solves[idx[count - n + j]]);
        values[j] = v < 0 ? LLONG_MAX : v;
    }
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++)
        if (values[j] < values[i]) { long long t = values[i]; values[i] = values[j]; values[j] = t; }
    long long sum = 0;
    for (int i = trim; i < n - trim; i++) {
        if (values[i] == LLONG_MAX) return -1;
        sum += values[i];
    }
    return sum / (n - 2 * trim);
}

static void show_help(void) {
    printf("\033[H\033[J");
    print_rule();
    printf("%s| NEXUSCLI C v%s  /  KEYBOARD GUIDE                                        |%s\n", color(1), APP_VERSION, color(0));
    print_rule();
    printf("\n%sTIMER%s\n", color(2), color(0));
    printf("  Hold Space, wait for the terminal's key-repeat signal, then release: start.\n");
    printf("  Space while running: stop and save. I: inspection. N: random scramble. R: paste one.\n");
    printf("\n%sSOLVES%s\n", color(2), color(0));
    printf("  + / = toggle +2 on latest solve    D toggle DNF    B bookmark\n");
    printf("  M edit any solve (time, penalty, session, bookmark, scramble, delete)\n  X delete latest solve (asks first)\n");
    printf("  U undo latest solve                H history       K bookmarks\n");
    printf("\n%sSESSIONS & ANALYTICS%s\n", color(2), color(0));
    printf("  S new session     G switch session     V session overview\n");
    printf("  A daily activity  P recent-time chart  E export CSV\n");
    printf("\n%sSETTINGS & FILES%s\n", color(2), color(0));
    printf("  C settings menu   ? help               Q quit and save\n");
    printf("  Config: %s\n  Solve data: %s\n", config_path, data_path);
    printf("\n%sNOTES%s\n", color(2), color(0));
    printf("  Scrambles are random-move sequences, not official state-based WCA scrambles.\n");
    printf("  Ordinary terminal input cannot guarantee exact Space key-up timing.\n\n");
    printf("Press any key to return..."); fflush(stdout); wait_for_key();
}

static int read_line_prompt(const char *prompt, char *buf, size_t size) {
    if (!buf || size == 0) return -1;
    if (terminal_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
        terminal_raw = 0;
        printf("\033[?25h\033[0m");
    }
    printf("%s%s%s", color(2), prompt, color(0));
    fflush(stdout);
    if (!fgets(buf, (int)size, stdin)) buf[0] = '\0';
    else {
        size_t got = strlen(buf);
        int has_newline = got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r');
        if (!has_newline && got == size - 1 && !feof(stdin)) {
            int ch; while ((ch = getchar()) != '\n' && ch != EOF) { }
            buf[0] = '\0';
        } else buf[strcspn(buf, "\r\n")] = '\0';
    }
    if (setup_terminal() != 0) return -1;
    return buf[0] ? 0 : -1;
}

static int prompt_int(const char *prompt, int min, int max, int *out) {
    char buf[64]; int v;
    if (read_line_prompt(prompt, buf, sizeof(buf)) != 0 || parse_config_int(buf, &v) != 0 || v < min || v > max) {
        printf("\nInvalid value. Expected %d through %d. Press any key...", min, max); fflush(stdout); wait_for_key(); return -1;
    }
    *out = v; return 0;
}

static void show_config_menu(void) {
    int done = 0;
    while (!done) {
        printf("\033[H\033[J");
        print_rule();
        printf("%s| SETTINGS  /  .nexuscli/config.conf                                           |%s\n", color(1), color(0));
        print_rule();
        printf("\n  1. Colors                 : %s\n", config.colors ? "on" : "off");
        printf("  2. Theme                  : %s\n", config.theme);
        printf("  3. Inspection (seconds)   : %d\n", config.inspection_seconds);
        printf("  4. DNF boundary (seconds) : %d  [+2 applies after inspection until here]\n", config.inspection_dnf_seconds);
        printf("  5. Hold threshold (ms)    : %d  [key-repeat heuristic]\n", config.hold_ms);
        printf("  6. Scramble length        : %d moves\n", config.scramble_length);
        printf("  7. Recent rows            : %d\n", config.recent_solves);
        printf("  8. Reset defaults\n  0. Return\n\nSelect setting: "); fflush(stdout);
        char choicebuf[32];
        if (read_line_prompt("", choicebuf, sizeof(choicebuf)) != 0) { done = 1; continue; }
        int choice;
        if (parse_config_int(choicebuf, &choice) != 0) continue;
        int v;
        if (choice == 0) done = 1;
        else if (choice == 1) { config.colors = !config.colors; update_ansi_enabled(); }
        else if (choice == 2) {
            printf("\nTheme: 1 cyan  2 green  3 amber  4 blue  5 magenta\n");
            if (prompt_int("Choose theme [1-5]: ", 1, 5, &v) == 0) snprintf(config.theme, sizeof(config.theme), "%s", theme_names[v - 1]);
            update_ansi_enabled();
        } else if (choice == 3) {
            if (prompt_int("Inspection seconds [0-60]: ", 0, 60, &v) == 0) {
                config.inspection_seconds = v;
                if (config.inspection_dnf_seconds <= v) config.inspection_dnf_seconds = v + 2;
            }
        } else if (choice == 4) {
            if (prompt_int("DNF boundary; must exceed inspection [1-120]: ", 1, 120, &v) == 0 && v > config.inspection_seconds) config.inspection_dnf_seconds = v;
            else { printf("\nDNF boundary must exceed inspection. Press any key..."); fflush(stdout); wait_for_key(); }
        } else if (choice == 5) { if (prompt_int("Hold threshold ms [250-1500]: ", 250, 1500, &v) == 0) config.hold_ms = v; }
        else if (choice == 6) { if (prompt_int("Scramble length [10-40]: ", 10, 40, &v) == 0) { config.scramble_length = v; make_scramble(); } }
        else if (choice == 7) { if (prompt_int("Recent solves [0-10]: ", 0, 10, &v) == 0) config.recent_solves = v; }
        else if (choice == 8) { config_defaults(); update_ansi_enabled(); make_scramble(); }
        else continue;
        save_config();
    }
}

static int parse_time_input(const char *text, long long *ms_out) {
    if (!text || !*text) return -1;
    if (strchr(text, ':')) {
        long long minutes = 0; double seconds = 0.0; char extra;
        if (sscanf(text, "%lld:%lf%c", &minutes, &seconds, &extra) != 2 || minutes < 0 || !isfinite(seconds) || seconds < 0.0 || seconds >= 60.0) return -1;
        double total = (double)minutes * 60000.0 + seconds * 1000.0;
        if (total < 0.0 || total > 86400000.0) return -1;
        *ms_out = (long long)(total + 0.5); return 0;
    }
    char *end = NULL; errno = 0; double seconds = strtod(text, &end);
    if (errno || end == text || *end != '\0' || !isfinite(seconds) || seconds <= 0.0 || seconds > 86400.0) return -1;
    *ms_out = (long long)(seconds * 1000.0 + 0.5); return 0;
}

static void edit_latest_time(void) {
    if (solve_count == 0 || running) return;
    int displayed = 1;
    if (solve_count > 1 && prompt_int("Solve to edit (1 = newest): ", 1, solve_count, &displayed) != 0) return;
    int idx = solve_count - displayed;
    int done = 0;
    while (!done) {
        Solve *s = &solves[idx];
        char t[32]; format_time(s->ms, t, sizeof(t));
        printf("\033[H\033[J");
        print_rule(); print_panel_title("SOLVE EDITOR"); print_rule();
        printf("\n  Record #%d / newest-first #%d\n", idx + 1, displayed);
        printf("  Raw time: %s   Penalty: %s   Session: #%d   Bookmark: %s\n", t, penalty_name(s->penalty), s->session_id, s->bookmarked ? "yes" : "no");
        printf("  Scramble: %s\n\n", s->scramble);
        printf("  1. Edit time\n  2. Edit penalty (0 OK / 1 +2 / 2 DNF)\n");
        printf("  3. Move to session\n  4. Toggle bookmark\n  5. Edit scramble\n  6. Delete this solve\n  0. Return\n\nSelect field: "); fflush(stdout);
        char input[32]; int choice;
        if (read_line_prompt("", input, sizeof(input)) != 0 || parse_config_int(input, &choice) != 0) continue;
        int v;
        if (choice == 0) done = 1;
        else if (choice == 1) {
            char curr[32], prompt[128], value[64]; long long ms;
            format_time(s->ms, curr, sizeof(curr));
            snprintf(prompt, sizeof(prompt), "New raw time (current %s; seconds or M:SS.cc): ", curr);
            if (read_line_prompt(prompt, value, sizeof(value)) == 0) {
                if (parse_time_input(value, &ms) == 0 && ms > 0) { s->ms = ms; if (save_all() != 0) perror("Could not save edited solve"); }
                else { printf("\nInvalid time. Press any key..."); fflush(stdout); wait_for_key(); }
            }
        } else if (choice == 2) {
            if (prompt_int("Penalty [0=OK, 1=+2, 2=DNF]: ", 0, 2, &v) == 0) { s->penalty = v; if (save_all() != 0) perror("Could not save edited solve"); }
        } else if (choice == 3) {
            int max_session = current_session_id;
            for (int i = 0; i < solve_count; i++) if (solves[i].session_id > max_session) max_session = solves[i].session_id;
            if (max_session < MAX_SOLVES && prompt_int("Destination session ID: ", 1, max_session + 1, &v) == 0) {
                s->session_id = v;
                if (v > current_session_id) current_session_id = v;
                save_config();
                if (save_all() != 0) perror("Could not save session change");
            }
        } else if (choice == 4) {
            s->bookmarked = !s->bookmarked;
            if (save_all() != 0) perror("Could not save bookmark change");
        } else if (choice == 5) {
            char value[SCRAMBLE_LEN];
            if (read_line_prompt("New scramble (no tabs; blank cancels): ", value, sizeof(value)) == 0 && !strchr(value, '\t')) {
                snprintf(s->scramble, sizeof(s->scramble), "%s", value);
                if (save_all() != 0) perror("Could not save scramble change");
            }
        } else if (choice == 6) {
            char answer[32];
            if (read_line_prompt("Delete this solve? Type yes to confirm: ", answer, sizeof(answer)) == 0 && strcasecmp(answer, "yes") == 0) {
                memmove(&solves[idx], &solves[idx + 1], (size_t)(solve_count - idx - 1) * sizeof(solves[0]));
                solve_count--;
                if (save_all() != 0) perror("Could not save deletion");
                done = 1;
            }
        }
    }
}

static void edit_current_scramble(void) {
    if (running || inspection_active || space_pending) return;
    char value[SCRAMBLE_LEN];
    if (read_line_prompt("Paste/customize current scramble (blank cancels): ", value, sizeof(value)) != 0) return;
    if (strchr(value, '\t')) {
        printf("\nTabs are not allowed in a scramble. Press any key..."); fflush(stdout); wait_for_key();
        return;
    }
    snprintf(current_scramble, sizeof(current_scramble), "%s", value);
}

static void delete_latest_solve(void) {
    if (solve_count == 0 || running) return;
    char answer[32];
    if (read_line_prompt("Delete the latest solve permanently? Type yes to confirm: ", answer, sizeof(answer)) != 0) return;
    if (strcasecmp(answer, "yes") != 0) return;
    solve_count--;
    if (save_all() != 0) perror("Could not save deleted solve");
}

static void switch_session(void) {
    int max_id = current_session_id;
    for (int i = 0; i < solve_count; i++) if (solves[i].session_id > max_id) max_id = solves[i].session_id;
    printf("\033[H\033[JAvailable session IDs: ");
    for (int id = 1; id <= max_id; id++) if (session_solve_count(id) > 0 || id == current_session_id) printf("%d%s ", id, id == current_session_id ? "*" : "");
    printf("\n");
    int chosen = 0;
    if (prompt_int("Switch to session ID: ", 1, max_id, &chosen) == 0 && (session_solve_count(chosen) > 0 || chosen == current_session_id)) {
        current_session_id = chosen;
        save_config();
    } else if (chosen > 0) {
        printf("\nThat session has no solves and is not current. Press any key..."); fflush(stdout); wait_for_key();
    }
}

static void show_bookmarks(void) {
    printf("\033[H\033[J");
    print_rule(); print_panel_title("BOOKMARKED SOLVES"); print_rule();
    int shown = 0;
    for (int i = solve_count - 1; i >= 0; i--) if (solves[i].bookmarked) {
        char t[32]; format_time(effective_ms(&solves[i]), t, sizeof(t));
        printf("#%-5d %-9s %-3s S%-3d %s\n", i + 1, t, penalty_name(solves[i].penalty), solves[i].session_id, solves[i].scramble);
        shown++;
    }
    if (!shown) printf("\nNo bookmarked solves yet. Press B after a solve to bookmark it.\n");
    printf("\nPress any key to return..."); fflush(stdout); wait_for_key();
}

static void show_progress_chart(void) {
    printf("\033[H\033[J");
    print_rule(); print_panel_title("RECENT TIME TREND / LAST 20 SOLVES"); print_rule();
    int indices[20], count = 0;
    for (int i = solve_count - 1; i >= 0 && count < 20; i--) indices[count++] = i;
    if (!count) printf("\nNo solves available yet.\n");
    long long max_ms = 1;
    for (int j = 0; j < count; j++) { long long v = effective_ms(&solves[indices[j]]); if (v > max_ms) max_ms = v; }
    printf("\n  Newest first; bar length is relative to the slowest valid solve in this view.\n\n");
    for (int j = 0; j < count; j++) {
        int idx = indices[j]; long long v = effective_ms(&solves[idx]); char t[32]; format_time(v, t, sizeof(t));
        printf("  #%02d %-9s ", idx + 1, t);
        if (v < 0) printf("%sDNF%s", color(6), color(0));
        else {
            int bars = (int)((double)v / (double)max_ms * 36.0); if (bars < 1) bars = 1;
            printf("%s", v == best_single() ? color(4) : color(2));
            for (int b = 0; b < bars; b++) putchar('#');
            printf("%s", color(0));
        }
        putchar('\n');
    }
    printf("\nPress any key to return..."); fflush(stdout); wait_for_key();
}

static void render(void) {
    char timer[32], best[32], ao5[32], ao12[32], mean[32], sbest[32], sao3[32], sao5[32], sao12[32];
    char bestao5[32], bestao12[32], allbestao5[32], allbestao12[32];
    format_time(elapsed_ms(), timer, sizeof(timer));
    print_formatted_or_na(best_single(), best, sizeof(best));
    print_formatted_or_na(average_last(5), ao5, sizeof(ao5));
    print_formatted_or_na(average_last(12), ao12, sizeof(ao12));
    print_formatted_or_na(session_mean(), mean, sizeof(mean));
    print_formatted_or_na(session_best_single(), sbest, sizeof(sbest));
    int session_count = session_solve_count(current_session_id);
    long long cur3 = current_session_average(3), cur5 = current_session_average(5), cur12 = current_session_average(12);
    print_formatted_or_na(cur3, sao3, sizeof(sao3));
    print_formatted_or_na(cur5, sao5, sizeof(sao5));
    print_formatted_or_na(cur12, sao12, sizeof(sao12));
    print_formatted_or_na(best_average(5, current_session_id), bestao5, sizeof(bestao5));
    print_formatted_or_na(best_average(12, current_session_id), bestao12, sizeof(bestao12));
    print_formatted_or_na(best_average(5, 0), allbestao5, sizeof(allbestao5));
    print_formatted_or_na(best_average(12, 0), allbestao12, sizeof(allbestao12));
    printf("\033[H\033[J");
    print_rule();
    const char *state = running ? "RUNNING" : (inspection_active ? "INSPECTION" : (space_pending ? "HOLD SPACE" : "READY"));
    printf("%s| NEXUSCLI C v%s  //  SPEEDCUBING LAB                 SESSION #%d  [%s]%s\n", color(1), APP_VERSION, current_session_id, state, color(0));
    print_rule();
    printf("\n  %sSCRAMBLE / 3x3%s\n  %s\n", color(2), color(0), current_scramble);
    printf("  HOLD SPACE >= %d ms, then release to start; SPACE stops while running\n\n", config.hold_ms);
    if (inspection_active && !running) {
        long long inspect_ms = (monotonic_ns() - inspection_start_ns) / 1000000LL;
        if (inspect_ms <= config.inspection_seconds * 1000LL)
            snprintf(timer, sizeof(timer), "%lld.%02lld", (config.inspection_seconds * 1000LL - inspect_ms) / 1000LL, ((config.inspection_seconds * 1000LL - inspect_ms) % 1000LL) / 10LL);
        else if (inspect_ms <= config.inspection_dnf_seconds * 1000LL)
            snprintf(timer, sizeof(timer), "+2 %lld.%02lld", (inspect_ms - config.inspection_seconds * 1000LL) / 1000LL, ((inspect_ms - config.inspection_seconds * 1000LL) % 1000LL) / 10LL);
        else snprintf(timer, sizeof(timer), "DNF");
    }
    print_big_timer(timer);
    printf("\n");
    print_rule();
    printf("%sSESSION #%d%s  solves %-5d mean %-8s best %-8s SD %.2fs\n", color(1), current_session_id, color(0), session_count, mean, sbest, session_deviation()/1000.0);
    printf("%sSESSION AVG%s   Ao3 %-8s Ao5 %-8s Ao12 %-8s\n", color(2), color(0), sao3, sao5, sao12);
    printf("%sSESSION BEST%s  Ao5 %-8s Ao12 %-8s\n", color(2), color(0), bestao5, bestao12);
    printf("%sALL-TIME%s      solves %-5d PB %-8s current Ao5 %-8s Ao12 %-8s\n", color(2), color(0), solve_count, best, ao5, ao12);
    printf("%sALL-TIME BEST%s Ao5 %-8s Ao12 %-8s\n", color(2), color(0), allbestao5, allbestao12);
    print_rule();
    printf("%sRECENT SOLVES%s\n", color(1), color(0));
    int shown = solve_count < config.recent_solves ? solve_count : config.recent_solves;
    if (shown) {
        for (int i = solve_count - 1, count = 0; i >= 0 && count < shown; i--, count++) {
            char t[32]; format_time(effective_ms(&solves[i]), t, sizeof(t));
            printf("  %s%2d.%s %-9s %-3s %s%s\n", color(3), solve_count - i, color(0), t, penalty_name(solves[i].penalty), solves[i].bookmarked ? "* " : "  ", solves[i].scramble);
        }
    } else if (solve_count == 0) printf("  No solves yet. Press I to inspect, or hold Space to start.\n");
    else printf("  Recent solve display is disabled in config.conf.\n");
    print_rule();
    printf(" %sSPACE%s timer  %sI%s inspect  %sN%s new scramble  %sR%s custom scramble  %sS%s session  %sG%s switch  %s?%s help\n", color(2), color(0), color(2), color(0), color(2), color(0), color(2), color(0), color(2), color(0), color(2), color(0), color(2), color(0));
    printf(" %s+%s +2  %sD%s DNF  %sB%s bookmark  %sM%s edit time  %sX%s delete  %sU%s undo  %sC%s settings\n", color(2), color(0), color(2), color(0), color(2), color(0), color(2), color(0), color(2), color(0), color(2), color(0), color(2), color(0));
    printf(" %sH%s history  %sK%s bookmarks  %sV%s sessions  %sA%s daily  %sP%s trend  %sE%s export  %sQ%s quit\n", color(2), color(0), color(2), color(0), color(2), color(0), color(2), color(0), color(2), color(0), color(2), color(0), color(2), color(0));
    fflush(stdout);
}
static void add_solve(long long ms) {
    if (solve_count >= MAX_SOLVES) {
        printf("Solve limit reached (%d). Export and back up your data.\n", MAX_SOLVES);
        return;
    }
    Solve *s = &solves[solve_count++];
    memset(s, 0, sizeof(*s));
    s->ms = ms;
    s->penalty = next_penalty;
    next_penalty = PENALTY_NONE;
    s->epoch = (long long)time(NULL);
    s->session_id = current_session_id;
    s->bookmarked = 0;
    snprintf(s->scramble, sizeof(s->scramble), "%s", current_scramble);
    if (save_all() != 0) perror("Could not save solves");
    make_scramble();
}

static int csv_write_field(FILE *f, const char *value) {
    if (fputc('"', f) == EOF) return -1;
    for (const char *p = value; *p; p++) {
        if (*p == '"' && fputc('"', f) == EOF) return -1;
        if (fputc((unsigned char)*p, f) == EOF) return -1;
    }
    return fputc('"', f) == EOF ? -1 : 0;
}

static int write_csv_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int failed = 0;
    if (fprintf(f, "timestamp,raw_ms,penalty,display_time,session,bookmarked,scramble\n") < 0) failed = 1;
    for (int i = 0; i < solve_count && !failed; i++) {
        char t[32];
        format_time(effective_ms(&solves[i]), t, sizeof(t));
        if (fprintf(f, "%lld,%lld,%s,", solves[i].epoch, solves[i].ms,
                    penalty_name(solves[i].penalty)) < 0 ||
            csv_write_field(f, t) != 0 ||
            fprintf(f, ",%d,%d,", solves[i].session_id, solves[i].bookmarked) < 0 ||
            csv_write_field(f, solves[i].scramble) != 0 || fputc('\n', f) == EOF) failed = 1;
    }
    if (fflush(f) != 0) failed = 1;
    if (ferror(f)) failed = 1;
    if (fclose(f) != 0) failed = 1;
    if (failed) { errno = EIO; return -1; }
    return 0;
}

static int export_csv(char *out_path, size_t out_size) {
    /* Prefer Android shared Downloads so the user can actually find the file. */
    const char *home = getenv("HOME");
    char shared_dir[1200];
    if (home && snprintf(shared_dir, sizeof(shared_dir), "%s/storage/shared/Download", home) < (int)sizeof(shared_dir) &&
        access(shared_dir, W_OK) == 0) {
        if (snprintf(out_path, out_size, "%s/nexuscli-solves.csv", shared_dir) < (int)out_size) {
            if (write_csv_file(out_path) == 0) return 0;
        }
    }
    if (snprintf(out_path, out_size, "%s/solves.csv", data_dir_path) >= (int)out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return write_csv_file(out_path);
}

static void wait_for_key(void) {
    fd_set set;
    for (;;) {
        FD_ZERO(&set); FD_SET(STDIN_FILENO, &set);
        int r = select(STDIN_FILENO + 1, &set, NULL, NULL, NULL);
        if (r > 0) { char c; if (read(STDIN_FILENO, &c, 1) == 1) return; }
        else if (r < 0 && errno != EINTR) return;
    }
}

static void show_sessions(void) {
    printf("\033[H\033[JSession overview (newest first)\n\n");
    int max_id = current_session_id;
    for (int i = 0; i < solve_count; i++) if (solves[i].session_id > max_id) max_id = solves[i].session_id;
    for (int sid = max_id; sid >= 1; sid--) {
        int count = 0, valid_count = 0; long long sum = 0, best = LLONG_MAX; long long latest = 0;
        for (int i = 0; i < solve_count; i++) if (solves[i].session_id == sid) {
            count++; latest = solves[i].epoch;
            long long v = effective_ms(&solves[i]);
            if (v >= 0) { sum += v; valid_count++; if (v < best) best = v; }
        }
        if (!count && sid != current_session_id) continue;
        char meanbuf[32], bestbuf[32], datebuf[32] = "empty";
        print_formatted_or_na(valid_count ? sum / valid_count : -2, meanbuf, sizeof(meanbuf));
        print_formatted_or_na(best == LLONG_MAX ? -2 : best, bestbuf, sizeof(bestbuf));
        if (latest) {
            time_t tt = (time_t)latest; struct tm tmv;
            if (localtime_r(&tt, &tmv)) strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", &tmv);
        }
        printf("Session #%-4d %5d solves | mean %9s | best %9s | %s%s\n",
               sid, count, meanbuf, bestbuf, datebuf, sid == current_session_id ? "  <current>" : "");
    }
    printf("\nPress any key to return..."); fflush(stdout); wait_for_key();
}

static void show_daily_activity(void) {
    time_t now = time(NULL); struct tm today;
    if (!localtime_r(&now, &today)) return;
    today.tm_hour = 12; today.tm_min = 0; today.tm_sec = 0;
    printf("\033[H\033[JDaily activity (last 7 days)\n\n");
    for (int back = 6; back >= 0; back--) {
        struct tm d = today;
        d.tm_mday -= back;
        d.tm_hour = 12; d.tm_min = 0; d.tm_sec = 0;
        time_t day = mktime(&d);
        if (day == (time_t)-1 || !localtime_r(&day, &d)) continue;
        int count = 0;
        for (int i = 0; i < solve_count; i++) {
            time_t tt = (time_t)solves[i].epoch; struct tm sd;
            if (localtime_r(&tt, &sd) && sd.tm_year == d.tm_year && sd.tm_yday == d.tm_yday) count++;
        }
        char datebuf[32]; strftime(datebuf, sizeof(datebuf), "%a %Y-%m-%d", &d);
        printf("%-16s %4d  ", datebuf, count);
        int bars = count > 40 ? 40 : count;
        for (int j = 0; j < bars; j++) putchar('#');
        putchar('\n');
    }
    printf("\nPress any key to return..."); fflush(stdout); wait_for_key();
}

static int setup_terminal(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "NexusCLI needs an interactive terminal.\n");
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &original_termios) != 0) { perror("tcgetattr"); return -1; }
    struct termios raw = original_termios;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON);
    raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) { perror("tcsetattr"); return -1; }
    terminal_raw = 1;
    printf("\033[?25l");
    return 0;
}

int main(int argc, char **argv) {
    int cli_export = argc > 1 && strcmp(argv[1], "--export-csv") == 0;
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        printf("NexusCLI C v%s\nUsage: %s [--help|--version|--export-csv|--config]\n", APP_VERSION, argv[0]);
        printf("Interactive keys: ? help, C settings, Space timer, I inspection, H history, E export.\n");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--version") == 0) { printf("NexusCLI C v%s\n", APP_VERSION); return 0; }
    const char *home = getenv("HOME");
    if (!home || snprintf(data_dir_path, sizeof(data_dir_path), "%s/%s", home, DATA_DIR) >= (int)sizeof(data_dir_path) ||
        snprintf(data_path, sizeof(data_path), "%s/%s/%s", home, DATA_DIR, DATA_FILE) >= (int)sizeof(data_path)) {
        fprintf(stderr, "Could not determine home directory.\n"); return 1;
    }
    if (mkdir(data_dir_path, 0700) != 0 && errno != EEXIST) { perror("mkdir ~/.nexuscli"); return 1; }
    if (snprintf(config_path, sizeof(config_path), "%s/%s", data_dir_path, CONFIG_FILE) >= (int)sizeof(config_path)) {
        fprintf(stderr, "Config path is too long.\n"); return 1;
    }
    load_config();
    srand((unsigned int)(time(NULL) ^ getpid()));
    load_all();
    config_has_session = 1;
    (void)save_config();
    if (argc > 1 && strcmp(argv[1], "--config") == 0) { puts(config_path); return 0; }
    if (cli_export) {
        char exported[1400];
        if (export_csv(exported, sizeof(exported)) != 0) { perror("CSV export failed"); return 1; }
        printf("Exported %d solves to: %s\n", solve_count, exported);
        return 0;
    }
    make_scramble();
    if (setup_terminal() != 0) return 1;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    int quit = 0;
    int redraw = 1;
    while (!quit) {
        if (redraw || running || inspection_active || space_pending) {
            render();
            redraw = 0;
        }
        fd_set set;
        FD_ZERO(&set); FD_SET(STDIN_FILENO, &set);
       struct timeval tv;

if (running || inspection_active || space_pending) {
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
} else {
    tv.tv_sec = 1;
    tv.tv_usec = 0;
}
        int ready = select(STDIN_FILENO + 1, &set, NULL, NULL, &tv);
        if (ready < 0) { if (errno == EINTR) continue; break; }
        if (ready == 0) {
            long long now = monotonic_ns();
            if (space_pending) {
                if (space_repeat_seen && now - space_last_ns >= 180000000LL &&
                    now - space_first_ns >= (long long)config.hold_ms * 1000000LL) {
                    long long inspect_ms = inspection_active ? (now - inspection_start_ns) / 1000000LL : 0;
                    next_penalty = PENALTY_NONE;
                    if (inspection_active) {
                        if (inspect_ms > config.inspection_dnf_seconds * 1000LL) next_penalty = PENALTY_DNF;
                        else if (inspect_ms > config.inspection_seconds * 1000LL) next_penalty = PENALTY_PLUS2;
                        inspection_active = 0;
                    }
                    running = 1;
                    start_ns = now;
                    stopped_ms = 0;
                    space_pending = 0;
                    space_repeat_seen = 0;
                    redraw = 1;
                } else if (!space_repeat_seen && now - space_first_ns >= 800000000LL) {
                    /* No repeated key event: likely a tap, not a hold. */
                    space_pending = 0;
                    redraw = 1;
                }
            }
            continue;
        }
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n != 1) continue;
        redraw = 1;
        long long now = monotonic_ns();

        if (c == 'q' || c == 'Q') { quit = 1; }
        else if (c == ' ' || c == '\r' || c == '\n') {
            if (now < ignore_space_until_ns) continue;
            if (running) {
                stopped_ms = elapsed_ms();
                running = 0;
                add_solve(stopped_ms);
                stopped_ms = 0;
                inspection_active = 0;
                next_penalty = PENALTY_NONE;
                space_pending = 0;
                space_repeat_seen = 0;
                ignore_space_until_ns = now + 800000000LL;
            } else {
                /* Terminals don't report key-up. Key repeat is used to infer a held key. */
                if (!space_pending || now - space_last_ns > 250000000LL) {
                    space_pending = 1;
                    space_repeat_seen = 0;
                    space_first_ns = now;
                    space_last_ns = now;
                } else {
                    space_repeat_seen = 1;
                    space_last_ns = now;
                }
            }
        } else if ((c == 'i' || c == 'I') && !running && !space_pending) {
            inspection_active = 1;
            inspection_start_ns = now;
            next_penalty = PENALTY_NONE;
        } else if ((c == 'n' || c == 'N') && !running && !inspection_active && !space_pending) make_scramble();
        else if ((c == 'r' || c == 'R') && !running && !inspection_active && !space_pending) edit_current_scramble();
        else if ((c == '+' || c == '=') && solve_count > 0 && !running) {
            Solve *s = &solves[solve_count - 1];
            if (s->penalty != PENALTY_DNF) s->penalty = (s->penalty == PENALTY_PLUS2) ? PENALTY_NONE : PENALTY_PLUS2;
            if (save_all() != 0) perror("Could not save solves");
        } else if ((c == 'd' || c == 'D') && solve_count > 0 && !running) {
            Solve *s = &solves[solve_count - 1];
            s->penalty = (s->penalty == PENALTY_DNF) ? PENALTY_NONE : PENALTY_DNF;
            if (save_all() != 0) perror("Could not save solves");
        } else if ((c == 's' || c == 'S') && !running && !inspection_active && !space_pending) {
            int max_session_id = current_session_id;
            for (int i = 0; i < solve_count; i++) if (solves[i].session_id > max_session_id) max_session_id = solves[i].session_id;
            if (max_session_id < MAX_SOLVES) current_session_id = max_session_id + 1;
            save_config();
            printf("Current session is #%d. Press any key to continue...", current_session_id); fflush(stdout);
            wait_for_key();
        } else if ((c == 'b' || c == 'B') && solve_count > 0 && !running) {
            solves[solve_count - 1].bookmarked = !solves[solve_count - 1].bookmarked;
            if (save_all() != 0) perror("Could not save solves");
        } else if ((c == 'u' || c == 'U') && solve_count > 0 && !running) {
            solve_count--;
            if (save_all() != 0) perror("Could not save solves");
        } else if ((c == 'e' || c == 'E') && !running) {
            char exported[1400];
            if (export_csv(exported, sizeof(exported)) == 0)
                printf("\033[H\033[JExported %d solves to:\n%s\n\nPress any key to return...", solve_count, exported);
            else printf("\033[H\033[JCSV export failed: %s\n\nPress any key to return...", strerror(errno));
            fflush(stdout); wait_for_key();
        } else if ((c == 'v' || c == 'V') && !running) {
            show_sessions();
        } else if ((c == 'a' || c == 'A') && !running) {
            show_daily_activity();
        } else if ((c == 'c' || c == 'C') && !running && !inspection_active && !space_pending) {
            show_config_menu();
        } else if (c == '?' && !running) {
            show_help();
        } else if ((c == 'm' || c == 'M') && !running) {
            edit_latest_time();
        } else if ((c == 'x' || c == 'X') && !running) {
            delete_latest_solve();
        } else if ((c == 'g' || c == 'G') && !running && !inspection_active && !space_pending) {
            switch_session();
        } else if ((c == 'k' || c == 'K') && !running) {
            show_bookmarks();
        } else if ((c == 'p' || c == 'P') && !running) {
            show_progress_chart();
        } else if ((c == 'h' || c == 'H') && !running) {
            printf("\033[H\033[JRecent solve history (newest first):\n");
            int shown = solve_count < 20 ? solve_count : 20;
            for (int i = solve_count - 1, count = 0; i >= 0 && count < shown; i--, count++) {
                char t[32]; format_time(effective_ms(&solves[i]), t, sizeof(t));
                printf("%3d. %-9s %-3s S%-3d %s%s\n", solve_count - i, t, penalty_name(solves[i].penalty), solves[i].session_id, solves[i].bookmarked ? "* " : "  ", solves[i].scramble);
            }
            printf("\nPress any key to return..."); fflush(stdout);
            wait_for_key();
        }
    }
    (void)save_all();
    save_config();
    restore_terminal();
    printf("Saved %d solves to %s\n", solve_count, data_path);
    return 0;
}

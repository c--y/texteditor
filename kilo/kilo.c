#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <termios.h>

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#define VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)
#define AppendBuffer_INIT {NULL, 0}

enum EditorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    PAGE_UP,
    PAGE_DOWN,
    HOME_KEY,
    END_KEY,
    DEL_KEY
};

typedef struct AppendBuffer {
    char *b;
    int len;
} AppendBuffer;

void AppendBuffer_append(AppendBuffer *ab, const char *s, int len) {
    char *mem = realloc(ab->b, ab->len + len);
    if (mem == NULL) return;
    memcpy(&mem[ab->len], s, len);
    ab->b = mem;
    ab->len += len;
}

void AppendBuffer_free(AppendBuffer *ab) {
    free(ab->b);
}

typedef struct Row {
    int size;
    char *buf;
} Row;

typedef struct EditorConfig {
    struct termios orig_termios;
    int screen_rows;
    int screen_cols;
    int cx;
    int cy;
    int row_off;
    int num_rows;
    Row *row;
} EditorConfig;

EditorConfig E;

void die(const char *s) {
    // clear screen
    write(STDOUT_FILENO, "\x1b[2J", 4);
    // reposition the cursor
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disable_raw_mode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr"); 
    }
}

void enable_raw_mode() {
    if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        die("tcgetattr");
    }
    atexit(disable_raw_mode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(ICRNL | IXON | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }
}

int editor_read_key() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno == EAGAIN) {
            die("read");
        }
    }

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            // Page up && down
            if (seq[1] > '0' && seq[1] < '9') {
                if (read(STDOUT_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            }

            // Arrow keys
            switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }    
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return '\x1b';
    } else {
        return c;
    }
}

int get_cursor_pos(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;
    // Device status report
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }

    buf[i] = '\0';
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols)) return -1;

    editor_read_key();
    return 0;
}

int get_window_size(int *rows, int *cols) {
    // TODO if ioctl failed, get the window size in another way
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }
        return get_cursor_pos(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

void init_editor() {
    E.cx = 0;
    E.cy = 0;
    E.row_off = 0;
    E.num_rows = 0;
    E.row = NULL;

    if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) {
        die("get_window_size");
    }
}

void editor_append_row(char *s, size_t n) {
    E.row = realloc(E.row, sizeof(Row) * (E.num_rows + 1));
    int at = E.num_rows;
    E.row[at].size = n;
    E.row[at].buf = malloc(n + 1);
    memcpy(E.row[at].buf, s, n);
    E.row[at].buf[n] = '\0';
    E.num_rows++;
}

void editor_draw_rows(AppendBuffer *ab) {
    int y;
    for (y = 0; y < E.screen_rows; y++) {
        int file_row = y + E.row_off;
        if (file_row >= E.num_rows) {
            if (E.num_rows == 0 && y == E.screen_rows / 3) {
                char welcome[80];
                int n = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", VERSION);
                if (n > E.screen_cols) {
                    n = E.screen_cols;
                }
                // Add paddings
                int padding = (E.screen_cols - n) / 2;
                if (padding) {
                    AppendBuffer_append(ab, "~", 1);
                    padding--;
                }
                while (padding--) {
                    AppendBuffer_append(ab, " ", 1);
                }
                AppendBuffer_append(ab, welcome, n);
            } else {
                AppendBuffer_append(ab, "~", 1);
            }
        } else {
            int n = E.row[file_row].size;
            if (n > E.screen_cols) n = E.screen_cols;
            AppendBuffer_append(ab, E.row[file_row].buf, n);
        }
        AppendBuffer_append(ab, "\x1b[K", 3);

        if (y < E.screen_rows - 1) {
            // write(STDOUT_FILENO, "\r\n", 2);
            AppendBuffer_append(ab, "\r\n", 2);
        }
    }
}

void editor_scroll() {
    if (E.cy < E.row_off) {
        E.row_off = E.cy;
    }
    if (E.cy >= E.row_off + E.screen_rows) {
        E.row_off = E.cy - E.screen_rows + 1;
    }
}

void editor_move_cursor(int key) {
    switch (key) {
        case ARROW_LEFT:
            if (E.cx > 0) E.cx--;
            break;
        case ARROW_RIGHT:
            if (E.cx < E.screen_cols) E.cx++;
            break;
        case ARROW_UP:
            if (E.cy > 0) E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy < E.num_rows) E.cy++;
            break;
    }
}

void editor_refresh_screen() {
    editor_scroll();

    AppendBuffer ab = AppendBuffer_INIT;
    // Hide cursor
    AppendBuffer_append(&ab, "\x1b[?25l", 6);
    // clear screen
    // AppendBuffer_append(&ab, "\x1b[2J", 4);
    // reposition the cursor
    AppendBuffer_append(&ab, "\x1b[H", 3);

    editor_draw_rows(&ab);
    // Move cursor to cx,cy
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.row_off + 1, E.cx + 1);
    AppendBuffer_append(&ab, buf, strlen(buf));
    // AppendBuffer_append(&ab, "\x1b[H", 3);
    // Show cursor
    AppendBuffer_append(&ab, "\x1b[?25h", 6);
    write(STDOUT_FILENO, ab.b, ab.len);
    AppendBuffer_free(&ab);
}

void editor_proc_keypress() {
    int c = editor_read_key();
    switch (c) {
        case CTRL_KEY('q'):
            // clear screen
            write(STDOUT_FILENO, "\x1b[2J", 4);
            // reposition the cursor
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            E.cx = E.screen_cols - 1;
            break;
        case PAGE_UP:
        case PAGE_DOWN:
            {
                int times = E.screen_rows;
                while (times--) {
                    editor_move_cursor(c == PAGE_UP? ARROW_UP: ARROW_DOWN);
                }
            }
            break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editor_move_cursor(c);
            break;
    }
}

void editor_open(char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        die("fopen");
    }
    
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    linelen = getline(&line, &linecap, fp);
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        if (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
            linelen--;
        }
        editor_append_row(line, linelen);
    }
    free(line);
    fclose(fp);
}

int main(int argc, char *argv[]) {
    enable_raw_mode();
    init_editor();
    if (argc >= 2) {
        editor_open(argv[1]);
    }

    while (1) {
        editor_refresh_screen();
        editor_proc_keypress();
    }
    return 0;
}

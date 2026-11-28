/*
 * ui.c - 화면 출력 모듈 (색상, 막대 그래프, 추이 그래프, 로그 꼬리 읽기)
 *
 * ctl 의 status / watch / log 화면을 만드는 도우미 함수 모음.
 * 터미널 제어는 ANSI escape 문자열(ESC [ ...)로 한다.
 *
 * 사용한 수업 내용
 *   ch4  fopen / fseek / ftell / fread (로그 파일 끝에서 거꾸로 읽기)
 *   기타 ioctl(TIOCGWINSZ) 터미널 크기, termios 비정규 입력 모드 (htop 처럼 키 하나로 조작)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

#include "common.h"
#include "ui.h"

/* ---------- 모듈 내부 상태 ---------- */
static int            g_color = 1;     /* 1 이면 escape 문자열 출력 */
static int            g_raw   = 0;     /* 1 이면 raw 모드 적용 중 */
static struct termios g_saved;         /* raw 모드 전 터미널 설정 (복구용) */

void ui_color_init(int on)
{
    g_color = on;
}

const char *ui_c(const char *code)
{
    return g_color ? code : "";
}

/* buf 뒤에 printf 형식으로 덧붙인다 (버퍼가 차면 조용히 잘라 낸다) */
static void sappend(char *buf, size_t n, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void sappend(char *buf, size_t n, const char *fmt, ...)
{
    size_t len = strlen(buf);
    va_list ap;

    if (len >= n - 1)
        return;
    va_start(ap, fmt);
    vsnprintf(buf + len, n - len, fmt, ap);
    va_end(ap);
}

int ui_term_size(int *rows, int *cols)
{
    struct winsize ws;

    /* 터미널 드라이버에 창 크기를 묻는다. 파이프로 출력 중이면 실패한다 */
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0 || ws.ws_col == 0) {
        *rows = 24;
        *cols = 80;
        return -1;
    }
    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return 0;
}

/* UTF-8 첫 바이트 c 로 글자의 바이트 수를 구하고, 화면 폭을 *w 에 넣는다 (한글 등 3바이트 이상은 2칸) */
static int u8_len(unsigned char c, int *w)
{
    if (c < 0x80)           { *w = 1; return 1; }
    if ((c & 0xE0) == 0xC0) { *w = 1; return 2; }
    if ((c & 0xF0) == 0xE0) { *w = 2; return 3; }
    if ((c & 0xF8) == 0xF0) { *w = 2; return 4; }
    *w = 1;                                 /* 잘못된 바이트는 1칸으로 취급 */
    return 1;
}

size_t ui_fit(const char *s, int cols)
{
    size_t i = 0;
    int used = 0, w, l;

    while (s[i] != '\0') {
        l = u8_len((unsigned char)s[i], &w);
        if (used + w > cols)
            break;
        if (memchr(s + i, '\0', (size_t)l) != NULL)   /* 글자 중간에서 문자열이 끝남 */
            break;
        used += w;
        i += (size_t)l;
    }
    return i;
}

void ui_bar(char *buf, size_t n, double pct, double warn_pct, const char *text, int width)
{
    int tlen = (int)strlen(text);
    int barw = width - tlen - 1;            /* 막대 칸 = 전체 - 글자 - 공백 1칸 */
    int fill, i;
    const char *col;

    if (barw < 1)
        barw = 1;
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    fill = (int)(pct * barw / 100.0 + 0.5);
    /* 임계값 이상 빨강, 임계값의 75% 이상 노랑, 그 아래 초록 */
    col = pct >= warn_pct ? UI_RED : (pct >= warn_pct * 0.75 ? UI_YELLOW : UI_GREEN);

    buf[0] = '\0';
    sappend(buf, n, "%s[%s%s", ui_c(UI_BOLD), ui_c(UI_RESET), ui_c(col));
    for (i = 0; i < barw; i++)
        sappend(buf, n, "%c", i < fill ? '|' : ' ');
    sappend(buf, n, "%s %s%s%s]%s", ui_c(UI_RESET), ui_c(UI_DIM), text,
            ui_c(UI_RESET UI_BOLD), ui_c(UI_RESET));
}

void ui_spark(char *buf, size_t n, const float *hist, int pos, int count, int width,
              double maxv, double warn)
{
    static const char *lv[8] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
    int shown = count < width ? count : width;
    int start = pos - shown;                /* 가장 오래된 표시 칸 (음수면 뒤에서부터) */
    int i, k, cur = 0, c;                   /* cur: 현재 색 (1 보통, 2 경고) */

    buf[0] = '\0';
    if (maxv <= 0)
        maxv = 1;
    for (i = 0; i < shown; i++) {
        double v = hist[((start + i) % HIST_LEN + HIST_LEN) % HIST_LEN];
        k = (int)(v / maxv * 7.0 + 0.5);
        if (k < 0)
            k = 0;
        if (k > 7)
            k = 7;
        c = v >= warn ? 2 : 1;
        if (c != cur) {                     /* 색이 바뀔 때만 escape 문자열 출력 */
            sappend(buf, n, "%s", ui_c(c == 2 ? UI_RED : UI_CYAN));
            cur = c;
        }
        sappend(buf, n, "%s", lv[k]);
    }
    sappend(buf, n, "%s", ui_c(UI_RESET));
    for (; i < width; i++)                  /* 아직 덜 찬 부분은 공백 */
        sappend(buf, n, " ");
}

void ui_log_line(FILE *out, const char *line, int short_time, int cols)
{
    char buf[LOG_LINE_MAX * 2];
    size_t len;
    const char *col, *rest;
    int hot = 0, used;

    snprintf(buf, sizeof(buf), "%s", line);
    len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    /* 형식: "[YYYY-MM-DD HH:MM:SS] [LEVEL] [pid] 메시지" (logger.c 의 log_write) */
    if (len < 30 || buf[0] != '[' || buf[20] != ']' || buf[22] != '[' || buf[28] != ']') {
        fprintf(out, "%.*s", (int)ui_fit(buf, cols), buf);
        return;
    }
    if (strncmp(buf + 23, "WARN", 4) == 0) {
        col = UI_YELLOW; hot = 1;
    } else if (strncmp(buf + 23, "ERROR", 5) == 0) {
        col = UI_RED; hot = 1;
    } else if (strncmp(buf + 23, "DEBUG", 5) == 0) {
        col = UI_DIM;
    } else {
        col = UI_GREEN;
    }
    rest = buf + 30;

    if (short_time) {                       /* watch: 날짜 없이 시각만 */
        fprintf(out, "%s%.8s%s ", ui_c(UI_DIM), buf + 12, ui_c(UI_RESET));
        used = 9;
    } else {
        fprintf(out, "%s%.21s%s ", ui_c(UI_DIM), buf, ui_c(UI_RESET));
        used = 22;
    }
    fprintf(out, "%s%.7s%s ", ui_c(col), buf + 22, ui_c(UI_RESET));
    used += 8;
    cols -= used;
    if (cols < 0)
        cols = 0;
    fprintf(out, "%s%.*s%s", hot ? ui_c(col) : "", (int)ui_fit(rest, cols), rest,
            hot ? ui_c(UI_RESET) : "");
}

long ui_tail_offset(FILE *fp, int n)
{
    char buf[4096];
    long end, pos;
    size_t chunk, i;
    int cnt = 0;

    if (n <= 0 || fseek(fp, 0, SEEK_END) < 0)  /* ch4: 파일 끝으로 이동 */
        return 0;
    end = ftell(fp);
    pos = end;

    /* 끝에서부터 4KB 씩 거꾸로 읽으며 개행 문자를 센다.
     * 파일 맨 끝의 개행은 마지막 줄의 끝이므로 세지 않는다. */
    while (pos > 0) {
        chunk = pos > (long)sizeof(buf) ? sizeof(buf) : (size_t)pos;
        pos -= (long)chunk;
        if (fseek(fp, pos, SEEK_SET) < 0 || fread(buf, 1, chunk, fp) != chunk)
            return 0;
        for (i = chunk; i > 0; i--) {
            if (buf[i - 1] == '\n' && pos + (long)i != end && ++cnt == n)
                return pos + (long)i;       /* 이 개행 바로 다음이 n 번째 줄의 시작 */
        }
    }
    return 0;                               /* 전체가 n 줄 이하 -> 처음부터 */
}

int ui_raw_mode(int on)
{
    struct termios t;

    if (on) {
        if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &g_saved) < 0)
            return -1;
        t = g_saved;
        t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);   /* 줄 단위 입력·에코 끄기 */
        t.c_cc[VMIN]  = 0;                         /* read 가 키가 없으면 바로 돌아오게 */
        t.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &t) < 0)
            return -1;
        g_raw = 1;
    } else if (g_raw) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
        g_raw = 0;
    }
    return 0;
}

void ui_screen_enter(void)
{
    /* 대체 화면 버퍼로 전환 + 커서 숨김 + 화면 지우기 (종료하면 원래 화면이 그대로 돌아온다) */
    fputs(ui_c("\033[?1049h\033[?25l\033[H\033[2J"), stdout);
    fflush(stdout);
}

void ui_screen_leave(void)
{
    fputs(ui_c("\033[?25h\033[?1049l"), stdout);
    fflush(stdout);
}

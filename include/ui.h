/*
 * ui.h - 화면 출력 모듈 인터페이스 (ctl 의 status / watch / log 화면)
 *
 * 색상과 커서 이동은 ANSI escape 문자열로 처리한다.
 * 출력이 터미널이 아니면(파이프, 파일) ui_color_init(0) 으로 escape 문자열을 끈다.
 */
#ifndef UI_H
#define UI_H

#include <stdio.h>
#include <stddef.h>

/* ---------- ANSI escape 문자열 (반드시 ui_c() 로 감싸서 사용) ---------- */
#define UI_RESET  "\033[0m"
#define UI_BOLD   "\033[1m"
#define UI_DIM    "\033[2m"
#define UI_RED    "\033[31m"
#define UI_GREEN  "\033[32m"
#define UI_YELLOW "\033[33m"
#define UI_CYAN   "\033[36m"
#define UI_HEAD   "\033[30;42m"   /* htop 표 머리줄: 검은 글자 + 초록 배경 */
#define UI_KEY    "\033[30;46m"   /* 아래쪽 키 안내: 검은 글자 + 청록 배경 */
#define UI_HOME   "\033[H"        /* 커서를 왼쪽 위로 */
#define UI_EOL    "\033[K"        /* 커서 뒤 줄 끝까지 지우기 */
#define UI_EOS    "\033[J"        /* 커서 뒤 화면 끝까지 지우기 */

/* 1 이면 색상/커서 제어 문자열 사용, 0 이면 사용 안 함 */
void ui_color_init(int on);

/* 색상이 켜져 있으면 code, 꺼져 있으면 "" 를 돌려준다 */
const char *ui_c(const char *code);

/* 터미널 크기를 rows, cols 에 넣는다. 성공 0, 터미널이 아니면 -1 (24x80 으로 채움) */
int  ui_term_size(int *rows, int *cols);

/* UTF-8 문자열 s 를 화면 폭 cols 칸 안에 넣을 때 쓸 수 있는 바이트 수 (한글은 2칸) */
size_t ui_fit(const char *s, int cols);

/* htop 스타일 막대 "[||||||     text]" 를 buf 에 만든다.
 * @pct: 0~100   @warn_pct: 이 값 이상이면 빨간색   @text: 막대 안 오른쪽 글자(ASCII)   @width: 막대 안쪽 폭 */
void ui_bar(char *buf, size_t n, double pct, double warn_pct, const char *text, int width);

/* 원형 버퍼 hist 의 최근 값을 "▁▂▃▅▇" 추이 그래프로 buf 에 만든다.
 * @pos: 다음에 쓸 칸  @count: 채워진 칸 수  @width: 최대 글자 수  @maxv: 그래프 최대값  @warn: 이 값 이상은 빨간색 */
void ui_spark(char *buf, size_t n, const float *hist, int pos, int count, int width,
              double maxv, double warn);

/* 로그 한 줄을 레벨별 색상으로 out 에 출력한다 (줄바꿈은 출력하지 않음).
 * @short_time: 1 이면 시각을 HH:MM:SS 만 표시   @cols: 최대 화면 폭 */
void ui_log_line(FILE *out, const char *line, int short_time, int cols);

/* 파일 끝에서 거꾸로 읽어 마지막 n 줄이 시작하는 위치(바이트)를 돌려준다. 실패 시 0 */
long ui_tail_offset(FILE *fp, int n);

/* on=1: 키를 Enter 없이 바로 읽고 화면에 찍지 않는 모드, on=0: 원래대로. 성공 0, 실패 -1 */
int  ui_raw_mode(int on);

/* 전체 화면(대체 화면 버퍼) 진입 / 복귀. 커서를 숨겼다가 되돌린다. */
void ui_screen_enter(void);
void ui_screen_leave(void);

#endif /* UI_H */

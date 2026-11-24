/*
 * logger.c - 로그 모듈
 *
 * 사용한 수업 내용
 *   ch4  저수준 파일 입출력 : open / write / read / close, O_APPEND
 *   ch5  시간 함수          : time / localtime_r / strftime
 *   ch6  프로세스 정보      : getpid
 *   ch8  시그널             : SIGINT 무시(logger 프로세스)
 *   ch10 파이프             : logger_run 이 pipe 에서 로그를 수신
 *   Pthread                 : mutex 로 로그 카운터/출력 보호
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>

#include "logger.h"

/* ---------- 모듈 내부 상태 ---------- */
static int             g_log_fd = -1;    /* 로그를 쓰는 fd (파일 또는 pipe) */
static int             g_echo   = 0;     /* 1 이면 stderr 에도 출력 */
static unsigned long   g_count  = 0;     /* 기록한 줄 수 */
static pthread_mutex_t g_lock   = PTHREAD_MUTEX_INITIALIZER; /* 카운터/출력 보호 */

/* 레벨 -> 문자열 */
static const char *level_str(log_level_t lv)
{
    switch (lv) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO ";
    case LOG_WARN:  return "WARN ";
    case LOG_ERROR: return "ERROR";
    default:        return "?????";
    }
}

/* fd 에 len 바이트를 모두 쓴다. EINTR 이면 재시도. 성공 0, 실패 -1 */
static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);   /* ch4: 저수준 write */
        if (n < 0) {
            if (errno == EINTR)            /* 시그널에 의해 중단 -> 다시 시도 */
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

int log_init(const char *path)
{
    /* O_APPEND: 여러 프로세스가 같은 파일에 써도 각 write 가 파일 끝에 붙는다 */
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        perror("log_init: open");
        return -1;
    }
    g_log_fd = fd;
    return 0;
}

void log_init_fd(int fd)
{
    g_log_fd = fd;
}

void log_close(void)
{
    if (g_log_fd >= 0) {
        close(g_log_fd);
        g_log_fd = -1;
    }
}

void log_set_echo(int on)
{
    g_echo = on;
}

unsigned long log_count(void)
{
    unsigned long c;
    pthread_mutex_lock(&g_lock);
    c = g_count;
    pthread_mutex_unlock(&g_lock);
    return c;
}

void log_write(log_level_t level, const char *fmt, ...)
{
    char    ts[32];                 /* "2026-11-23 12:00:00" */
    char    msg[LOG_LINE_MAX / 2];  /* 사용자 메시지 */
    char    line[LOG_LINE_MAX];     /* 완성된 한 줄 */
    time_t  now;
    struct tm tmv;
    va_list ap;
    int     len;

    /* 1) 시각 문자열 만들기 (ch5: time / localtime_r / strftime) */
    now = time(NULL);
    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    /* 2) printf 형식 메시지 만들기 */
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* 3) "[시각] [레벨] [pid] 메시지\n" 한 줄로 합치기 (ch6: getpid) */
    len = snprintf(line, sizeof(line), "[%s] [%s] [%d] %s\n",
                   ts, level_str(level), (int)getpid(), msg);
    if (len < 0)
        return;
    if ((size_t)len >= sizeof(line))      /* 잘렸다면 끝을 개행으로 맞춘다 */
        len = (int)sizeof(line) - 1, line[len - 1] = '\n';

    /* 4) 한 번의 write 로 기록: 줄 크기가 PIPE_BUF(4096) 이하이므로 pipe 에서도
     *    여러 프로세스의 로그가 서로 섞이지 않는다 */
    pthread_mutex_lock(&g_lock);
    g_count++;
    if (g_log_fd < 0 || write_all(g_log_fd, line, (size_t)len) < 0) {
        /* 로그 fd 가 없거나 쓰기 실패(EPIPE 등) -> stderr 로 대체 */
        write_all(STDERR_FILENO, line, (size_t)len);
    } else if (g_echo) {
        write_all(STDERR_FILENO, line, (size_t)len);
    }
    pthread_mutex_unlock(&g_lock);
}

int logger_run(int read_fd, const char *path)
{
    char buf[BUF_SIZE];
    ssize_t n;
    int out;

    /* logger 는 Ctrl-C 가 눌려도 죽지 않고 남은 로그를 끝까지 기록해야 한다.
     * (터미널의 Ctrl-C 는 같은 프로세스 그룹 전체에 SIGINT 를 보낸다) */
    signal(SIGINT, SIG_IGN);

    out = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (out < 0) {
        perror("logger_run: open");
        return -1;
    }

    /* pipe 에서 읽어서 그대로 파일에 append. 쓰기 끝이 모두 닫히면 read 가 0 을 반환 */
    while ((n = read(read_fd, buf, sizeof(buf))) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("logger_run: read");
            break;
        }
        if (write_all(out, buf, (size_t)n) < 0) {
            perror("logger_run: write");
            break;
        }
    }

    close(out);
    close(read_fd);
    return 0;
}


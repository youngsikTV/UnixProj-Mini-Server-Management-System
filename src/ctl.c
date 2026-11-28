/*
 * ctl.c - 제어 클라이언트 (CLI)
 *
 * 사용법
 *   ./build/ctl status                      supervisor/worker 상태(FIFO) + 시스템 상태(공유 메모리)
 *   ./build/ctl watch [-d 초] [-n 횟수]     htop 스타일 실시간 화면 (q: 종료, b: 즉시 백업)
 *   ./build/ctl log [-n 줄수] [-f] [-w] [-p 파일]  로그 보기 (-f 계속 따라가기, -w WARN 이상만)
 *   ./build/ctl backup                      backup worker 에게 즉시 백업 요청
 *   ./build/ctl stop                        서버(supervisor)와 모든 worker 안전 종료
 *
 * 통신 방식
 *   명령/응답 : FIFO (ipc.c 의 ctl_request)  -> supervisor 와 대화
 *   시스템 상태: mmap 공유 메모리 직접 읽기   -> monitor 가 기록한 값
 *   로그       : supervisor 가 알려 준 로그 파일을 직접 읽기 (fopen/fread)
 *
 * 사용한 수업 내용
 *   ch1  getopt (하위 명령별 옵션)
 *   ch4  fopen / fseek / fread / fgets (로그 파일, /proc/<pid>/stat 읽기)
 *   ch5  time / localtime_r / strftime, sysconf(_SC_CLK_TCK, _SC_PAGESIZE)
 *   ch8  sigaction + alarm (서버 무응답 시 시간 초과), SIGINT/SIGTERM 으로 watch·log -f 종료
 *   ch9  mmap 공유 메모리 읽기 (ipc.c)
 *   ch10 FIFO 로 명령 전송 / 응답 수신 (ipc.c)
 *   기타 select (키 입력과 화면 갱신 주기를 함께 기다림), termios (ui.c)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>

#include "common.h"
#include "ipc.h"
#include "ui.h"

#define REQUEST_TIMEOUT_SEC 3   /* 서버가 이 시간 안에 응답하지 않으면 포기 */
#define MAX_ROWS            12  /* watch 프로세스 표 최대 줄 수 */
#define MAX_PREV            64  /* CPU% 계산용 이전 값 기억 칸 수 */

static const char *g_prog = "ctl";            /* usage 에 표시할 프로그램 이름 */
static volatile sig_atomic_t g_quit = 0;      /* SIGINT/SIGTERM: watch, log -f 종료 */

/* SIGALRM 핸들러: 아무것도 하지 않는다. 블록된 open/read 를 EINTR 로 깨우는 것이 목적 */
static void on_alarm(int sig)
{
    (void)sig;
}

/* SIGINT/SIGTERM 핸들러: 종료 플래그만 세운다 (화면 복구는 main 흐름에서) */
static void on_quit(int sig)
{
    (void)sig;
    g_quit = 1;
}

/* 초 단위 시간을 "N일 N시간 N분" 문자열로 바꾼다 */
static void format_duration(unsigned long sec, char *buf, size_t n)
{
    unsigned long d = sec / 86400, h = (sec % 86400) / 3600, m = (sec % 3600) / 60;
    if (d > 0)
        snprintf(buf, n, "%lu일 %lu시간 %lu분", d, h, m);
    else
        snprintf(buf, n, "%lu시간 %lu분", h, m);
}

/* 초 단위 시간을 htop 처럼 "H:MM:SS" (하루 이상이면 "Nd HH:MM") 로 바꾼다 */
static void format_hms(long sec, char *buf, size_t n)
{
    if (sec >= 86400)
        snprintf(buf, n, "%ldd %02ld:%02ld", sec / 86400, (sec % 86400) / 3600, (sec % 3600) / 60);
    else
        snprintf(buf, n, "%ld:%02ld:%02ld", sec / 3600, (sec % 3600) / 60, sec % 60);
}

static void usage(void)
{
    fprintf(stderr,
            "사용법: %s <명령> [옵션]\n"
            "  status                     서버/worker 상태와 시스템 상태 출력\n"
            "  watch [-d 초] [-n 횟수]    실시간 상태 화면 (q: 종료, b: 즉시 백업)\n"
            "  log [-n 줄수] [-f] [-w] [-p 파일]\n"
            "                             로그 보기 (-f: 새 줄 계속 출력, -w: WARN/ERROR 만)\n"
            "  backup                     즉시 백업 요청\n"
            "  stop                       서버 종료\n", g_prog);
}

/*
 * send_command - 서버에 명령을 보내고 응답 문자열을 out 에 받는다.
 * @quiet: 1 이면 실패 원인을 출력하지 않는다 (watch 처럼 반복 호출할 때)
 * 반환: 성공 0, 실패 -1
 */
static int send_command(cmd_type_t type, char *out, size_t outsz, int quiet)
{
    cmd_msg_t m;
    struct sigaction sa;
    int r, e;

    memset(&m, 0, sizeof(m));
    m.type = type;

    /* SA_RESTART 를 쓰지 않아야 alarm 이 울렸을 때 open/read 가 EINTR 로 풀려난다 */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    alarm(REQUEST_TIMEOUT_SEC);
    r = ctl_request(&m, out, outsz);
    e = errno;
    alarm(0);

    if (r < 0) {
        if (quiet)
            return -1;
        if (e == ENXIO || e == ENOENT)
            fprintf(stderr, "서버가 실행 중이지 않습니다. 먼저 ./build/server 를 실행하세요.\n");
        else if (e == EINTR)
            fprintf(stderr, "서버가 %d초 안에 응답하지 않았습니다.\n", REQUEST_TIMEOUT_SEC);
        else
            fprintf(stderr, "서버와 통신 실패: %s\n", strerror(e));
        return -1;
    }
    return 0;
}

/*
 * reply_field - 서버 응답에서 "key : 값" 형식 줄을 찾아 값을 out 에 복사한다.
 * 반환: 찾음 0, 없음 -1
 */
static int reply_field(const char *reply, const char *key, char *out, size_t n)
{
    const char *p = reply, *v, *e;
    size_t klen = strlen(key);

    while (p != NULL && *p != '\0') {
        if (strncmp(p, key, klen) == 0 && (p[klen] == ' ' || p[klen] == ':')) {
            v = strstr(p, ": ");
            if (v == NULL)
                return -1;
            v += 2;
            e = strchr(v, '\n');
            if (e == NULL)
                e = v + strlen(v);
            snprintf(out, n, "%.*s", (int)(e - v), v);
            return 0;
        }
        p = strchr(p, '\n');
        if (p != NULL)
            p++;
    }
    return -1;
}

/*
 * get_log_path - 로그 파일 경로를 out 에 넣는다.
 * 서버가 실행 중이면 status 응답의 logfile(절대 경로)을, 아니면 기본 경로를 쓴다.
 */
static void get_log_path(char *out, size_t n)
{
    char reply[4096];

    if (send_command(CMD_STATUS, reply, sizeof(reply), 1) == 0 &&
        reply_field(reply, "logfile", out, n) == 0)
        return;
    snprintf(out, n, "%s", LOG_PATH_DEFAULT);
}

/* hist 의 최근 count 개 값 중 최대값 (추이 그래프의 세로 범위를 정할 때 사용) */
static double hist_max(const float *hist, int count)
{
    double m = 0;
    int i;
    for (i = 0; i < count && i < HIST_LEN; i++)
        if (hist[i] > m)
            m = hist[i];
    return m;
}

/* 공유 메모리에서 일관된 복사본을 읽는다. 성공(값 있음) 0, 실패 -1 */
static int read_monitor(monitor_data_t *s)
{
    const monitor_data_t *shm = shm_map_readonly();
    int r;

    if (shm == NULL)
        return -1;
    r = shm_read_snapshot(shm, s);
    shm_unmap(shm);
    return (r == 0 && s->updated != 0) ? 0 : -1;
}

/* monitor 가 공유 메모리에 기록한 시스템 상태를 막대·추이 그래프와 함께 출력한다 */
static void print_monitor(void)
{
    monitor_data_t s;
    char ts[32], up[64], bar[512], sp[1024], txt[64];
    struct tm tmv;
    time_t now = time(NULL);
    double used_mb, total_mb, mem_pct, lw;
    int ncpu;

    printf("%s===== 시스템 상태 (monitor) =====%s\n", ui_c(UI_BOLD), ui_c(UI_RESET));
    if (read_monitor(&s) < 0) {
        printf("monitor 데이터 없음 (monitor 가 실행 중이 아니거나 아직 수집 전)\n");
        return;
    }

    localtime_r(&s.updated, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    format_duration(s.uptime, up, sizeof(up));
    total_mb = s.totalram / 1048576.0;
    used_mb  = (s.totalram - s.freeram) / 1048576.0;
    mem_pct  = total_mb > 0 ? used_mb * 100.0 / total_mb : 0.0;
    ncpu     = s.ncpu > 0 ? s.ncpu : 1;
    lw       = s.load_warn > 0 ? s.load_warn : (double)ncpu;

    printf("호스트 / 사용자 : %s / %s\n", s.host, s.user);
    printf("마지막 갱신     : %s (%ld초 전, 총 %lu회 갱신)\n",
           ts, (long)(now - s.updated), s.samples);
    printf("시스템 가동     : %s\n", up);
    snprintf(txt, sizeof(txt), "%.1f%%", mem_pct);
    ui_bar(bar, sizeof(bar), mem_pct, s.mem_warn, txt, 30);
    printf("메모리 사용     : %s %.0f / %.0f MB\n", bar, used_mb, total_mb);
    snprintf(txt, sizeof(txt), "%.2f", s.load1);
    ui_bar(bar, sizeof(bar), s.load1 * 100.0 / ncpu, lw * 100.0 / ncpu, txt, 30);
    printf("Load average(1m): %s CPU %d개\n", bar, ncpu);
    printf("프로세스 수     : %u\n", (unsigned)s.procs);
    printf("monitor CPU     : user %.2fs / sys %.2fs\n", s.cpu_user, s.cpu_sys);
    ui_spark(sp, sizeof(sp), s.mem_hist, s.hist_pos, s.hist_len, 40, 100.0, s.mem_warn);
    printf("메모리 추이     : %s\n", sp);
    ui_spark(sp, sizeof(sp), s.load_hist, s.hist_pos, s.hist_len, 40,
             hist_max(s.load_hist, s.hist_len) > lw ? hist_max(s.load_hist, s.hist_len) : lw, lw);
    printf("Load 추이       : %s\n", sp);
    printf("임계값          : 메모리 %.0f%%, load %.2f -> %s%s%s\n", s.mem_warn, lw,
           ui_c(s.warn_flags ? UI_RED : UI_GREEN), s.warn_flags ? "초과" : "정상", ui_c(UI_RESET));

    if (now - s.updated > 10)          /* monitor 가 멈춘 경우 경고 */
        printf("%s[경고] 10초 넘게 갱신되지 않았습니다. monitor 상태를 확인하세요.%s\n",
               ui_c(UI_YELLOW), ui_c(UI_RESET));
}

/* ====================================================================
 *  watch: htop 스타일 실시간 화면
 * ==================================================================== */

typedef struct {
    char name[32];      /* supervisor, logger, monitor, backup ... */
    int  pid;           /* 실행 중이 아니면 0 이하 */
    int  alive;         /* 1 이면 실행 중 */
    int  restarts;      /* 재시작 횟수 */
    long uptime;        /* 가동 시간(초), 모르면 0 */
} row_t;

typedef struct {
    int pid;
    unsigned long long ticks;   /* 직전 화면에서 읽은 utime + stime */
} prev_t;

/*
 * parse_status - supervisor 의 status 응답을 프로세스 표 rows 로 바꾼다.
 * 예) "monitor    : pid=123 (실행 중, 재시작 0회, 가동 42초)"
 * 반환: 채운 줄 수
 */
static int parse_status(const char *reply, row_t *rows, int max)
{
    const char *p = reply, *e, *q;
    char line[256], name[32];
    int n = 0, pid, r;

    while (*p != '\0' && n < max) {
        e = strchr(p, '\n');
        snprintf(line, sizeof(line), "%.*s", e ? (int)(e - p) : (int)strlen(p), p);
        p = e ? e + 1 : p + strlen(p);

        r = sscanf(line, "%31s : pid=%d", name, &pid);
        if (r < 1 || strcmp(name, "logfile") == 0 || name[0] == '=')
            continue;
        if (r == 1 && strstr(line, "종료됨") == NULL)
            continue;
        memset(&rows[n], 0, sizeof(rows[n]));
        snprintf(rows[n].name, sizeof(rows[n].name), "%s", name);
        rows[n].pid   = r == 2 ? pid : 0;
        rows[n].alive = rows[n].pid > 0 && strstr(line, "종료됨") == NULL;
        if ((q = strstr(line, "재시작 ")) != NULL)
            rows[n].restarts = atoi(q + strlen("재시작 "));
        if ((q = strstr(line, "가동 ")) != NULL)
            rows[n].uptime = atol(q + strlen("가동 "));
        n++;
    }
    return n;
}

/*
 * proc_usage - /proc/<pid>/stat 에서 CPU 사용 시간(tick)과 메모리(RSS)를 읽는다.
 * 반환: 성공 0, 실패(프로세스 없음 등) -1
 */
static int proc_usage(int pid, unsigned long long *ticks, double *rss_mb)
{
    char path[64], buf[1024], *p;
    unsigned long ut, st;
    long rss;
    FILE *fp;

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    fp = fopen(path, "r");                           /* ch4: 커널이 보여 주는 프로세스 정보 파일 */
    if (fp == NULL)
        return -1;
    p = fgets(buf, sizeof(buf), fp);
    fclose(fp);
    /* 2번째 필드(명령 이름)에 공백이 있을 수 있으므로 마지막 ')' 뒤부터 센다.
     * ')' 뒤: 3 state ... 14 utime, 15 stime ... 24 rss(페이지 수) */
    if (p == NULL || (p = strrchr(buf, ')')) == NULL)
        return -1;
    if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu "
                      "%*d %*d %*d %*d %*d %*d %*u %*u %ld", &ut, &st, &rss) != 3)
        return -1;
    *ticks  = (unsigned long long)ut + st;
    *rss_mb = (double)rss * (double)sysconf(_SC_PAGESIZE) / 1048576.0;
    return 0;
}

/* 직전 화면과의 CPU tick 차이로 CPU 사용률(%)을 구한다. dt: 화면 간격(초) */
static double cpu_percent(prev_t *prev, int *nprev, int pid, unsigned long long ticks, double dt)
{
    long hz = sysconf(_SC_CLK_TCK);                  /* ch5: 초당 clock tick 수 */
    double pct = 0.0;
    int i;

    for (i = 0; i < *nprev && i < MAX_PREV; i++) {
        if (prev[i].pid == pid) {
            if (dt > 0 && ticks >= prev[i].ticks)
                pct = (double)(ticks - prev[i].ticks) / (double)hz / dt * 100.0;
            prev[i].ticks = ticks;
            return pct;
        }
    }
    i = (*nprev)++ % MAX_PREV;                       /* 처음 보는 pid: 기억만 하고 0% */
    prev[i].pid   = pid;
    prev[i].ticks = ticks;
    return 0.0;
}

/* 화면 한 줄을 끝낸다: 이전 화면에서 남은 글자를 지우고 줄바꿈 */
static void eol(void)
{
    fputs(ui_c(UI_EOL), stdout);
    fputc('\n', stdout);
}

/*
 * draw_frame - watch 화면 한 장을 그린다.
 * @logpath: 최근 로그를 읽을 파일   @msg: 아래 줄에 잠깐 보여 줄 안내 (없으면 "")
 * @delay: 갱신 주기(초)   @prev/@nprev: CPU% 계산용 이전 값   @dt: 직전 화면과의 간격(초)
 */
static void draw_frame(const char *logpath, const char *msg, double delay,
                       prev_t *prev, int *nprev, double dt)
{
    char reply[4096], bar[512], sp[1024], txt[64], tbuf[32], up[64], line[512];
    char l[LOG_LINE_MAX * 2];
    monitor_data_t s;
    row_t rows[MAX_ROWS];
    int nrows = 0, have_shm, have_srv, trows, cols, i, k;
    time_t now = time(NULL);
    struct tm tmv;
    FILE *fp;

    ui_term_size(&trows, &cols);
    have_srv = send_command(CMD_STATUS, reply, sizeof(reply), 1) == 0;   /* FIFO */
    if (have_srv)
        nrows = parse_status(reply, rows, MAX_ROWS);
    have_shm = read_monitor(&s) == 0;                                     /* mmap */

    fputs(ui_c(UI_HOME), stdout);
    localtime_r(&now, &tmv);
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);

    /* 1) 머리줄 */
    printf("%s Mini Server Manager %s  %s%s@%s%s   %s", ui_c(UI_KEY), ui_c(UI_RESET),
           ui_c(UI_BOLD), have_shm ? s.user : "-", have_shm ? s.host : "-", ui_c(UI_RESET), tbuf);
    if (have_shm) {
        format_duration(s.uptime, up, sizeof(up));
        printf("   시스템 가동 %s", up);
    }
    eol();
    eol();

    /* 2) 시스템 상태: 막대 그래프 + 추이 그래프 */
    if (have_shm) {
        double total = s.totalram / 1048576.0, used = (s.totalram - s.freeram) / 1048576.0;
        double mem_pct = total > 0 ? used * 100.0 / total : 0.0;
        int ncpu = s.ncpu > 0 ? s.ncpu : 1;
        double lw = s.load_warn > 0 ? s.load_warn : (double)ncpu;
        double lmax = hist_max(s.load_hist, s.hist_len);

        snprintf(txt, sizeof(txt), "%.0f/%.0fMB", used, total);
        ui_bar(bar, sizeof(bar), mem_pct, s.mem_warn, txt, 44);
        printf("  %sMem %s%s %5.1f%%", ui_c(UI_CYAN), ui_c(UI_RESET), bar, mem_pct);
        eol();
        snprintf(txt, sizeof(txt), "%.2f", s.load1);
        ui_bar(bar, sizeof(bar), s.load1 * 100.0 / ncpu, lw * 100.0 / ncpu, txt, 44);
        printf("  %sLoad%s%s CPU %d", ui_c(UI_CYAN), ui_c(UI_RESET), bar, ncpu);
        eol();
        printf("  %sTasks%s %u   %s수집%s %lu회 (%d초 간격)", ui_c(UI_CYAN), ui_c(UI_RESET),
               (unsigned)s.procs, ui_c(UI_CYAN), ui_c(UI_RESET), s.samples,
               s.interval > 0 ? s.interval : MONITOR_INTERVAL_SEC);
        eol();
        ui_spark(sp, sizeof(sp), s.mem_hist, s.hist_pos, s.hist_len, 50, 100.0, s.mem_warn);
        printf("  메모리 추이 %s", sp);
        eol();
        ui_spark(sp, sizeof(sp), s.load_hist, s.hist_pos, s.hist_len, 50,
                 lmax > lw ? lmax : lw, lw);
        printf("  Load   추이 %s", sp);
        eol();
        if (s.warn_flags)
            printf("  %s%s[경고] %s%s 임계값 초과 (메모리 %.0f%%, load %.2f)%s", ui_c(UI_RED),
                   ui_c(UI_BOLD), (s.warn_flags & WARN_MEM) ? "메모리" : "",
                   (s.warn_flags & WARN_MEM) && (s.warn_flags & WARN_LOAD) ? "·load"
                   : ((s.warn_flags & WARN_LOAD) ? "load" : ""), s.mem_warn, lw, ui_c(UI_RESET));
        else
            printf("  %s임계값 정상%s (메모리 %.0f%%, load %.2f)", ui_c(UI_GREEN), ui_c(UI_RESET),
                   s.mem_warn, lw);
        if (now - s.updated > 10)
            printf("  %s[갱신 멈춤 %ld초]%s", ui_c(UI_YELLOW), (long)(now - s.updated),
                   ui_c(UI_RESET));
        eol();
    } else {
        printf("  %smonitor 데이터 없음 (monitor 가 실행 중이 아님)%s", ui_c(UI_YELLOW),
               ui_c(UI_RESET));
        eol();
        for (k = 0; k < 5; k++)        /* 줄 수를 맞춰 아래 내용이 위아래로 흔들리지 않게 */
            eol();
    }
    eol();

    /* 3) 프로세스 표: htop 처럼 색 반전된 머리줄 */
    snprintf(line, sizeof(line), "  %-7s %-11s %-8s %7s %11s %6s %8s",
             "PID", "NAME", "STATE", "RESTART", "UPTIME", "CPU%", "RSS(MB)");
    printf("%s%-*.*s%s", ui_c(UI_HEAD), cols - 1, cols - 1, line, ui_c(UI_RESET));
    eol();
    if (!have_srv) {
        printf("  %s서버에 연결할 수 없습니다 (./build/server 실행 필요)%s", ui_c(UI_RED),
               ui_c(UI_RESET));
        eol();
    }
    for (i = 0; i < nrows; i++) {
        row_t *r = &rows[i];
        unsigned long long ticks;
        double rss = 0.0, cpu = 0.0;
        char pidbuf[16] = "-", upbuf[32] = "-";

        if (r->alive && proc_usage(r->pid, &ticks, &rss) == 0)
            cpu = cpu_percent(prev, nprev, r->pid, ticks, dt);
        if (r->pid > 0)
            snprintf(pidbuf, sizeof(pidbuf), "%d", r->pid);
        if (r->uptime > 0)
            format_hms(r->uptime, upbuf, sizeof(upbuf));
        printf("  %-7s %s%-11s%s %s%-8s%s %7d %11s %6.1f %8.1f", pidbuf,
               ui_c(UI_BOLD), r->name, ui_c(UI_RESET),
               ui_c(r->alive ? UI_GREEN : UI_RED), r->alive ? "running" : "stopped",
               ui_c(UI_RESET), r->restarts, upbuf, cpu, rss);
        eol();
    }
    eol();

    /* 4) 최근 로그 LOG_TAIL_LINES 줄: 화면마다 파일 끝을 다시 읽어 새 줄이 계속 올라온다 */
    printf("%s 최근 로그 %d줄 %s %s%s", ui_c(UI_KEY), LOG_TAIL_LINES, ui_c(UI_RESET),
           ui_c(UI_DIM), logpath);
    printf("%s", ui_c(UI_RESET));
    eol();
    fp = fopen(logpath, "r");                        /* ch4 */
    k = 0;
    if (fp == NULL) {
        printf("  (로그 파일을 열 수 없음: %s)", strerror(errno));
        eol();
        k = 1;
    } else {
        fseek(fp, ui_tail_offset(fp, LOG_TAIL_LINES), SEEK_SET);
        while (k < LOG_TAIL_LINES && fgets(l, sizeof(l), fp) != NULL) {
            fputs("  ", stdout);
            ui_log_line(stdout, l, 1, cols - 3);
            eol();
            k++;
        }
        fclose(fp);
    }
    for (; k < LOG_TAIL_LINES; k++)                  /* 로그가 적어도 줄 수를 고정 */
        eol();
    eol();

    /* 5) 아래쪽 키 안내 (htop 의 F1~F10 줄 흉내) */
    printf("%sq%s%s 종료 %s %sb%s%s 즉시백업 %s  갱신 %.1f초", ui_c(UI_BOLD), ui_c(UI_RESET),
           ui_c(UI_KEY), ui_c(UI_RESET), ui_c(UI_BOLD), ui_c(UI_RESET), ui_c(UI_KEY),
           ui_c(UI_RESET), delay);
    if (msg[0] != '\0')
        printf("   %s%s%s", ui_c(UI_YELLOW), msg, ui_c(UI_RESET));
    eol();
    fputs(ui_c(UI_EOS), stdout);                     /* 화면 아래 남은 이전 내용 지우기 */
    fflush(stdout);
}

/*
 * cmd_watch - htop 스타일 실시간 화면. delay 초마다 다시 그리고 키 입력을 처리한다.
 * 반환: 종료 코드
 */
static int cmd_watch(int argc, char *argv[])
{
    double delay = 1.0, dt;
    int count = 0, frames = 0, opt, tty;
    char logpath[PATH_MAX], msg[128] = "";
    time_t msg_until = 0;
    prev_t prev[MAX_PREV];
    int nprev = 0;
    struct timespec t0, t1;
    struct sigaction sa;

    while ((opt = getopt(argc, argv, "d:n:h")) != -1) {
        switch (opt) {
        case 'd': delay = atof(optarg); break;
        case 'n': count = atoi(optarg); break;
        case 'h': usage(); return 0;
        default:  usage(); return 1;
        }
    }
    if (delay < 0.2)
        delay = 0.2;
    get_log_path(logpath, sizeof(logpath));

    /* Ctrl-C 로 끝내도 터미널 설정을 되돌릴 수 있도록 핸들러에서는 플래그만 세운다 */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_quit;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    if (tty)
        ui_raw_mode(1);
    ui_screen_enter();

    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!g_quit) {
        fd_set rfds;
        struct timeval tv;
        int r;

        clock_gettime(CLOCK_MONOTONIC, &t1);
        dt = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
        t0 = t1;
        if (msg_until != 0 && time(NULL) > msg_until) {
            msg[0] = '\0';
            msg_until = 0;
        }
        draw_frame(logpath, msg, delay, prev, &nprev, dt);
        if (count > 0 && ++frames >= count)
            break;

        /* 키 입력 또는 갱신 주기까지 기다린다. 터미널이 아니면 시간만 기다린다 */
        FD_ZERO(&rfds);
        if (tty)
            FD_SET(STDIN_FILENO, &rfds);
        tv.tv_sec  = (long)delay;
        tv.tv_usec = (long)((delay - (double)(long)delay) * 1e6);
        r = select(tty ? STDIN_FILENO + 1 : 0, &rfds, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 'q' || c == 'Q')
                    break;
                if (c == 'b' || c == 'B') {
                    char rep[512];
                    if (send_command(CMD_BACKUP, rep, sizeof(rep), 1) == 0)
                        snprintf(msg, sizeof(msg), "즉시 백업을 요청했습니다");
                    else
                        snprintf(msg, sizeof(msg), "서버에 연결할 수 없습니다");
                    msg_until = time(NULL) + 3;
                }
            }
        }
    }

    ui_screen_leave();
    if (tty)
        ui_raw_mode(0);
    return 0;
}

/* WARN 또는 ERROR 로그 줄이면 1 */
static int is_warn_line(const char *l)
{
    return strstr(l, "[WARN ]") != NULL || strstr(l, "[ERROR]") != NULL;
}

/*
 * cmd_log - 로그 파일의 마지막 n 줄을 레벨별 색상으로 출력한다.
 * -f 이면 tail -f 처럼 새로 기록되는 줄을 계속 출력한다 (Ctrl-C 로 종료).
 * 반환: 종료 코드
 */
static int cmd_log(int argc, char *argv[])
{
    int n = 20, follow = 0, warn_only = 0, opt, i, cnt = 0, rows, cols = 4096;
    const char *path = NULL;
    char pathbuf[PATH_MAX], line[LOG_LINE_MAX * 2];
    char (*ring)[LOG_LINE_MAX * 2];
    FILE *fp;
    struct sigaction sa;

    while ((opt = getopt(argc, argv, "n:fwp:h")) != -1) {
        switch (opt) {
        case 'n': n = atoi(optarg);  break;
        case 'f': follow = 1;        break;
        case 'w': warn_only = 1;     break;
        case 'p': path = optarg;     break;
        case 'h': usage(); return 0;
        default:  usage(); return 1;
        }
    }
    if (n < 1)
        n = 1;
    if (path == NULL) {
        get_log_path(pathbuf, sizeof(pathbuf));
        path = pathbuf;
    }
    if (isatty(STDOUT_FILENO))
        ui_term_size(&rows, &cols);

    fp = fopen(path, "r");                           /* ch4: 표준 입출력 라이브러리로 열기 */
    if (fp == NULL) {
        fprintf(stderr, "로그 파일을 열 수 없습니다: %s (%s)\n", path, strerror(errno));
        return 1;
    }

    if (!warn_only) {
        /* 파일 끝에서 거꾸로 n 줄을 찾아 그 위치부터 출력 (큰 파일도 끝부분만 읽는다) */
        fseek(fp, ui_tail_offset(fp, n), SEEK_SET);
        while (fgets(line, sizeof(line), fp) != NULL) {
            ui_log_line(stdout, line, 0, cols);
            putchar('\n');
        }
    } else {
        /* WARN/ERROR 줄만 원형 버퍼에 모아 마지막 n 줄을 출력 */
        ring = malloc((size_t)n * sizeof(*ring));
        if (ring == NULL) {
            perror("malloc");
            fclose(fp);
            return 1;
        }
        while (fgets(line, sizeof(line), fp) != NULL) {
            if (is_warn_line(line)) {
                memcpy(ring[cnt % n], line, sizeof(line));
                cnt++;
            }
        }
        for (i = cnt > n ? cnt - n : 0; i < cnt; i++) {
            ui_log_line(stdout, ring[i % n], 0, cols);
            putchar('\n');
        }
        free(ring);
    }

    if (follow) {
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_quit;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        fflush(stdout);
        while (!g_quit) {
            if (fgets(line, sizeof(line), fp) != NULL) {
                if (!warn_only || is_warn_line(line)) {
                    ui_log_line(stdout, line, 0, cols);
                    putchar('\n');
                    fflush(stdout);
                }
            } else {
                clearerr(fp);                        /* EOF 표시를 지워야 다음 fgets 가 새 줄을 읽는다 */
                usleep(300000);                      /* 새 줄이 쓰일 때까지 0.3초 대기 */
            }
        }
    }
    fclose(fp);
    return 0;
}

int main(int argc, char *argv[])
{
    char reply[4096];

    g_prog = argv[0];
    ui_color_init(isatty(STDOUT_FILENO));            /* 파이프·파일로 출력하면 색상 끔 */

    if (argc < 2) {
        usage();
        return 1;
    }
    /* 하위 명령의 옵션은 argv+1 부터 getopt 로 처리한다 (argv[0] 자리에 명령 이름) */
    if (strcmp(argv[1], "watch") == 0)
        return cmd_watch(argc - 1, argv + 1);
    if (strcmp(argv[1], "log") == 0)
        return cmd_log(argc - 1, argv + 1);
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0) {
        usage();
        return 0;
    }
    if (argc != 2) {
        usage();
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        if (send_command(CMD_STATUS, reply, sizeof(reply), 0) < 0)
            return 1;
        fputs(reply, stdout);                        /* supervisor 응답은 그대로 (스크립트가 파싱) */
        print_monitor();
        return 0;
    }
    if (strcmp(argv[1], "backup") == 0) {
        if (send_command(CMD_BACKUP, reply, sizeof(reply), 0) < 0)
            return 1;
        fputs(reply, stdout);
        return 0;
    }
    if (strcmp(argv[1], "stop") == 0) {
        if (send_command(CMD_STOP, reply, sizeof(reply), 0) < 0)
            return 1;
        fputs(reply, stdout);
        return 0;
    }

    fprintf(stderr, "알 수 없는 명령: %s\n", argv[1]);
    usage();
    return 1;
}

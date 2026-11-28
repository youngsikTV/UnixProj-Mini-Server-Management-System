/*
 * monitor.c - monitor worker (supervisor 가 exec 로 실행하는 독립 프로그램)
 *
 * 역할
 *   일정 주기(기본 2초)마다 시스템/프로세스 정보를 수집해
 *   mmap 공유 메모리(monitor_data_t)에 기록한다. ctl 이 이 값을 읽어 보여 준다.
 *
 * 사용한 수업 내용
 *   ch1  getopt (-i 주기)
 *   ch5  uname(호스트), getuid/getpwuid(사용자), time
 *   ch6  sysinfo(메모리/load/프로세스 수), times + sysconf(CPU 시간), getenv
 *   ch8  sigaction / setitimer(SIGALRM 주기 실행) / sigsuspend
 *   ch9  mmap 공유 메모리 (ipc.c 의 shm_* 함수)
 *
 * 임계값 경고
 *   메모리 사용률(-m, 기본 90%) 또는 load(-L, 기본 CPU 수)를 넘으면 WARN 로그를 남긴다.
 *   초과가 계속되는 동안 매번 쓰지 않고 "정상 -> 초과", "초과 -> 정상" 으로 바뀔 때만 기록한다.
 * 추이 기록
 *   최근 HIST_LEN 회의 값을 공유 메모리의 원형 버퍼에 남겨 ctl 이 추이 그래프를 그린다.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>

#include "common.h"
#include "logger.h"
#include "ipc.h"

/* 시그널 핸들러가 세우는 플래그 (핸들러에서는 플래그만 변경) */
static volatile sig_atomic_t g_tick = 0;   /* SIGALRM: 수집할 시간 */
static volatile sig_atomic_t g_stop = 0;   /* SIGTERM/SIGINT: 종료 */

static void on_alarm(int sig) { (void)sig; g_tick = 1; }
static void on_stop(int sig)  { (void)sig; g_stop = 1; }

/* 임계값 (getopt 로 설정) */
static double g_mem_warn  = MEM_WARN_DEFAULT;  /* -m: 메모리 사용률 경고 임계값(%) */
static double g_load_warn = 0.0;               /* -L: load 경고 임계값 (0 이면 CPU 수) */

/* sigaction 등록 도우미 */
static int install_handler(int sig, void (*handler)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    return sigaction(sig, &sa, NULL);
}

/*
 * collect - 정보를 한 번 수집해 공유 메모리 d 에 기록한다.
 * seqlock 으로 감싸서 ctl 이 쓰다 만 값을 읽지 않도록 한다.
 */
static void collect(monitor_data_t *d)
{
    struct sysinfo si;
    struct tms tb;
    struct utsname un;
    struct passwd *pw;
    long hz = sysconf(_SC_CLK_TCK);                     /* ch5: 초당 clock tick 수 */
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);           /* ch5: 사용 가능한 CPU 수 */
    double mem_pct, load_warn;
    int flags, old;

    memset(&si, 0, sizeof(si));
    if (sysinfo(&si) < 0)                                /* ch6: 메모리/uptime/load/프로세스 수 */
        log_write(LOG_WARN, "sysinfo 실패: %s", strerror(errno));
    times(&tb);                                          /* ch6: 이 프로세스의 CPU 사용 시간 */
    uname(&un);                                          /* ch5: 호스트 이름 */
    pw = getpwuid(getuid());                             /* ch5: 사용자 이름 (getlogin 은 NULL 일 수 있음) */

    shm_update_begin(d);
    d->samples++;                                        /* 수집 횟수 (ctl 이 표시) */
    d->updated  = time(NULL);
    snprintf(d->user, sizeof(d->user), "%s", pw ? pw->pw_name : "unknown");
    snprintf(d->host, sizeof(d->host), "%s", un.nodename);
    d->cpu_user = (double)tb.tms_utime / (double)hz;
    d->cpu_sys  = (double)tb.tms_stime / (double)hz;
    d->load1    = (double)si.loads[0] / 65536.0;         /* sysinfo 의 load 는 16비트 고정소수점 */
    d->uptime   = (unsigned long)si.uptime;
    d->totalram = (unsigned long)si.totalram * si.mem_unit;
    /* 버퍼로 쓰는 메모리는 바로 돌려받을 수 있으므로 남은 메모리에 포함한다 */
    d->freeram  = (unsigned long)(si.freeram + si.bufferram) * si.mem_unit;
    d->procs    = si.procs;

    if (ncpu < 1)
        ncpu = 1;
    load_warn    = g_load_warn > 0 ? g_load_warn : (double)ncpu;
    d->ncpu      = (int)ncpu;
    d->mem_warn  = g_mem_warn;
    d->load_warn = load_warn;
    mem_pct = d->totalram > 0
              ? (double)(d->totalram - d->freeram) * 100.0 / (double)d->totalram : 0.0;

    /* 추이 기록: 원형 버퍼라서 가득 차면 가장 오래된 칸을 덮어쓴다 */
    d->mem_hist[d->hist_pos]  = (float)mem_pct;
    d->load_hist[d->hist_pos] = (float)d->load1;
    d->hist_pos = (d->hist_pos + 1) % HIST_LEN;
    if (d->hist_len < HIST_LEN)
        d->hist_len++;

    flags = 0;
    if (mem_pct >= g_mem_warn)
        flags |= WARN_MEM;
    if (d->load1 >= load_warn)
        flags |= WARN_LOAD;
    old = d->warn_flags;
    d->warn_flags = flags;
    shm_update_end(d);

    /* 임계값 검사: 상태가 바뀔 때만 로그를 남긴다 (초과가 계속될 때 2초마다 쓰면 로그가 넘친다).
     * log_write 는 pipe write 라서 seqlock 구간 밖에서 호출한다. */
    if ((flags & WARN_MEM) && !(old & WARN_MEM))
        log_write(LOG_WARN, "메모리 사용률 %.1f%% 가 임계값 %.0f%% 를 넘었습니다", mem_pct, g_mem_warn);
    else if (!(flags & WARN_MEM) && (old & WARN_MEM))
        log_write(LOG_INFO, "메모리 사용률 정상 복귀 (%.1f%%)", mem_pct);
    if ((flags & WARN_LOAD) && !(old & WARN_LOAD))
        log_write(LOG_WARN, "load %.2f 가 임계값 %.2f 를 넘었습니다", d->load1, load_warn);
    else if (!(flags & WARN_LOAD) && (old & WARN_LOAD))
        log_write(LOG_INFO, "load 정상 복귀 (%.2f)", d->load1);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "사용법: %s [-i 주기(초)] [-m 메모리%%] [-L load] [-h]\\n"
            "  -i 초      수집 주기 (기본: %d)\\n"
            "  -m 퍼센트  메모리 사용률 경고 임계값 (기본: %.0f)\\n"
            "  -L 값      load 경고 임계값 (기본: CPU 수)\\n",
            prog, MONITOR_INTERVAL_SEC, MEM_WARN_DEFAULT);
}

int main(int argc, char *argv[])
{
    int interval = MONITOR_INTERVAL_SEC;   /* 수집 주기(초), common.h 의 기본값 */
    int opt;
    unsigned long n = 0;               /* 수집 횟수 */
    const char *fdstr;
    monitor_data_t *d;
    struct itimerval it;
    sigset_t block, oldmask;

    while ((opt = getopt(argc, argv, "i:m:L:h")) != -1) {
        switch (opt) {
        case 'i': interval = atoi(optarg); break;
        case 'm': g_mem_warn = atof(optarg); break;
        case 'L': g_load_warn = atof(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }
    if (interval <= 0) {
        fprintf(stderr, "주기는 1 이상의 정수여야 합니다.\n");
        return 1;
    }
    if (g_mem_warn <= 0 || g_mem_warn > 100 || g_load_warn < 0) {
        fprintf(stderr, "임계값이 올바르지 않습니다. (-m 1~100, -L 0 이상)\n");
        return 1;
    }

    /* supervisor 가 환경 변수로 넘겨 준 로그 pipe fd 사용 (ch6: getenv).
     * 없으면(단독 실행) 로그가 stderr 로 출력된다. */
    fdstr = getenv(LOG_FD_ENV);
    if (fdstr != NULL)
        log_init_fd(atoi(fdstr));
    else
        log_set_echo(1);

    d = shm_map_create();
    if (d == NULL) {
        log_write(LOG_ERROR, "공유 메모리 생성 실패");
        return 1;
    }

    if (install_handler(SIGALRM, on_alarm) < 0 ||
        install_handler(SIGTERM, on_stop)  < 0 ||
        install_handler(SIGINT,  on_stop)  < 0) {
        perror("sigaction");
        shm_unmap(d);
        return 1;
    }

    /* 평소에는 시그널을 막고 sigsuspend 안에서만 받는다 (시그널 유실 방지) */
    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGINT);
    sigprocmask(SIG_BLOCK, &block, &oldmask);

    /* ch8: setitimer 로 interval 초마다 SIGALRM 이 오도록 설정 */
    it.it_value.tv_sec     = interval;
    it.it_value.tv_usec    = 0;
    it.it_interval.tv_sec  = interval;
    it.it_interval.tv_usec = 0;
    if (setitimer(ITIMER_REAL, &it, NULL) < 0) {
        perror("setitimer");
        shm_unmap(d);
        return 1;
    }

    shm_update_begin(d);
    d->interval = interval;            /* ctl 이 "N초 간격" 으로 표시 */
    shm_update_end(d);
    log_write(LOG_INFO, "monitor 시작 (주기 %d초, 임계값 메모리 %.0f%% / load %s, shm=%s)",
              interval, g_mem_warn, g_load_warn > 0 ? "지정" : "CPU 수", SHM_FILE_PATH);
    collect(d);                        /* 첫 값은 바로 기록 */
    n++;

    while (!g_stop) {
        if (g_tick) {
            g_tick = 0;
            collect(d);
            n++;
            if (n % 10 == 0)           /* 로그가 너무 많아지지 않도록 10회마다 한 줄 */
                log_write(LOG_INFO, "monitor: %lu회 수집, load=%.2f, 프로세스 %u개",
                          n, d->load1, (unsigned)d->procs);
        }
        sigsuspend(&oldmask);
    }

    /* 정리: 타이머 해제, 매핑 해제, 공유 파일 삭제 */
    it.it_value.tv_sec = it.it_value.tv_usec = 0;
    it.it_interval = it.it_value;
    setitimer(ITIMER_REAL, &it, NULL);
    shm_unmap(d);
    shm_remove();
    log_write(LOG_INFO, "monitor 종료 (총 %lu회 수집)", n);
    return 0;
}

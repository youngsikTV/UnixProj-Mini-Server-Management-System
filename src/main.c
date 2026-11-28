/*
 * main.c - supervisor (메인 프로세스)
 *
 * 역할
 *   1) logger 프로세스를 pipe 와 함께 띄운다.
 *   2) worker(monitor, backup ...)를 fork + exec 로 실행하고 관리한다.
 *   2-1) ctl 이 FIFO 로 보내는 명령(status / backup / stop)을 받아 처리하고 응답한다.
 *   3) SIGCHLD 로 종료된 worker 를 회수(좀비 방지)하고 로그를 남긴다.
 *   3-1) monitor/backup 이 비정상 종료되면 자동으로 다시 실행한다 (최대 MAX_RESTARTS 회).
 *   4) SIGINT/SIGTERM 을 받으면 모든 worker 를 정리하고 안전하게 종료한다.
 *   5) SIGUSR1 을 받으면 backup worker 에 즉시 백업을 전달한다 (kill -USR1 <server pid>).
 *
 * 사용한 수업 내용
 *   ch1  명령행 인자 / getopt
 *   ch3  access (worker 실행 파일 존재 확인), readlink (자기 실행 파일 위치 찾기)
 *   ch6  환경 변수 setenv, getpid
 *   ch7  fork / exec / waitpid / _exit, 좀비 방지
 *   ch8  sigaction / sigprocmask / sigsuspend / kill
 *   ch10 pipe (logger 와 통신), FIFO (ctl 명령 수신/응답)
 *   기타 pselect (시그널과 FIFO 입력을 동시에 기다림)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/wait.h>

#include "common.h"
#include "logger.h"
#include "ipc.h"

/* ---------- worker 정보 ---------- */
#define MAX_RESTARTS       5     /* 한 worker 의 최대 자동 재시작 횟수 */
#define RESTART_RESET_SEC  60    /* 이 시간 이상 안정적으로 실행되면 재시작 횟수를 초기화 */

typedef struct {
    char   name[32];     /* 로그에 표시할 이름 */
    char   path[PATH_MAX]; /* 실행 파일 경로 (재시작에 사용) */
    pid_t  pid;          /* 실행 중이면 pid, 아니면 -1 */
    int    restart;      /* 1 이면 종료 시 자동 재시작 */
    int    restarts;     /* 지금까지 재시작한 횟수 */
    time_t started;      /* 마지막으로 시작한 시각 */
} worker_t;

static worker_t g_workers[MAX_WORKERS];
static int      g_nworkers = 0;
static pid_t    g_logger_pid = -1;
static int      g_log_pipe_wr = -1;          /* logger 로 가는 pipe 쓰기 끝 */
static int      g_cmd_fd = -1;               /* ctl 명령을 받는 FIFO 의 fd */
static time_t   g_start_time;                /* supervisor 시작 시각 (status 표시용) */
static char     g_bindir[PATH_MAX];          /* 이 실행 파일이 있는 디렉터리 (worker 도 여기에 있다) */
static char     g_logpath[PATH_MAX];         /* 로그 파일 절대 경로 (status 응답으로 ctl 에 알려 줌) */

/* ---------- 시그널 핸들러가 바꾸는 플래그 ----------
 * 핸들러 안에서는 플래그만 세우고, 실제 처리는 main 루프에서 한다.
 * (log_write 등은 async-signal-safe 하지 않으므로 핸들러에서 호출하지 않는다) */
static volatile sig_atomic_t g_got_sigchld = 0;
static volatile sig_atomic_t g_stop        = 0;
static volatile sig_atomic_t g_stop_sig    = 0;
static volatile sig_atomic_t g_backup_req      = 0;

static sigset_t g_oldmask;   /* sigsuspend 에서 사용할 "기존 마스크" (모든 시그널 허용 상태) */

static void on_sigchld(int sig) { (void)sig; g_got_sigchld = 1; }
static void on_stop(int sig)    { g_stop = 1; g_stop_sig = sig; }
static void on_sigusr1(int sig) { (void)sig; g_backup_req = 1; }   /* 즉시 백업 요청 */

/* 시그널 핸들러 등록 도우미 (ch8: sigaction) */
static int install_handler(int sig, void (*handler)(int), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = flags;
    return sigaction(sig, &sa, NULL);
}

/* 필요한 시그널 설정. 성공 0, 실패 -1 */
static int setup_signals(void)
{
    sigset_t block;

    if (install_handler(SIGCHLD, on_sigchld, SA_RESTART | SA_NOCLDSTOP) < 0 ||
        install_handler(SIGINT,  on_stop,    0) < 0 ||
        install_handler(SIGTERM, on_stop,    0) < 0 ||
        install_handler(SIGUSR1, on_sigusr1, SA_RESTART) < 0 ||
        install_handler(SIGPIPE, SIG_IGN,    0) < 0) {   /* logger 가 죽어도 supervisor 는 살아남게 */
        perror("sigaction");
        return -1;
    }

    /* 평소에는 관심 시그널을 막아 두고, sigsuspend 안에서만 허용한다.
     * -> 플래그 검사와 대기 사이에 시그널이 끼어들어 놓치는 경쟁 상태를 막는다. */
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &block, &g_oldmask) < 0) {
        perror("sigprocmask");
        return -1;
    }
    return 0;
}

/* ---------- logger 프로세스 시작 ---------- */
static int start_logger(const char *logpath)
{
    int pfd[2];
    pid_t pid;

    if (pipe(pfd) < 0) {                  /* ch10: pfd[0]=읽기, pfd[1]=쓰기 */
        perror("pipe");
        return -1;
    }

    pid = fork();                         /* ch7: 자식 프로세스 생성 */
    if (pid < 0) {
        perror("fork(logger)");
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        /* ---- 자식(logger) ---- */
        close(pfd[1]);                    /* 쓰기 끝을 닫아야 EOF 를 감지할 수 있다 */
        _exit(logger_run(pfd[0], logpath) == 0 ? 0 : 1);
    }

    /* ---- 부모(supervisor) ---- */
    close(pfd[0]);                        /* 읽기 끝은 logger 만 사용 */
    g_logger_pid  = pid;
    g_log_pipe_wr = pfd[1];
    log_init_fd(pfd[1]);                  /* 이후 log_write 는 pipe 로 전송된다 */
    return 0;
}

/* ---------- worker 실행 ---------- */

/*
 * init_bindir - 이 프로그램(server)이 있는 디렉터리를 g_bindir 에 저장한다.
 * 덕분에 어느 디렉터리에서 실행해도 같은 위치의 monitor, backup 을 찾을 수 있다.
 *   1) argv[0] 에 '/' 가 있으면(./build/server 처럼 경로로 실행) 그 디렉터리를 쓴다.
 *   2) 없으면(PATH 로 실행) /proc/self/exe 를 readlink 로 읽는다.
 *      /proc/self/exe 는 자기 자신의 실행 파일을 가리키는 심벌릭 링크이다.
 */
static void init_bindir(const char *argv0)
{
    char *slash;

    if (argv0 != NULL && strchr(argv0, '/') != NULL) {
        snprintf(g_bindir, sizeof(g_bindir), "%s", argv0);
    } else {
        ssize_t n = readlink("/proc/self/exe", g_bindir, sizeof(g_bindir) - 1);   /* ch3 */
        if (n < 0) {                      /* 실패하면 현재 디렉터리의 build 를 가정 */
            snprintf(g_bindir, sizeof(g_bindir), "build");
            return;
        }
        g_bindir[n] = '\0';
    }
    slash = strrchr(g_bindir, '/');
    if (slash != NULL)
        *slash = '\0';
    else
        snprintf(g_bindir, sizeof(g_bindir), ".");
}

/*
 * fork_exec - 자식 프로세스를 만들어 path 를 exec 한다.
 * 반환: 성공 시 자식 pid, 실패 -1
 */
static pid_t fork_exec(const char *name, const char *path, char *const argv[])
{
    pid_t pid;
    char fdstr[16];

    pid = fork();                                         /* ch7 */
    if (pid < 0) {
        log_write(LOG_ERROR, "fork 실패(%s): %s", name, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* ---- 자식 ---- */
        sigprocmask(SIG_SETMASK, &g_oldmask, NULL);       /* 부모가 막아 둔 시그널 마스크 해제 */
        snprintf(fdstr, sizeof(fdstr), "%d", g_log_pipe_wr);
        setenv(LOG_FD_ENV, fdstr, 1);                     /* ch6: worker 가 로그 fd 를 알 수 있게 전달 */
        execv(path, argv);                                /* ch7: 새 프로그램으로 교체 */
        perror("execv");                                  /* 여기까지 왔다면 exec 실패 */
        _exit(127);
    }
    return pid;
}

/*
 * spawn_worker - worker 를 등록하고 실행한다.
 * @name: 로그에 쓸 이름   @path: 실행 파일   @argv: NULL 로 끝나는 인자 배열
 * @restart: 1 이면 비정상 종료 시 자동 재시작
 * 반환: 성공 0, 실패 -1
 */
static int spawn_worker(const char *name, const char *path, char *const argv[], int restart)
{
    worker_t *w;
    pid_t pid;

    if (g_nworkers >= MAX_WORKERS) {
        log_write(LOG_ERROR, "worker 가 너무 많음: %s", name);
        return -1;
    }
    pid = fork_exec(name, path, argv);
    if (pid < 0)
        return -1;

    w = &g_workers[g_nworkers++];
    snprintf(w->name, sizeof(w->name), "%s", name);
    snprintf(w->path, sizeof(w->path), "%s", path);
    w->pid      = pid;
    w->restart  = restart;
    w->restarts = 0;
    w->started  = time(NULL);
    log_write(LOG_INFO, "worker 시작: %s (pid=%d, path=%s)", name, (int)pid, path);
    return 0;
}

/*
 * restart_worker - 종료된 worker(idx 번)를 다시 실행한다.
 * 계속 죽는 worker 가 무한히 재시작되는 것을 막기 위해 횟수에 상한을 둔다.
 */
static void restart_worker(int idx)
{
    worker_t *w = &g_workers[idx];
    char *argv[] = { w->name, NULL };
    pid_t pid;

    if (time(NULL) - w->started >= RESTART_RESET_SEC)     /* 오래 잘 돌았다면 횟수 초기화 */
        w->restarts = 0;
    if (w->restarts >= MAX_RESTARTS) {
        log_write(LOG_ERROR, "%s 가 계속 종료되어 재시작을 포기합니다 (%d회)", w->name, MAX_RESTARTS);
        return;
    }

    w->restarts++;
    pid = fork_exec(w->name, w->path, argv);
    if (pid < 0)
        return;
    w->pid = pid;
    w->started = time(NULL);
    log_write(LOG_WARN, "%s 재시작 (%d/%d, 새 pid=%d)", w->name, w->restarts, MAX_RESTARTS, (int)pid);
}

/* ---------- 종료된 자식 회수 ---------- */
/*
 * reap_children - 종료된 자식을 모두 waitpid 로 회수하고 결과를 로그에 남긴다.
 * SIGCHLD 는 여러 개가 합쳐져 한 번만 올 수 있으므로 WNOHANG 으로 반복 호출한다.
 * 서버가 종료 중(g_stop)이 아닌데 재시작 대상 worker 가 끝났다면 다시 실행한다.
 */
static void reap_children(void)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {   /* ch7: 좀비 방지 */
        const char *name = "unknown";
        int i, idx = -1;

        if (pid == g_logger_pid) {
            name = "logger";
            g_logger_pid = -1;
        } else {
            for (i = 0; i < g_nworkers; i++) {
                if (g_workers[i].pid == pid) {
                    name = g_workers[i].name;
                    g_workers[i].pid = -1;
                    idx = i;
                    break;
                }
            }
        }

        if (WIFEXITED(status))
            log_write(LOG_INFO, "%s (pid=%d) 종료: exit code %d",
                      name, (int)pid, WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            log_write(LOG_WARN, "%s (pid=%d) 시그널로 종료: %d (%s)",
                      name, (int)pid, WTERMSIG(status), strsignal(WTERMSIG(status)));

        if (idx >= 0 && g_workers[idx].restart && !g_stop)
            restart_worker(idx);
    }
}

/* 실행 중인 worker 수 */
static int count_alive(void)
{
    int i, n = 0;
    for (i = 0; i < g_nworkers; i++)
        if (g_workers[i].pid > 0)
            n++;
    return n;
}

/*
 * stop_all_workers - 모든 worker 에게 SIGTERM 을 보내고 최대 3초 기다린다.
 * 그래도 남아 있으면 SIGKILL 로 강제 종료한다.
 */
static void stop_all_workers(void)
{
    int i, tries;

    for (i = 0; i < g_nworkers; i++)
        if (g_workers[i].pid > 0)
            kill(g_workers[i].pid, SIGTERM);              /* ch8: 시그널 전송 */

    for (tries = 0; tries < 30 && count_alive() > 0; tries++) {
        reap_children();
        if (count_alive() > 0)
            usleep(100000);                               /* 0.1초 */
    }

    for (i = 0; i < g_nworkers; i++) {
        if (g_workers[i].pid > 0) {
            log_write(LOG_WARN, "%s 가 종료되지 않아 SIGKILL 전송", g_workers[i].name);
            kill(g_workers[i].pid, SIGKILL);
            waitpid(g_workers[i].pid, NULL, 0);
            g_workers[i].pid = -1;
        }
    }
    reap_children();
}

/* ---------- ctl 명령 처리 ---------- */

/* 이름이 name 이고 실행 중인 worker 를 찾는다. 없으면 NULL */
static worker_t *find_worker(const char *name)
{
    int i;
    for (i = 0; i < g_nworkers; i++)
        if (g_workers[i].pid > 0 && strcmp(g_workers[i].name, name) == 0)
            return &g_workers[i];
    return NULL;
}

/* buf 뒤에 printf 형식으로 덧붙인다 (버퍼가 차면 조용히 잘라 낸다) */
static void append(char *buf, size_t size, const char *fmt, ...)
{
    size_t len = strlen(buf);
    va_list ap;

    if (len >= size - 1)
        return;
    va_start(ap, fmt);
    vsnprintf(buf + len, size - len, fmt, ap);
    va_end(ap);
}

/* status 응답: supervisor 와 worker 들의 상태 문자열을 만든다 */
static void build_status(char *buf, size_t size)
{
    int i;

    buf[0] = '\0';
    append(buf, size, "===== supervisor =====\n");
    append(buf, size, "supervisor : pid=%d, 가동 %ld초, 기록한 로그 %lu줄\n",
           (int)getpid(), (long)(time(NULL) - g_start_time), log_count());
    append(buf, size, "logger     : pid=%d %s\n", (int)g_logger_pid,
           g_logger_pid > 0 ? "(실행 중)" : "(종료됨)");
    append(buf, size, "logfile    : %s\n", g_logpath);   /* ctl log / watch 가 읽을 파일 */
    for (i = 0; i < g_nworkers; i++) {
        if (g_workers[i].pid > 0)
            append(buf, size, "%-10s : pid=%d (실행 중, 재시작 %d회, 가동 %ld초)\n",
                   g_workers[i].name, (int)g_workers[i].pid, g_workers[i].restarts,
                   (long)(time(NULL) - g_workers[i].started));
        else
            append(buf, size, "%-10s : 종료됨\n", g_workers[i].name);
    }
    append(buf, size, "\n");
}

/* ctl 명령 하나를 처리하고 응답을 돌려보낸다 */
static void handle_command(const cmd_msg_t *m)
{
    char reply[2048];
    worker_t *w;

    reply[0] = '\0';
    switch (m->type) {
    case CMD_STATUS:
        build_status(reply, sizeof(reply));
        break;
    case CMD_BACKUP:
        w = find_worker("backup");
        if (w != NULL) {
            kill(w->pid, SIGUSR1);                        /* ch8: backup worker 에게 "지금 백업" 신호 */
            log_write(LOG_INFO, "ctl 요청: 즉시 백업 (backup pid=%d)", (int)w->pid);
            append(reply, sizeof(reply), "backup worker(pid=%d)에 즉시 백업을 요청했습니다.\n", (int)w->pid);
        } else {
            append(reply, sizeof(reply), "backup worker 가 실행 중이 아닙니다.\n");
        }
        break;
    case CMD_STOP:
        log_write(LOG_INFO, "ctl 요청: 서버 종료");
        append(reply, sizeof(reply), "서버를 종료합니다.\n");
        g_stop = 1;                                       /* main 루프가 안전 종료 절차를 수행 */
        break;
    default:
        append(reply, sizeof(reply), "알 수 없는 명령입니다: %d\n", (int)m->type);
        break;
    }

    if (m->client_pid > 0 && reply_send(m->client_pid, reply) < 0)
        log_write(LOG_WARN, "ctl(pid=%d)에 응답 실패: %s", (int)m->client_pid, strerror(errno));
}

/* FIFO 에 쌓인 명령을 모두 처리한다 */
static void handle_commands(void)
{
    cmd_msg_t m;
    int r;

    while ((r = cmd_recv(g_cmd_fd, &m)) > 0)
        handle_command(&m);
    if (r < 0)
        log_write(LOG_WARN, "명령 FIFO 읽기 오류: %s", strerror(errno));
}

/* ---------- 사용법 ---------- */
static void usage(const char *prog)
{
    fprintf(stderr,
            "사용법: %s [-l 로그파일] [-d] [-h]\n"
            "  -l 파일   로그 파일 경로 (기본: %s)\n"
            "  -d        데모 모드: sleep 프로세스를 worker 로 띄워 supervisor 동작 확인\n"
            "  -h        도움말\n", prog, LOG_PATH_DEFAULT);
}

int main(int argc, char *argv[])
{
    const char *logpath = LOG_PATH_DEFAULT;
    int demo = 0;
    int opt;

    /* ch1: getopt 로 명령행 옵션 처리 */
    while ((opt = getopt(argc, argv, "l:dh")) != -1) {
        switch (opt) {
        case 'l': logpath = optarg; break;
        case 'd': demo = 1;         break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    init_bindir(argv[0]);

    /* 로그 경로를 절대 경로로 저장: ctl 이 다른 디렉터리에서 실행돼도 로그를 찾을 수 있게 한다 */
    if (logpath[0] == '/' || getcwd(g_logpath, sizeof(g_logpath)) == NULL) {
        snprintf(g_logpath, sizeof(g_logpath), "%s", logpath);
    } else {
        size_t len = strlen(g_logpath);
        snprintf(g_logpath + len, sizeof(g_logpath) - len, "/%s", logpath);
    }

    /* 1) logger 먼저 시작 (자식이 기본 시그널 설정을 갖도록 setup_signals 보다 먼저 호출) */
    if (start_logger(logpath) < 0)
        return 1;
    log_set_echo(1);                                      /* 터미널에도 같이 출력 */
    log_write(LOG_INFO, "supervisor 시작 (pid=%d, log=%s)", (int)getpid(), logpath);

    /* 2) 시그널 설정 */
    if (setup_signals() < 0) {
        log_write(LOG_ERROR, "시그널 설정 실패");
        log_close();
        waitpid(g_logger_pid, NULL, 0);
        return 1;
    }

    /* 2-1) ctl 명령 FIFO 생성 (worker 보다 먼저 만들어도 O_CLOEXEC 라 worker 는 물려받지 않는다) */
    g_start_time = time(NULL);
    g_cmd_fd = cmd_fifo_open_server();
    if (g_cmd_fd < 0) {
        log_write(LOG_ERROR, "명령 FIFO 생성 실패");
        log_close();
        waitpid(g_logger_pid, NULL, 0);
        return 1;
    }

    /* 3) worker 실행: 실행 파일이 있을 때만 */
    {
        static const char *names[] = { "monitor", "backup" };
        size_t k;

        for (k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
            char path[PATH_MAX];
            char *wargv[] = { (char *)names[k], NULL };

            if (snprintf(path, sizeof(path), "%s/%s", g_bindir, names[k]) >= (int)sizeof(path)) {
                log_write(LOG_ERROR, "경로가 너무 김: %s/%s", g_bindir, names[k]);
                continue;
            }
            if (access(path, X_OK) == 0)                  /* ch3: 실행 가능 여부 확인 */
                spawn_worker(names[k], path, wargv, 1);   /* 1: 죽으면 자동 재시작 */
            else
                log_write(LOG_WARN, "%s 없음 -> %s 건너뜀", path, names[k]);
        }
    }

    if (demo) {
        /* 데모: 3초 뒤 스스로 끝나는 worker 와 계속 도는 worker */
        char *a1[] = { "sleep", "3",   NULL };
        char *a2[] = { "sleep", "600", NULL };
        spawn_worker("demo-short", "/bin/sleep", a1, 0);
        spawn_worker("demo-long",  "/bin/sleep", a2, 0);
    }

    /* 4) 메인 루프: 시그널이 올 때까지 잠들고, 깨어나면 플래그를 처리 */
    log_write(LOG_INFO, "대기 중... (종료: Ctrl-C 또는 kill -TERM %d)", (int)getpid());
    while (!g_stop) {
        if (g_got_sigchld) {
            g_got_sigchld = 0;
            reap_children();
        }
        if (g_backup_req) {
            g_backup_req = 0;
            /* kill -USR1 <server pid> 로도 ctl backup 과 같은 즉시 백업을 요청할 수 있다 */
            worker_t *bw = find_worker("backup");
            if (bw != NULL) {
                kill(bw->pid, SIGUSR1);                   /* ch8: backup worker 에 전달 */
                log_write(LOG_INFO, "SIGUSR1 수신: backup worker(pid=%d)에 즉시 백업 전달", (int)bw->pid);
            } else {
                log_write(LOG_WARN, "SIGUSR1 수신: backup worker 가 실행 중이 아님");
            }
        }
        /* 시그널 또는 ctl 명령(FIFO 입력)이 올 때까지 대기.
         * pselect 는 sigsuspend 처럼 "마스크 해제 + 대기"를 원자적으로 수행하므로
         * 플래그 검사와 대기 사이에 온 시그널을 놓치지 않는다. */
        {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(g_cmd_fd, &rfds);
            if (pselect(g_cmd_fd + 1, &rfds, NULL, NULL, NULL, &g_oldmask) > 0 &&
                FD_ISSET(g_cmd_fd, &rfds))
                handle_commands();
        }
    }

    /* 5) 안전 종료 */
    if (g_stop_sig != 0)
        log_write(LOG_INFO, "종료 시그널 수신 (%d: %s) -> worker 정리",
                  (int)g_stop_sig, strsignal(g_stop_sig));
    else
        log_write(LOG_INFO, "ctl stop 명령 수신 -> worker 정리");
    stop_all_workers();
    cmd_fifo_close_server(g_cmd_fd);   /* FIFO 닫고 삭제 */
    log_write(LOG_INFO, "supervisor 종료 (총 %lu줄 기록)", log_count() + 1);

    log_close();                       /* pipe 쓰기 끝을 닫으면 logger 가 EOF 를 받고 끝난다 */
    if (g_logger_pid > 0)
        waitpid(g_logger_pid, NULL, 0);
    return 0;
}

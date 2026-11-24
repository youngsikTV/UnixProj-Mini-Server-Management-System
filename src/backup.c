/*
 * backup.c - backup worker (supervisor 가 exec 로 실행하는 독립 프로그램)
 *
 * 역할
 *   원본 디렉터리를 재귀적으로 순회하며 "변경된 파일만" 백업 디렉터리로 복사한다.
 *   - 주기적 실행: setitimer(SIGALRM)
 *   - 즉시 실행  : SIGUSR1 (5일차에 ctl 이 supervisor 를 통해 전달할 예정)
 *   - 종료       : SIGTERM / SIGINT (진행 중이던 파일까지만 처리하고 정리)
 *
 * 복사는 스레드 풀이 병렬로 처리한다.
 *   - 디렉터리를 순회하는 스레드(producer)가 "복사할 파일"을 작업 큐에 넣고,
 *   - worker 스레드들(consumer)이 큐에서 꺼내 복사한다.
 *   - 큐와 통계는 mutex 로 보호하고, 큐가 비었거나 가득 찬 경우는 조건 변수로 기다린다.
 *
 * 변경 판정: 백업본이 없거나, 크기 또는 수정 시각(mtime, 나노초 단위)이 다르면 복사한다.
 * 복사 후 백업본의 권한과 mtime 을 원본과 똑같이 맞춰 두므로, 다음 번에는 건너뛴다.
 *
 * 사용한 수업 내용
 *   ch1  getopt, 명령행 인자
 *   ch2  opendir / readdir / closedir / mkdir / rename
 *   ch3  lstat / S_ISDIR 등 / fchmod / symlink / readlink / unlink / utimensat
 *   ch4  open / read / write / close (저수준 파일 입출력)
 *   ch5  time
 *   ch6  getenv (로그 fd)
 *   ch8  sigaction / setitimer / sigprocmask / sigsuspend
 *   Pthread  pthread_create / join / mutex / 조건 변수 (복사 스레드 풀)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "common.h"
#include "logger.h"

#define SRC_DEFAULT       "data"          /* 기본 원본 디렉터리 (작업 디렉터리 기준) */
#define DST_DEFAULT       "backup_out"    /* 기본 백업 디렉터리 */
#define INTERVAL_DEFAULT  30              /* 기본 백업 주기(초) */
#define COPY_BUF_SIZE     65536           /* 복사용 버퍼 (스레드마다 하나) */
#define THREADS_DEFAULT   4               /* 기본 복사 스레드 수 */
#define THREADS_MAX       16              /* 최대 복사 스레드 수 */
#define QUEUE_SIZE        64              /* 작업 큐 크기 */

/* ---------- 시그널 플래그 (핸들러에서는 플래그만 변경) ---------- */
static volatile sig_atomic_t g_run  = 0;  /* 1 이면 백업을 한 번 수행 (SIGALRM / SIGUSR1) */
static volatile sig_atomic_t g_stop = 0;  /* 1 이면 종료 */

static void on_run(int sig)  { (void)sig; g_run = 1; }
static void on_stop(int sig) { (void)sig; g_stop = 1; }

static int install_handler(int sig, void (*handler)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;             /* 복사 중 시그널이 와도 read/write 가 끊기지 않게 */
    return sigaction(sig, &sa, NULL);
}

/* ---------- 이번 실행의 통계 ---------- */
typedef struct {
    unsigned long copied;       /* 복사한 파일 수 */
    unsigned long skipped;      /* 변경 없어 건너뛴 파일 수 */
    unsigned long dirs;         /* 새로 만든 디렉터리 수 */
    unsigned long links;        /* 새로 만들거나 갱신한 심벌릭 링크 수 */
    unsigned long errors;       /* 오류 수 */
    unsigned long long bytes;   /* 복사한 총 바이트 */
} stats_t;

static stats_t     g_st;
static int         g_nthreads = THREADS_DEFAULT;      /* 복사 스레드 수 (-t 옵션) */

/* 통계는 여러 스레드가 동시에 갱신하므로 mutex 로 보호한다 */
static pthread_mutex_t g_st_lock = PTHREAD_MUTEX_INITIALIZER;
#define ST_ADD(field, n) do {                       \
        pthread_mutex_lock(&g_st_lock);             \
        g_st.field += (n);                          \
        pthread_mutex_unlock(&g_st_lock);           \
    } while (0)
static struct stat g_dst_root;  /* 백업 디렉터리 자신(원본 안에 있을 때 재귀 방지용) */

/* ---------- 파일/디렉터리 도우미 ---------- */

/* fd 에 len 바이트를 모두 쓴다. 성공 0, 실패 -1 */
static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* path 의 모든 상위 디렉터리를 만든다 (mkdir -p). 성공 0, 실패 -1 */
static int make_dirs(const char *path)
{
    char tmp[PATH_MAX];
    char *p;

    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
        return -1;
    for (p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* 경로 두 개를 합친다: out = dir + "/" + name. 너무 길면 -1 */
static int join_path(char *out, size_t n, const char *dir, const char *name)
{
    return snprintf(out, n, "%s/%s", dir, name) >= (int)n ? -1 : 0;
}

/*
 * needs_copy - 원본 파일 st 를 dst 로 복사해야 하는지 판단한다.
 * 반환: 복사 필요 1, 불필요 0
 */
static int needs_copy(const char *dst, const struct stat *st)
{
    struct stat ds;

    if (lstat(dst, &ds) < 0)                       /* 백업본이 없음 */
        return 1;
    if (!S_ISREG(ds.st_mode))
        return 1;
    if (ds.st_size != st->st_size || ds.st_mtim.tv_sec != st->st_mtim.tv_sec)
        return 1;
    /* 같은 초 안에 같은 크기로 수정된 파일도 잡아내기 위해 나노초까지 비교한다.
     * 단, 백업 위치의 파일 시스템이 나노초를 저장하지 못하면(nsec 가 0) 비교를 생략한다. */
    return ds.st_mtim.tv_nsec != 0 && ds.st_mtim.tv_nsec != st->st_mtim.tv_nsec;
}

/*
 * copy_file - src 를 dst 로 복사한다.
 * 임시 파일에 먼저 쓰고 rename 으로 교체하므로, 복사 도중에 멈춰도 불완전한 백업본이
 * 남지 않는다. 복사 후 권한과 mtime 을 원본과 같게 맞춘다.
 * buf 는 호출한 스레드 전용 복사 버퍼(COPY_BUF_SIZE 바이트)이다. 성공 0, 실패 -1
 */
static int copy_file(const char *src, const char *dst, const struct stat *st, char *buf)
{
    char tmp[PATH_MAX];
    struct timespec times[2];
    unsigned long long total = 0;   /* 이 파일에서 복사한 바이트 */
    int in, out;
    ssize_t n;

    if (snprintf(tmp, sizeof(tmp), "%s.tmp~", dst) >= (int)sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    in = open(src, O_RDONLY);                              /* ch4: 원본 열기 */
    if (in < 0)
        return -1;
    out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);   /* 임시 파일 */
    if (out < 0) {
        close(in);
        return -1;
    }

    while ((n = read(in, buf, COPY_BUF_SIZE)) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            goto fail;
        }
        if (write_all(out, buf, (size_t)n) < 0)
            goto fail;
        total += (unsigned long long)n;
    }

    if (fchmod(out, st->st_mode & 07777) < 0)              /* ch3: 권한 보존 */
        goto fail;
    close(in);
    if (close(out) < 0) {
        unlink(tmp);
        return -1;
    }

    times[0] = st->st_mtim;                                /* atime 도 mtime 으로 */
    times[1] = st->st_mtim;
    utimensat(AT_FDCWD, tmp, times, 0);                    /* mtime 보존 -> 다음 번 비교에 사용 */

    if (rename(tmp, dst) < 0) {                            /* ch2: 완성된 파일로 교체 */
        unlink(tmp);
        return -1;
    }
    ST_ADD(bytes, total);
    return 0;

fail:
    {
        int saved = errno;
        close(in);
        close(out);
        unlink(tmp);
        errno = saved;
    }
    return -1;
}

/* 심벌릭 링크를 백업한다 (링크 자체를 복사). 성공 0, 실패 -1 */
static int backup_link(const char *src, const char *dst)
{
    char target[PATH_MAX], cur[PATH_MAX];
    ssize_t n, m;

    n = readlink(src, target, sizeof(target) - 1);         /* ch3: 링크가 가리키는 경로 */
    if (n < 0)
        return -1;
    target[n] = '\0';

    m = readlink(dst, cur, sizeof(cur) - 1);
    if (m >= 0) {
        cur[m] = '\0';
        if (strcmp(cur, target) == 0)                      /* 이미 같은 링크 */
            return 1;
        unlink(dst);                                       /* 다르면 지우고 다시 만든다 */
    } else if (errno != ENOENT) {
        unlink(dst);
    }
    return symlink(target, dst) < 0 ? -1 : 0;
}

/* ---------- 복사 스레드 풀 ---------- */

typedef struct {
    char        src[PATH_MAX];
    char        dst[PATH_MAX];
    struct stat st;             /* 변경 판단과 권한/mtime 복원에 쓰는 원본 정보 */
} job_t;

static job_t           g_queue[QUEUE_SIZE];   /* 원형 큐 */
static int             g_q_head, g_q_tail, g_q_count;
static int             g_q_done;              /* 1 이면 더 이상 작업이 들어오지 않음 */
static pthread_mutex_t g_q_lock      = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_q_not_empty = PTHREAD_COND_INITIALIZER;   /* 작업이 생김 */
static pthread_cond_t  g_q_not_full  = PTHREAD_COND_INITIALIZER;   /* 빈 자리가 생김 */

/* 작업 큐에 복사 작업을 넣는다. 큐가 가득 차 있으면 자리가 날 때까지 기다린다. */
static void enqueue_job(const char *src, const char *dst, const struct stat *st)
{
    job_t *j;

    pthread_mutex_lock(&g_q_lock);
    while (g_q_count == QUEUE_SIZE)
        pthread_cond_wait(&g_q_not_full, &g_q_lock);   /* mutex 를 풀고 잠들었다가 깨면 다시 잡는다 */

    j = &g_queue[g_q_tail];
    snprintf(j->src, sizeof(j->src), "%s", src);
    snprintf(j->dst, sizeof(j->dst), "%s", dst);
    j->st = *st;
    g_q_tail = (g_q_tail + 1) % QUEUE_SIZE;
    g_q_count++;

    pthread_cond_signal(&g_q_not_empty);               /* 기다리는 worker 하나를 깨운다 */
    pthread_mutex_unlock(&g_q_lock);
}

/* worker 스레드: 큐에서 작업을 꺼내 복사하는 일을 큐가 닫히고 빌 때까지 반복한다 */
static void *copy_thread(void *arg)
{
    static const int sigs[] = { SIGALRM, SIGUSR1, SIGTERM, SIGINT };
    char buf[COPY_BUF_SIZE];                           /* 스레드마다 자기 버퍼를 쓴다 */
    sigset_t block;
    size_t i;

    (void)arg;

    /* 시그널은 항상 메인 스레드가 받도록 이 스레드에서는 막아 둔다 */
    sigemptyset(&block);
    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaddset(&block, sigs[i]);
    pthread_sigmask(SIG_BLOCK, &block, NULL);

    for (;;) {
        job_t j;

        pthread_mutex_lock(&g_q_lock);
        while (g_q_count == 0 && !g_q_done)
            pthread_cond_wait(&g_q_not_empty, &g_q_lock);
        if (g_q_count == 0) {                          /* 닫혔고 남은 작업도 없음 -> 종료 */
            pthread_mutex_unlock(&g_q_lock);
            break;
        }
        j = g_queue[g_q_head];                         /* 작업을 복사해 가져온 뒤 바로 mutex 해제 */
        g_q_head = (g_q_head + 1) % QUEUE_SIZE;
        g_q_count--;
        pthread_cond_signal(&g_q_not_full);            /* producer 가 기다리고 있다면 깨운다 */
        pthread_mutex_unlock(&g_q_lock);

        if (g_stop)                                    /* 종료 요청이 오면 남은 작업은 버린다 */
            continue;

        if (copy_file(j.src, j.dst, &j.st, buf) == 0) {
            ST_ADD(copied, 1);
            log_write(LOG_DEBUG, "복사: %s (%lld바이트)", j.src, (long long)j.st.st_size);
        } else {
            log_write(LOG_ERROR, "복사 실패 %s: %s", j.src, strerror(errno));
            ST_ADD(errors, 1);
        }
    }
    return NULL;
}

static pthread_t g_tids[THREADS_MAX];

/* worker 스레드를 g_nthreads 개 만든다. 실제로 만든 수를 반환 */
static int pool_start(void)
{
    int i, n = 0;

    g_q_head = g_q_tail = g_q_count = 0;
    g_q_done = 0;
    for (i = 0; i < g_nthreads; i++) {
        int rc = pthread_create(&g_tids[n], NULL, copy_thread, NULL);
        if (rc != 0) {
            log_write(LOG_ERROR, "pthread_create 실패: %s", strerror(rc));
            break;
        }
        n++;
    }
    return n;
}

/* 작업 입력이 끝났음을 알리고, 모든 worker 스레드가 끝나기를 기다린다 */
static void pool_finish(int n)
{
    int i;

    pthread_mutex_lock(&g_q_lock);
    g_q_done = 1;
    pthread_cond_broadcast(&g_q_not_empty);            /* 잠든 worker 를 전부 깨워 종료하게 함 */
    pthread_mutex_unlock(&g_q_lock);

    for (i = 0; i < n; i++)
        pthread_join(g_tids[i], NULL);
}

/*
 * backup_dir - srcdir 를 dstdir 로 재귀 백업한다.
 * 개별 파일의 오류는 통계에 기록하고 계속 진행한다. (g_stop 이면 중단)
 */
static void backup_dir(const char *srcdir, const char *dstdir)
{
    DIR *dp;
    struct dirent *de;
    struct stat st;

    dp = opendir(srcdir);                                  /* ch2 */
    if (dp == NULL) {
        log_write(LOG_ERROR, "opendir 실패 %s: %s", srcdir, strerror(errno));
        ST_ADD(errors, 1);
        return;
    }

    while (!g_stop && (de = readdir(dp)) != NULL) {
        char sp[PATH_MAX], dp_path[PATH_MAX];

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (join_path(sp, sizeof(sp), srcdir, de->d_name) < 0 ||
            join_path(dp_path, sizeof(dp_path), dstdir, de->d_name) < 0) {
            log_write(LOG_ERROR, "경로가 너무 김: %s/%s", srcdir, de->d_name);
            ST_ADD(errors, 1);
            continue;
        }
        if (lstat(sp, &st) < 0) {                          /* ch3: 링크를 따라가지 않고 정보 조회 */
            log_write(LOG_ERROR, "lstat 실패 %s: %s", sp, strerror(errno));
            ST_ADD(errors, 1);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            /* 백업 디렉터리가 원본 안에 있으면 자기 자신을 다시 복사하게 되므로 제외 */
            if (st.st_dev == g_dst_root.st_dev && st.st_ino == g_dst_root.st_ino)
                continue;
            if (mkdir(dp_path, 0755) == 0)
                ST_ADD(dirs, 1);
            else if (errno != EEXIST) {
                log_write(LOG_ERROR, "mkdir 실패 %s: %s", dp_path, strerror(errno));
                ST_ADD(errors, 1);
                continue;
            }
            backup_dir(sp, dp_path);
            chmod(dp_path, st.st_mode & 07777);            /* 내용을 다 쓴 뒤 디렉터리 권한 보존 */
        } else if (S_ISREG(st.st_mode)) {
            if (!needs_copy(dp_path, &st))
                ST_ADD(skipped, 1);
            else
                enqueue_job(sp, dp_path, &st);             /* 실제 복사는 worker 스레드가 한다 */
        } else if (S_ISLNK(st.st_mode)) {
            int r = backup_link(sp, dp_path);
            if (r == 0)
                ST_ADD(links, 1);
            else if (r < 0) {
                log_write(LOG_ERROR, "링크 백업 실패 %s: %s", sp, strerror(errno));
                ST_ADD(errors, 1);
            }
        }
        /* 소켓, FIFO, 장치 파일 등은 백업 대상이 아니므로 건너뛴다 */
    }
    closedir(dp);
}

/* 백업을 한 번 수행하고 결과를 로그에 남긴다. 성공 0, 실패 -1 */
static int run_backup(const char *src, const char *dst)
{
    struct stat ss;
    time_t t0 = time(NULL);
    int nthreads;

    memset(&g_st, 0, sizeof(g_st));

    if (stat(src, &ss) < 0 || !S_ISDIR(ss.st_mode)) {
        log_write(LOG_ERROR, "원본 디렉터리를 찾을 수 없음: %s", src);
        return -1;
    }
    if (make_dirs(dst) < 0 || stat(dst, &g_dst_root) < 0) {
        log_write(LOG_ERROR, "백업 디렉터리 준비 실패 %s: %s", dst, strerror(errno));
        return -1;
    }

    nthreads = pool_start();
    if (nthreads == 0) {
        log_write(LOG_ERROR, "복사 스레드를 만들지 못해 백업을 중단합니다");
        return -1;
    }
    log_write(LOG_INFO, "백업 시작: %s -> %s (복사 스레드 %d개)", src, dst, nthreads);
    backup_dir(src, dst);
    pool_finish(nthreads);                           /* 큐가 빌 때까지 기다린 뒤 스레드 정리 */
    log_write(g_st.errors ? LOG_WARN : LOG_INFO,
              "백업 %s: 복사 %lu, 변경없음 %lu, 새 디렉터리 %lu, 링크 %lu, 오류 %lu, %llu바이트, %ld초",
              g_stop ? "중단" : "완료", g_st.copied, g_st.skipped, g_st.dirs, g_st.links,
              g_st.errors, g_st.bytes, (long)(time(NULL) - t0));
    return g_st.errors ? -1 : 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "사용법: %s [-s 원본] [-d 백업위치] [-i 주기(초)] [-t 스레드수] [-o] [-h]\n"
            "  -s 경로   백업할 원본 디렉터리 (기본: %s)\n"
            "  -d 경로   백업 저장 디렉터리   (기본: %s)\n"
            "  -i 초     자동 백업 주기       (기본: %d, 0 이면 자동 백업 없음)\n"
            "  -t 개수   복사 스레드 수 1~%d (기본: %d)\n"
            "  -o        한 번만 백업하고 종료\n"
            "시그널: SIGUSR1 = 즉시 백업, SIGTERM/SIGINT = 종료\n",
            prog, SRC_DEFAULT, DST_DEFAULT, INTERVAL_DEFAULT, THREADS_MAX, THREADS_DEFAULT);
}

int main(int argc, char *argv[])
{
    const char *src = SRC_DEFAULT, *dst = DST_DEFAULT;
    int interval = INTERVAL_DEFAULT, once = 0, opt;
    const char *fdstr;
    struct itimerval it;
    sigset_t block, oldmask;

    while ((opt = getopt(argc, argv, "s:d:i:t:oh")) != -1) {
        switch (opt) {
        case 's': src = optarg;           break;
        case 'd': dst = optarg;           break;
        case 'i': interval = atoi(optarg); break;
        case 't': g_nthreads = atoi(optarg); break;
        case 'o': once = 1;               break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }
    if (g_nthreads < 1 || g_nthreads > THREADS_MAX) {
        fprintf(stderr, "스레드 수는 1~%d 사이여야 합니다.\n", THREADS_MAX);
        return 1;
    }
    if (interval < 0) {
        fprintf(stderr, "주기는 0 이상이어야 합니다.\n");
        return 1;
    }

    /* supervisor 가 넘겨 준 로그 pipe fd 사용. 없으면 stderr 로 출력 */
    fdstr = getenv(LOG_FD_ENV);
    if (fdstr != NULL)
        log_init_fd(atoi(fdstr));
    else
        log_set_echo(1);

    if (once)                                    /* 단독 테스트용: 한 번 하고 끝 */
        return run_backup(src, dst) == 0 ? 0 : 1;

    if (install_handler(SIGALRM, on_run)   < 0 ||
        install_handler(SIGUSR1, on_run)   < 0 ||
        install_handler(SIGTERM, on_stop)  < 0 ||
        install_handler(SIGINT,  on_stop)  < 0) {
        perror("sigaction");
        return 1;
    }

    /* 평소에는 시그널을 막고 sigsuspend 안에서만 받아서 시그널 유실을 막는다 */
    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    sigaddset(&block, SIGUSR1);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGINT);
    sigprocmask(SIG_BLOCK, &block, &oldmask);

    if (interval > 0) {                          /* ch8: 주기 실행 타이머 */
        it.it_value.tv_sec     = interval;
        it.it_value.tv_usec    = 0;
        it.it_interval.tv_sec  = interval;
        it.it_interval.tv_usec = 0;
        if (setitimer(ITIMER_REAL, &it, NULL) < 0) {
            perror("setitimer");
            return 1;
        }
    }

    log_write(LOG_INFO, "backup 시작 (원본=%s, 백업=%s, 주기=%d초)", src, dst, interval);
    g_run = 1;                                   /* 시작하자마자 한 번 수행 */

    while (!g_stop) {
        if (g_run) {
            g_run = 0;
            /* 백업 중에는 시그널을 풀어 둔다: 핸들러는 플래그만 바꾸므로 안전하고,
             * 오래 걸리는 백업 도중에도 종료 요청(g_stop)을 바로 알아챌 수 있다 */
            sigprocmask(SIG_SETMASK, &oldmask, NULL);
            run_backup(src, dst);
            sigprocmask(SIG_BLOCK, &block, NULL);
            continue;                            /* 백업 중 도착한 요청 확인 */
        }
        sigsuspend(&oldmask);
    }

    it.it_value.tv_sec = it.it_value.tv_usec = 0;
    it.it_interval = it.it_value;
    setitimer(ITIMER_REAL, &it, NULL);
    log_write(LOG_INFO, "backup 종료");
    return 0;
}

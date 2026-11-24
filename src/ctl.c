/*
 * ctl.c - 제어 클라이언트 (CLI)
 *
 * 사용법
 *   ./build/ctl status    supervisor/worker 상태(FIFO 응답) + 시스템 상태(공유 메모리)
 *   ./build/ctl backup    backup worker 에게 즉시 백업 요청
 *   ./build/ctl stop      서버(supervisor)와 모든 worker 안전 종료
 *
 * 통신 방식
 *   명령/응답 : FIFO (ipc.c 의 ctl_request)  -> supervisor 와 대화
 *   시스템 상태: mmap 공유 메모리 직접 읽기   -> monitor 가 기록한 값
 *
 * 사용한 수업 내용
 *   ch5  time / localtime_r / strftime
 *   ch8  sigaction + alarm (서버 무응답 시 시간 초과)
 *   ch9  mmap 공유 메모리 읽기 (ipc.c)
 *   ch10 FIFO 로 명령 전송 / 응답 수신 (ipc.c)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "ipc.h"

#define REQUEST_TIMEOUT_SEC 3   /* 서버가 이 시간 안에 응답하지 않으면 포기 */

/* SIGALRM 핸들러: 아무것도 하지 않는다. 블록된 open/read 를 EINTR 로 깨우는 것이 목적 */
static void on_alarm(int sig)
{
    (void)sig;
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

/*
 * send_command - 서버에 명령을 보내고 응답 문자열을 out 에 받는다.
 * 실패하면 원인을 stderr 에 출력하고 -1 을 반환한다.
 */
static int send_command(cmd_type_t type, char *out, size_t outsz)
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

/* monitor 가 공유 메모리에 기록한 시스템 상태를 출력한다 */
static void print_monitor(void)
{
    const monitor_data_t *shm;
    monitor_data_t s;
    char ts[32], up[64];
    struct tm tmv;
    time_t now = time(NULL);
    double used_mb, total_mb;

    printf("===== 시스템 상태 (monitor) =====\n");
    shm = shm_map_readonly();
    if (shm == NULL) {
        printf("monitor 데이터 없음 (monitor 가 실행 중이 아님)\n");
        return;
    }
    if (shm_read_snapshot(shm, &s) < 0) {
        printf("공유 메모리를 일관되게 읽지 못했습니다. 다시 시도하세요.\n");
        shm_unmap(shm);
        return;
    }
    shm_unmap(shm);

    if (s.updated == 0) {              /* monitor 가 첫 값을 쓰기 전 */
        printf("아직 수집된 값이 없습니다. 잠시 후 다시 시도하세요.\n");
        return;
    }

    localtime_r(&s.updated, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    format_duration(s.uptime, up, sizeof(up));
    total_mb = s.totalram / 1048576.0;
    used_mb  = (s.totalram - s.freeram) / 1048576.0;

    printf("호스트 / 사용자 : %s / %s\n", s.host, s.user);
    printf("마지막 갱신     : %s (%ld초 전, 총 %lu회 갱신)\n",
           ts, (long)(now - s.updated), s.samples);
    printf("시스템 가동     : %s\n", up);
    printf("메모리 사용     : %.0f / %.0f MB (%.1f%%)\n",
           used_mb, total_mb, total_mb > 0 ? used_mb * 100.0 / total_mb : 0.0);
    printf("Load average(1m): %.2f\n", s.load1);
    printf("프로세스 수     : %u\n", (unsigned)s.procs);
    printf("monitor CPU     : user %.2fs / sys %.2fs\n", s.cpu_user, s.cpu_sys);

    if (now - s.updated > 10)          /* monitor 가 멈춘 경우 경고 */
        printf("[경고] 10초 넘게 갱신되지 않았습니다. monitor 상태를 확인하세요.\n");
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "사용법: %s <명령>\n"
            "  status   서버/worker 상태와 시스템 상태 출력\n"
            "  backup   즉시 백업 요청\n"
            "  stop     서버 종료\n", prog);
}

int main(int argc, char *argv[])
{
    char reply[2048];

    if (argc != 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        if (send_command(CMD_STATUS, reply, sizeof(reply)) < 0)
            return 1;
        fputs(reply, stdout);
        print_monitor();
        return 0;
    }
    if (strcmp(argv[1], "backup") == 0) {
        if (send_command(CMD_BACKUP, reply, sizeof(reply)) < 0)
            return 1;
        fputs(reply, stdout);
        return 0;
    }
    if (strcmp(argv[1], "stop") == 0) {
        if (send_command(CMD_STOP, reply, sizeof(reply)) < 0)
            return 1;
        fputs(reply, stdout);
        return 0;
    }

    fprintf(stderr, "알 수 없는 명령: %s\n", argv[1]);
    usage(argv[0]);
    return 1;
}

/*
 * common.h - 프로젝트 전체에서 공유하는 상수, 열거형, 구조체
 *
 * 미니 서버 관리 시스템 (supervisor + logger + monitor + backup + ctl)
 * 모든 모듈이 이 헤더를 include 한다.
 */
#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h>
#include <time.h>

/* ---------- 경로 / 크기 상수 ---------- */
#define LOG_PATH_DEFAULT "server.log"                /* 기본 로그 파일 */
#define FIFO_CMD_PATH    "/tmp/unixproj_cmd.fifo"    /* ctl -> server 명령 FIFO  (ch10) */
#define FIFO_RESP_PATH   "/tmp/unixproj_resp.fifo"   /* server -> ctl 응답 FIFO  (ch10) */
#define SHM_FILE_PATH    "/tmp/unixproj_monitor.shm" /* mmap 공유 메모리 파일     (ch9)  */

#define MONITOR_BIN      "build/monitor"             /* monitor worker 실행 파일 */
#define BACKUP_BIN       "build/backup"              /* backup worker 실행 파일  */

#define BUF_SIZE         512     /* 일반 문자열 버퍼 */
#define LOG_LINE_MAX     512     /* 로그 한 줄 최대 길이 (PIPE_BUF 이하여야 원자적 write) */
#define MAX_WORKERS      8       /* supervisor 가 관리하는 worker 최대 수 */

/* worker 가 exec 된 뒤 로그 파이프 fd 번호를 알 수 있도록 넘기는 환경 변수 (ch6 환경 변수) */
#define LOG_FD_ENV       "UNIXPROJ_LOG_FD"

/* ---------- 로그 레벨 ---------- */
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} log_level_t;

/* ---------- ctl -> server 명령 (4~5일차에 사용) ---------- */
typedef enum {
    CMD_STATUS = 1,   /* 현재 상태 조회   */
    CMD_BACKUP,       /* 즉시 백업 실행   */
    CMD_STOP          /* 서버 종료        */
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    pid_t      client_pid; /* 응답 FIFO 이름에 쓰이는 ctl 의 pid */
    char       arg[256];   /* 선택 인자 (예: 백업할 디렉터리) */
} cmd_msg_t;

/* ---------- monitor 가 공유 메모리(mmap)에 기록하는 구조체 (3일차에 사용) ---------- */
typedef struct {
    unsigned long seq;         /* 갱신 횟수 */
    unsigned long samples;     /* 지금까지 수집한 횟수 */
    time_t        updated;     /* 마지막 갱신 시각 */
    double        cpu_user;    /* times() 로 측정한 사용자 CPU 시간(초) */
    double        cpu_sys;     /* times() 로 측정한 시스템 CPU 시간(초) */
    double        load1;       /* sysinfo: 1분 평균 부하 */
    unsigned long uptime;      /* sysinfo: 부팅 후 경과 시간(초) */
    unsigned long totalram;    /* sysinfo: 전체 메모리 */
    unsigned long freeram;     /* sysinfo: 남은 메모리 */
    unsigned short procs;      /* sysinfo: 현재 프로세스 수 */
    char          user[32];    /* getpwuid: 실행 사용자 이름 */
    char          host[65];    /* uname: 호스트 이름 (nodename 최대 64자 + NUL) */
} monitor_data_t;

#define MONITOR_INTERVAL_SEC 2 /* monitor 기본 수집 주기(초) */

#endif /* COMMON_H */


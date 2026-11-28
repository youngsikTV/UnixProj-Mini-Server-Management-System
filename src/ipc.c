/*
 * ipc.c - IPC 모듈 (mmap 공유 메모리 + FIFO 명령 채널)
 *
 * 사용한 수업 내용
 *   ch4  open / ftruncate / close
 *   ch3  unlink (공유 파일 삭제)
 *   ch9  mmap / munmap, MAP_SHARED
 *   ch10 mkfifo / open(O_NONBLOCK) / read / write (ctl 명령과 응답)
 *
 * 동작 원리
 *   파일을 MAP_SHARED 로 mmap 하면 같은 파일을 매핑한 모든 프로세스가
 *   같은 물리 메모리를 본다. monitor 가 쓰면 ctl 이 바로 읽을 수 있다.
 *
 *   두 프로세스가 동시에 접근하므로 읽는 쪽이 "쓰다 만 값"을 볼 수 있다.
 *   이를 막기 위해 seq 를 이용한 seqlock 을 쓴다.
 *     쓰는 쪽 : seq 를 홀수로 -> 값 갱신 -> seq 를 짝수로
 *     읽는 쪽 : seq 가 짝수이고, 복사 전후의 seq 가 같을 때만 값을 인정
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "ipc.h"

monitor_data_t *shm_map_create(void)
{
    int fd;
    void *p;

    /* O_TRUNC: monitor 가 새로 시작하면 이전 값을 지우고 0 으로 시작 */
    fd = open(SHM_FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("shm_map_create: open");
        return NULL;
    }
    /* 매핑하려면 파일 크기가 구조체 크기 이상이어야 한다 (ch9: ftruncate) */
    if (ftruncate(fd, (off_t)sizeof(monitor_data_t)) < 0) {
        perror("shm_map_create: ftruncate");
        close(fd);
        return NULL;
    }
    p = mmap(NULL, sizeof(monitor_data_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);                         /* 매핑이 유지되므로 fd 는 닫아도 된다 */
    if (p == MAP_FAILED) {
        perror("shm_map_create: mmap");
        return NULL;
    }
    return (monitor_data_t *)p;
}

const monitor_data_t *shm_map_readonly(void)
{
    int fd;
    void *p;

    fd = open(SHM_FILE_PATH, O_RDONLY);
    if (fd < 0)
        return NULL;                   /* 파일이 없음 = monitor 가 아직 실행 안 됨 */
    p = mmap(NULL, sizeof(monitor_data_t), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        perror("shm_map_readonly: mmap");
        return NULL;
    }
    return (const monitor_data_t *)p;
}

void shm_unmap(const monitor_data_t *d)
{
    if (d != NULL)
        munmap((void *)d, sizeof(monitor_data_t));
}

void shm_remove(void)
{
    unlink(SHM_FILE_PATH);
}

void shm_update_begin(monitor_data_t *d)
{
    __atomic_add_fetch(&d->seq, 1, __ATOMIC_SEQ_CST);   /* 홀수: "쓰는 중" */
}

void shm_update_end(monitor_data_t *d)
{
    __atomic_add_fetch(&d->seq, 1, __ATOMIC_SEQ_CST);   /* 짝수: "쓰기 완료" */
}

int shm_read_snapshot(const monitor_data_t *src, monitor_data_t *out)
{
    int tries;

    for (tries = 0; tries < 1000; tries++) {
        unsigned long s1 = __atomic_load_n(&src->seq, __ATOMIC_ACQUIRE);
        unsigned long s2;

        if (s1 & 1UL) {                /* 쓰는 중 -> 잠시 후 재시도 */
            usleep(1000);
            continue;
        }
        memcpy(out, src, sizeof(*out));
        s2 = __atomic_load_n(&src->seq, __ATOMIC_ACQUIRE);
        if (s1 == s2)                  /* 복사하는 동안 갱신이 없었음 */
            return 0;
    }
    return -1;
}

/* ====================================================================
 *  FIFO 명령 채널 (ch10)
 * ==================================================================== */

/* buf 의 len 바이트를 모두 쓴다. 성공 0, 실패 -1 */
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

/* client_pid 전용 응답 FIFO 경로: /tmp/unixproj_resp.fifo.<pid> */
static void reply_path(pid_t pid, char *buf, size_t n)
{
    snprintf(buf, n, "%s.%d", FIFO_RESP_PATH, (int)pid);
}

int cmd_fifo_open_server(void)
{
    int fd;

    unlink(FIFO_CMD_PATH);                          /* 이전 실행에서 남은 FIFO 제거 */
    if (mkfifo(FIFO_CMD_PATH, 0600) < 0) {
        perror("cmd_fifo_open_server: mkfifo");
        return -1;
    }
    /* O_RDWR 로 여는 이유: 쓰는 쪽(ctl)이 하나도 없을 때 read 가 EOF 를 반환해
     *   select 가 계속 깨어나는 것을 막기 위해 서버 자신도 쓰기 끝을 하나 잡아 둔다.
     * O_NONBLOCK: 명령이 없을 때 read 가 멈추지 않게 한다.
     * O_CLOEXEC : exec 로 실행되는 worker 가 이 fd 를 물려받지 않게 한다. */
    fd = open(FIFO_CMD_PATH, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        perror("cmd_fifo_open_server: open");
        unlink(FIFO_CMD_PATH);
        return -1;
    }
    return fd;
}

void cmd_fifo_close_server(int fd)
{
    if (fd >= 0)
        close(fd);
    unlink(FIFO_CMD_PATH);
}

int cmd_recv(int fd, cmd_msg_t *msg)
{
    ssize_t n;

    do {
        n = read(fd, msg, sizeof(*msg));   /* 구조체가 PIPE_BUF 이하라 한 번에 통째로 읽힌다 */
    } while (n < 0 && errno == EINTR);

    if (n == (ssize_t)sizeof(*msg)) {
        msg->arg[sizeof(msg->arg) - 1] = '\0';
        return 1;
    }
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    return n == 0 ? 0 : -1;                /* 0: 비어 있음, 그 외: 크기가 맞지 않는 데이터 */
}

int reply_send(pid_t client_pid, const char *text)
{
    char path[128];
    int fd = -1, i;

    reply_path(client_pid, path, sizeof(path));
    /* ctl 이 읽기 끝을 열 때까지(ENXIO) 잠깐씩 기다리며 재시도한다. 논블로킹이라
     * ctl 이 죽어 있어도 서버가 여기서 영원히 멈추지 않는다. */
    for (i = 0; i < 20; i++) {
        fd = open(path, O_WRONLY | O_NONBLOCK);
        if (fd >= 0 || errno != ENXIO)
            break;
        usleep(50000);
    }
    if (fd < 0)
        return -1;
    i = write_all(fd, text, strlen(text));
    close(fd);                              /* 닫으면 ctl 의 read 가 EOF 를 받아 끝난다 */
    return i;
}

int ctl_request(const cmd_msg_t *msg, char *out, size_t outsz)
{
    cmd_msg_t m = *msg;
    char path[128];
    size_t used = 0;
    ssize_t n;
    int cfd, rfd, e;

    m.client_pid = getpid();
    reply_path(m.client_pid, path, sizeof(path));

    /* 1) 응답을 받을 내 전용 FIFO 생성 */
    unlink(path);
    if (mkfifo(path, 0600) < 0)
        return -1;

    /* 2) 명령 전송. 서버가 읽는 중이 아니면 open 이 ENXIO 로 실패한다 (서버 없음) */
    cfd = open(FIFO_CMD_PATH, O_WRONLY | O_NONBLOCK);
    if (cfd < 0) {
        e = errno;
        unlink(path);
        errno = e;
        return -1;
    }
    if (write_all(cfd, (const char *)&m, sizeof(m)) < 0) {
        e = errno;
        close(cfd);
        unlink(path);
        errno = e;
        return -1;
    }
    close(cfd);

    /* 3) 응답 수신: 서버가 쓰기 끝을 열 때까지 open 이 블록된다. 서버가 응답하지 않으면
     *    호출자가 설정한 alarm 의 SIGALRM 으로 EINTR 이 되어 빠져나온다. */
    rfd = open(path, O_RDONLY);
    if (rfd < 0) {
        e = errno;
        unlink(path);
        errno = e;
        return -1;
    }
    while (used < outsz - 1 && (n = read(rfd, out + used, outsz - 1 - used)) != 0) {
        if (n < 0) {
            e = errno;
            close(rfd);
            unlink(path);
            errno = e;
            return -1;
        }
        used += (size_t)n;
    }
    out[used] = '\0';
    close(rfd);
    unlink(path);
    return 0;
}


/*
 * ipc.h - 프로세스 간 통신(IPC) 모듈 인터페이스
 *
 * 제공 기능
 *   - mmap 공유 메모리 (monitor -> ctl), seqlock 으로 일관된 읽기
 *   - FIFO 명령 채널   (ctl <-> server)
 */
#ifndef IPC_H
#define IPC_H

#include "common.h"

/* ---------- mmap 공유 메모리 ---------- */

/* 쓰기용: 공유 파일을 만들고(크기 지정) 읽기/쓰기로 매핑한다. 실패 시 NULL */
monitor_data_t *shm_map_create(void);

/* 읽기용: 이미 존재하는 공유 파일을 읽기 전용으로 매핑한다. 파일이 없으면 NULL */
const monitor_data_t *shm_map_readonly(void);

/* 매핑 해제 */
void shm_unmap(const monitor_data_t *d);

/* 공유 파일 삭제 (monitor 종료 시 호출) */
void shm_remove(void);

/* 값을 갱신하기 전/후에 한 쌍으로 호출한다. (seqlock: 읽는 쪽이 중간 상태를 피함) */
void shm_update_begin(monitor_data_t *d);
void shm_update_end(monitor_data_t *d);

/* 일관된 복사본을 out 에 읽어 온다. 성공 0, 실패 -1 (writer 가 계속 쓰는 중) */
int  shm_read_snapshot(const monitor_data_t *src, monitor_data_t *out);

/* ---------- FIFO 명령 채널 (ctl <-> server) ----------
 *
 *   ctl --(cmd_msg_t)--> [FIFO_CMD_PATH] --> server
 *   ctl <--(응답 문자열)-- [FIFO_RESP_PATH.<ctl pid>] <-- server
 *
 * 응답 FIFO 를 ctl 마다 따로 만들기 때문에 여러 ctl 이 동시에 실행돼도 응답이 섞이지 않는다.
 */

/* [server] 명령 FIFO 를 만들고 읽기용으로 연다. 성공 시 fd(논블로킹), 실패 -1 */
int  cmd_fifo_open_server(void);

/* [server] 명령 FIFO 를 닫고 삭제한다. */
void cmd_fifo_close_server(int fd);

/* [server] 명령 하나를 읽는다. 읽음 1, 대기 중인 명령 없음 0, 오류 -1 */
int  cmd_recv(int fd, cmd_msg_t *msg);

/* [server] client_pid 의 응답 FIFO 로 text 를 보낸다. 성공 0, 실패 -1 */
int  reply_send(pid_t client_pid, const char *text);

/* [ctl] 명령을 보내고 응답을 out 에 받는다. 성공 0.
 * 실패 -1: errno 가 ENXIO/ENOENT 이면 서버 없음, EINTR 이면 시간 초과(호출자가 alarm 사용) */
int  ctl_request(const cmd_msg_t *msg, char *out, size_t outsz);

#endif /* IPC_H */

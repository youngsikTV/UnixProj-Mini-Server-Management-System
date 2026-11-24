/*
 * logger.h - 로그 모듈 인터페이스
 *
 * 구조:
 *   모든 프로세스는 log_write() 로 "시각/레벨/pid/메시지" 한 줄을 만들어
 *   로그 fd 에 write 한다. 이 fd 는
 *     - 1일차 단독 테스트: 로그 파일 (log_init)
 *     - 통합 후: logger 프로세스로 이어지는 pipe 의 쓰기 끝 (log_init_fd)
 *   logger 프로세스(logger_run)는 pipe 에서 읽어 로그 파일에 append 한다.
 */
#ifndef LOGGER_H
#define LOGGER_H

#include "common.h"

/* 로그 파일을 직접 열어 로그 fd 로 사용한다. 성공 0, 실패 -1 */
int  log_init(const char *path);

/* 이미 열려 있는 fd(예: pipe 쓰기 끝)를 로그 fd 로 사용한다. */
void log_init_fd(int fd);

/* 로그 fd 를 닫는다. */
void log_close(void);

/* 1 이면 로그를 stderr 에도 같이 출력한다 (시연/디버깅용). */
void log_set_echo(int on);

/* printf 형식으로 로그 한 줄을 기록한다. 스레드 안전(mutex). 시그널 핸들러 안에서는 호출 금지. */
void log_write(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* 지금까지 기록한 로그 줄 수 (mutex 로 보호되는 카운터) */
unsigned long log_count(void);

/* logger 프로세스 본체: read_fd(pipe 읽기 끝)에서 읽어 path 파일에 append.
 * 쓰기 끝이 모두 닫혀 EOF 가 되면 0 을 반환한다. */
int  logger_run(int read_fd, const char *path);

#endif /* LOGGER_H */


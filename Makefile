# 미니 서버 관리 시스템 Makefile
# (명령 줄 앞은 반드시 Tab 문자)
CC = gcc
CFLAGS = -Wall -Wextra -g -Iinclude
LIBS = -pthread

# 실행 파일별 오브젝트 구성. 모듈이 추가되면 해당 목록에 한 줄씩 늘린다.
SERVER_OBJS  = build/main.o build/logger.o build/ipc.o
MONITOR_OBJS = build/monitor.o build/ipc.o build/logger.o
CTL_OBJS     = build/ctl.o build/ipc.o build/ui.o
BACKUP_OBJS  = build/backup.o build/logger.o
HDRS         = $(wildcard include/*.h)

all: build/server build/monitor build/backup build/ctl

build/server: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_OBJS) $(LIBS)
build/monitor: $(MONITOR_OBJS)
	$(CC) $(CFLAGS) -o $@ $(MONITOR_OBJS) $(LIBS)
build/backup: $(BACKUP_OBJS)
	$(CC) $(CFLAGS) -o $@ $(BACKUP_OBJS) $(LIBS)
build/ctl: $(CTL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(CTL_OBJS) $(LIBS)
# .c -> .o 규칙 (헤더가 바뀌어도 다시 컴파일)
build/%.o: src/%.c $(HDRS) | build
	$(CC) $(CFLAGS) -c $< -o $@
build: 
	mkdir -p build
clean:
	rm -f build/*.o build/server build/monitor build/backup  build/ctl
# 통합 테스트 실행 (scripts/test.sh 가 make clean, make 를 직접 수행)
test:
	bash scripts/test.sh
.PHONY: all clean test

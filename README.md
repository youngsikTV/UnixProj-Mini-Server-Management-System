# 미니 서버 관리 시스템

UNIX 시스템 프로그래밍 기말 프로젝트. 여러 프로세스가 **pipe, FIFO, mmap 공유 메모리, 시그널**로 협력하여 서버 상태를 감시하고 파일을 자동 백업하는 C 프로그램입니다.

## 주요 기능

| 구성 요소 | 역할 |
|---|---|
| **supervisor** (`build/server`) | logger, monitor, backup을 `fork/exec`로 실행하고, 종료되면 SIGCHLD로 회수해 자동 재시작(최대 5회). ctl 명령을 FIFO로 받아 처리 |
| **logger** | supervisor의 자식 프로세스. pipe로 받은 로그를 `server.log`에 기록 |
| **monitor** (`build/monitor`) | 2초마다 CPU·메모리·load·프로세스 수를 수집해 공유 메모리에 기록. 임계값 초과 시 경고 로그 |
| **backup** (`build/backup`) | `data/`를 `backup_out/`으로 증분 백업(크기·수정시각이 바뀐 파일만 복사). 스레드 풀로 병렬 복사 |
| **ctl** (`build/ctl`) | 사용자 명령 도구. 상태 조회, 실시간 대시보드, 로그 보기, 즉시 백업, 종료 |

## 시스템 구성

```
        명령 (FIFO)
 ctl  ───────────────►  supervisor ──pipe──► logger ──► server.log
  ▲  ◄───────────────      │   │
  │      응답 (FIFO)       │   │ fork/exec
  │                        ▼   ▼
  │                   monitor  backup (스레드 풀)
  │                      │        │
  │   mmap 읽기          │        └─ 로그 pipe
  └──────  공유 메모리 ◄─┘  mmap 기록
```

- 명령·응답: FIFO (`/tmp/unixproj_cmd.fifo`, 클라이언트별 응답 FIFO)
- 시스템 상태: mmap 공유 메모리 (`/tmp/unixproj_monitor.shm`), seqlock으로 읽기/쓰기 일관성 확보
- 로그: pipe → logger → 파일
- 즉시 백업: ctl → supervisor → backup에 `SIGUSR1`

## 적용한 수업 내용

| 단원 | 사용한 기능 |
|---|---|
| 파일 입출력·파일 다루기 | `open/read/write`, `stat`, `mkdir`, 파일 복사, 나노초 단위 수정시각 비교 |
| 디렉터리 | `opendir/readdir`, 재귀 탐색 |
| 시스템·프로세스 정보 | `sysinfo`, `uname`, `getpwuid`, `/proc/meminfo`, `times` |
| 프로세스 생성과 실행 | `fork`, `exec`, `waitpid`, 자동 재시작 |
| 시그널 | `sigaction`, `sigprocmask`, `pselect`, `SIGCHLD`, `SIGUSR1`, `SIGALRM` |
| 메모리 매핑 | `mmap` 공유 메모리, 읽기 전용 매핑 |
| 파이프 | `pipe`(로그), `mkfifo`(명령/응답) |
| 스레드 | `pthread` 스레드 풀(mutex + cond), logger mutex |
| 쉘 프로그래밍 | `scripts/run.sh`, `scripts/test.sh` |

## 폴더 구조

```
.
├── Makefile
├── include/        common.h, ipc.h, logger.h, ui.h
├── src/            main.c, logger.c, ipc.c, monitor.c, backup.c, ctl.c, ui.c
├── scripts/        run.sh (실행 도우미), test.sh (통합 테스트)
├── data/           백업 원본 (샘플)
└── build/          빌드 결과 (git 제외)
```

## 빌드와 실행

환경: Linux(Ubuntu), gcc, make

```bash
git clone https://github.com/youngsikTV/UnixProj-Mini-Server-Management-System.git
cd UnixProj-Mini-Server-Management-System
make                        # build/ 아래에 server, monitor, backup, ctl 생성
chmod +x scripts/*.sh       # 실행 권한이 없을 때
```

### 실행 도우미

모든 명령은 프로젝트 최상위 폴더에서 실행합니다.

```bash
./scripts/run.sh start      # 빌드 후 서버를 백그라운드로 실행
./scripts/run.sh status     # 서버/worker 상태와 시스템 상태
./scripts/run.sh watch      # 실시간 대시보드 (q: 종료, b: 즉시 백업)
./scripts/run.sh backup     # 즉시 백업 요청
./scripts/run.sh log        # 최근 로그
./scripts/run.sh stop       # 서버와 모든 worker 종료
./scripts/run.sh clean      # 로그, pid, 백업 결과 삭제
```

### ctl 명령

| 명령 | 설명 |
|---|---|
| `ctl status` | supervisor/worker 상태 + 시스템 상태 |
| `ctl watch [-d 초] [-n 횟수]` | htop 스타일 실시간 화면 (q 종료, b 즉시 백업) |
| `ctl log [-n 줄수] [-f] [-w] [-p 파일]` | 로그 보기 (`-f` 계속 따라가기, `-w` WARN 이상만) |
| `ctl backup` | 즉시 백업 요청 |
| `ctl stop` | 서버 종료 |

### 개별 프로그램 옵션

```
monitor  [-i 주기(초)] [-m 메모리%] [-L load] [-h]
backup   [-s 원본] [-d 백업위치] [-i 주기(초)] [-t 스레드수(1~16)] [-o] [-h]
```

- backup 기본값: 원본 `data`, 대상 `backup_out`, 주기 30초, 스레드 4개
- `SIGUSR1`을 받으면 즉시 백업, `SIGTERM`/`SIGINT`는 종료

## 사용 예

```bash
./scripts/run.sh start
echo "hello" > data/b.txt       # 원본에 파일 추가
./scripts/run.sh backup         # 즉시 백업
ls backup_out                   # b.txt 가 복사됨
./scripts/run.sh log            # '복사 1' 로그 확인
./scripts/run.sh stop
```

## 테스트

```bash
make test                       # 또는 ./scripts/test.sh
```

빌드부터 supervisor 재시작, FIFO 명령, 공유 메모리, 증분 백업, 임계값 경고까지 통합 검사를 수행하고 `PASS / FAIL` 개수를 출력합니다.

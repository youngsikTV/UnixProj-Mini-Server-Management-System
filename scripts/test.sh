#!/bin/bash
#
# test.sh - 통합 테스트 (빌드 -> 실행 -> 기능 확인 -> 종료/정리 확인)
#
# 사용법: scripts/test.sh
# 결과: 항목별 PASS/FAIL 과 요약을 출력하고, 하나라도 실패하면 종료 코드 1
#
# 사용한 셸 기능: 함수, 반복문, 조건문, trap(종료 시 정리), 임시 디렉터리, 명령 치환
#

ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 1
BIN="$ROOT/build"
PASS=0
FAIL=0
SERVER_PID=""

# ---- 도우미 함수 ----------------------------------------------------------

# check "설명" 명령...   : 명령이 성공(종료 코드 0)하면 PASS
check() {
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then
        echo "  [PASS] $desc"; PASS=$((PASS + 1))
    else
        echo "  [FAIL] $desc"; FAIL=$((FAIL + 1))
    fi
}

# wait_for 초 명령...    : 명령이 성공할 때까지 0.2초 간격으로 최대 N초 기다린다
wait_for() {
    local limit=$(( $1 * 5 )); shift
    local i
    for ((i = 0; i < limit; i++)); do
        if "$@" >/dev/null 2>&1; then return 0; fi
        sleep 0.2
    done
    return 1
}

# ctl status 출력에서 worker 의 pid 를 꺼낸다.  예) worker_pid monitor -> 12345
worker_pid() {
    "$BIN/ctl" status 2>/dev/null | sed -n "s/^$1 *: pid=\([0-9]*\).*/\1/p"
}

# 종료할 때(정상/비정상/Ctrl-C 모두) 서버와 임시 파일을 정리한다
cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -INT "$SERVER_PID" 2>/dev/null
        sleep 1
    fi
    [ -n "$WORK" ] && rm -rf "$WORK"
}
trap cleanup EXIT

# ---- 1. 빌드 ---------------------------------------------------------------
echo "[1] 빌드"
cd "$ROOT" || exit 1
make clean >/dev/null
BUILD_LOG=$(make 2>&1)
check "make 성공"            test -x "$BIN/server" -a -x "$BIN/monitor" -a -x "$BIN/backup" -a -x "$BIN/ctl"
check "컴파일 경고 없음"     test -z "$(echo "$BUILD_LOG" | grep -i warning)"

# 서버가 data/, backup_out/, server.log 를 작업 디렉터리에 만들므로 임시 디렉터리에서 실행한다
WORK=$(mktemp -d)
cd "$WORK" || exit 1
mkdir -p data/sub
echo "version 1" > data/a.txt
echo "nested"    > data/sub/b.txt
ln -s a.txt data/link

# ---- 2. 서버 시작 ----------------------------------------------------------
echo "[2] 서버 시작"
"$BIN/server" -l "$WORK/server.log" >/dev/null 2>&1 &
SERVER_PID=$!
check "ctl 이 서버에 연결됨(FIFO)"          wait_for 5 "$BIN/ctl" status
check "monitor worker 실행 중"              test -n "$(worker_pid monitor)"
check "backup worker 실행 중"               test -n "$(worker_pid backup)"
check "logger 프로세스 실행 중"             sh -c "'$BIN/ctl' status | grep -q 'logger.*실행 중'"

# ---- 3. monitor / 공유 메모리 ----------------------------------------------
echo "[3] monitor (mmap 공유 메모리)"
check "시스템 상태 출력(monitor CPU)"       wait_for 6 sh -c "'$BIN/ctl' status | grep -q 'monitor CPU'"
check "공유 메모리 파일 존재"               test -e /tmp/unixproj_monitor.shm

# ---- 4. backup --------------------------------------------------------------
echo "[4] backup (증분 백업)"
check "시작 직후 첫 백업 수행"              wait_for 6 test -f backup_out/a.txt
check "하위 디렉터리와 심벌릭 링크 백업"    test -f backup_out/sub/b.txt -a -L backup_out/link
echo "version 2" > data/a.txt
echo "new file"  > data/c.txt
check "ctl backup 요청 접수"                "$BIN/ctl" backup
check "변경된 파일만 갱신됨"                wait_for 6 sh -c "grep -q 'version 2' backup_out/a.txt && test -f backup_out/c.txt"
check "원본과 백업본이 동일(diff -r)"       diff -r data backup_out
check "백업 완료 로그 존재"                 grep -q "백업 완료" "$WORK/server.log"
check "SIGUSR1 으로 즉시 백업 전달"         sh -c "kill -USR1 $SERVER_PID && sleep 1 && grep -q 'SIGUSR1 수신' '$WORK/server.log'"

# ---- 4-1. ctl 화면 ----------------------------------------------------------
echo "[4-1] ctl 화면 (status / log / watch)"
check "status 에 막대·추이 그래프 표시"     sh -c "'$BIN/ctl' status | grep -q '메모리 추이'"
check "ctl log 가 로그 파일을 찾아 출력"    sh -c "'$BIN/ctl' log -n 200 | grep -q 'supervisor 시작'"
check "ctl watch 에 프로세스 표 표시"       sh -c "'$BIN/ctl' watch -n 1 | grep -q 'RESTART'"
check "ctl watch 에 최근 로그 10줄 표시"    sh -c "'$BIN/ctl' watch -n 2 -d 0.5 | grep -q '최근 로그'"

# ---- 5. 자동 재시작 --------------------------------------------------------
echo "[5] worker 자동 재시작"
OLD_PID=$(worker_pid monitor)
kill -9 "$OLD_PID" 2>/dev/null
check "monitor 를 강제 종료하면 다시 실행됨" wait_for 6 sh -c "p=\$('$BIN/ctl' status | sed -n 's/^monitor *: pid=\\([0-9]*\\).*/\\1/p'); test -n \"\$p\" -a \"\$p\" != '$OLD_PID'"
check "재시작 로그 존재"                    grep -q "monitor 재시작" "$WORK/server.log"
check "ctl log -w 는 WARN/ERROR 만 출력"    sh -c "'$BIN/ctl' log -w -n 50 | grep -q 'WARN' && ! '$BIN/ctl' log -w -n 50 | grep -q 'INFO'"
check "좀비 프로세스 없음"                  test -z "$(ps -o stat= --ppid "$SERVER_PID" | grep Z)"

# ---- 6. 종료 ---------------------------------------------------------------
echo "[6] 종료와 정리"
check "ctl stop 명령 접수"                  "$BIN/ctl" stop
check "서버 프로세스 종료"                  wait_for 6 sh -c "! kill -0 $SERVER_PID"
check "명령 FIFO 삭제됨"                    test ! -e /tmp/unixproj_cmd.fifo
check "공유 메모리 파일 삭제됨"             test ! -e /tmp/unixproj_monitor.shm
check "남은 worker 프로세스 없음"           test -z "$(pgrep -x monitor; pgrep -x backup)"
check "서버 없을 때 ctl 은 오류 코드 반환"  sh -c "! '$BIN/ctl' status"

# ---- 7. monitor 임계값 경고 ------------------------------------------------
echo "[7] monitor 임계값 경고 (단독 실행)"
check "임계값을 넘으면 WARN 로그"           sh -c "timeout 3 '$BIN/monitor' -i 1 -m 1 -L 0.01 2>&1 | grep -q 'WARN.*임계값'"
check "monitor 종료 후 공유 메모리 삭제"    test ! -e /tmp/unixproj_monitor.shm

# ---- 요약 ------------------------------------------------------------------
echo
echo "결과: PASS $PASS / FAIL $FAIL"
[ "$FAIL" -eq 0 ]

#!/bin/bash
#
# run.sh - 미니 서버 관리 시스템 실행/종료/조회 도우미
#
# 사용법: scripts/run.sh {start|stop|restart|status|watch|backup|log [줄수]|clean}
#
# 사용한 셸 기능: 함수, case, 조건문, 명령 치환, 종료 코드 검사
#

# 스크립트가 어느 디렉터리에서 실행되어도 프로젝트 루트를 기준으로 동작하게 한다
ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 1
cd "$ROOT" || exit 1

usage() {
    cat <<EOF
사용법: $0 {start|stop|restart|status|watch|backup|log [줄수]|clean}
  start    빌드 후 서버를 백그라운드로 실행 (monitor, backup 자동 실행)
  stop     서버와 worker 종료
  restart  stop 후 start
  status   서버/시스템 상태 출력
  watch    실시간 상태 화면 (htop 스타일, q 로 종료)
  backup   즉시 백업 요청
  log      로그 마지막 줄 보기 (기본 20줄, 레벨별 색상)
  clean    빌드 결과와 로그, 백업본 삭제
EOF
}

# 서버가 응답하는지 확인한다 (ctl status 의 종료 코드로 판단)
is_running() {
    ./build/ctl status >/dev/null 2>&1
}

# 빌드: 실패하면 중단한다
build() {
    if ! make --silent; then
        echo "빌드에 실패했습니다." >&2
        exit 1
    fi
}

# 백업할 data 디렉터리가 없으면 예제 파일을 만들어 준다
prepare_data() {
    if [ ! -d data ]; then
        mkdir -p data/docs
        echo "hello unix" > data/hello.txt
        echo "sample document" > data/docs/note.txt
        echo "data/ 폴더가 없어 예제 파일을 만들었습니다."
    fi
}

start() {
    build
    prepare_data
    if is_running; then
        echo "이미 실행 중입니다."
        return 1
    fi
    # 터미널을 닫아도 계속 실행되도록 nohup 으로 띄운다. 로그는 server.log 에 쌓인다.
    nohup ./build/server >/dev/null 2>&1 &
    echo $! > server.pid
    sleep 1
    if is_running; then
        echo "서버를 시작했습니다. (pid $(cat server.pid))"
        ./build/ctl status | head -n 6
    else
        echo "서버 시작에 실패했습니다. server.log 를 확인하세요." >&2
        return 1
    fi
}

stop() {
    if ./build/ctl stop; then
        rm -f server.pid
    else
        echo "서버가 실행 중이 아닙니다."
        return 1
    fi
}

case "$1" in
    start)   start ;;
    stop)    stop ;;
    restart) stop; sleep 1; start ;;
    status)  ./build/ctl status ;;
    backup)  ./build/ctl backup ;;
    watch)   ./build/ctl watch ;;
    log)     ./build/ctl log -n "${2:-20}" ;;
    clean)
        make clean
        rm -rf server.log server.pid backup_out
        echo "정리했습니다. (data/ 폴더는 그대로 둡니다)"
        ;;
    *)       usage; exit 1 ;;
esac

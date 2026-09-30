#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""裸 TCP 终端：把 wifi-serial 的网络串口当成本地串口用（零第三方依赖）。

为什么需要这个：
    Windows 10/11 默认不装 TelnetClient，System32 下没有 telnet.exe；
    nc / ncat / PuTTY 也未必有。而 wifi-serial 的默认形态是
    「TCP 服务端 :2333」——那是一个**裸 TCP 透传**口，本来就不是给
    telnet 客户端连的（见下）。这个脚本只依赖标准库 socket，
    ESP-IDF 自带的 venv 里的 python 就能直接跑，装都不用装。

为什么不要用 telnet 客户端连 2333：
    固件里 IAC 过滤是 per-连接、且只在 NET_TELNET 模式下生效
    （netLoop: `if (tcpSlots[i].isTelnet) len = telnetFilter(...)`）。
    TCP 服务端模式下不会过滤，而真 telnet 客户端一连上就会发
    IAC DO/WILL 协商字节（0xFF 0xFD ...），这些字节会被原样写进
    USB 串口，喂给下游的被调试设备。本脚本作为客户端从不主动发
    IAC，所以连 2333 是干净的。
    要用 telnet 客户端，请把网页配置页的「网络模式」改成 Telnet，
    固件会改听 23 端口，那时 IAC 才被剔除。

用法：
    python tcp-term.py 192.168.0.239 2333
    python tcp-term.py 192.168.0.239:2333
    python tcp-term.py 192.168.0.239 2333 --no-crlf      # 裸字节，不补行尾
    python tcp-term.py 192.168.0.239 2333 --send AT+GMR --wait 2
    python tcp-term.py 192.168.0.239 2333 --log session.log

退出：Ctrl+C；或连接被对端关闭；或在输入行里打 :quit
退出码：0 正常 / 1 读写失败 / 2 连不上 / 3 参数非法（比如 host 为空或只给了端口）

（对端是 UART/USB-CDC 透传，波特率在本层无意义，故没有 --baud 参数。）

踩坑记录（为什么要在参数上这么啰嗦）：
    `tcp-term.py 2333` —— host 位置是个纯数字，几乎不可能是设备 IP，而是调用方
    把空参数丢掉后端口顶到了第一位（cmd 的 %VAR% 展开成空就会这样）。
    此时若只报 getaddrinfo failed，看起来像网络/防火墙问题，实际是参数错位，
    所以这里对「纯数字 host」单独打一行定位提示。net-term.bat 已在入口挡住这个情况。
"""
import argparse
import os
import socket
import sys
import threading
import time


def norm_stdout() -> None:
    """让 stdout/stderr 按 UTF-8 输出，遇到非法字节不炸。

    注意不要加 newline=''：那会关掉 \\n → \\r\\n 的翻译，在 cmd/PowerShell 里
    每行只下移不回车，变成阶梯状。这里已经把 \\r\\n 折成 \\n（见 CrlfFold），
    交给文本层翻译回 \\r\\n 正好。
    """
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding='utf-8', errors='replace')
        except Exception:
            pass                                  # 老 python 或被重定向：忍了


class CrlfFold:
    """把 \\r\\n 折成 \\n 再打印，避免 Windows 控制台上出现空行。

    难点是 \\r 和 \\n 可能落在两个 recv 分片里，所以要把结尾的 '\\r'
    留到下一片再决定；孤立的 '\\r'（设备用来原地刷新进度条）保持原样。
    """

    def __init__(self):
        self.pending_cr = False

    def feed(self, text: str) -> str:
        if self.pending_cr:
            text = '\r' + text
            self.pending_cr = False
        if text.endswith('\r'):
            text = text[:-1]
            self.pending_cr = True
        return text.replace('\r\n', '\n')


def stdin_pump(sock: socket.socket, crlf: bool, stop: threading.Event) -> None:
    """stdin 逐行 → socket。独立线程，因为 Windows 的 select() 不认管道。

    为什么用 os.read(0, ...) 而不是 sys.stdin.buffer.readline()：
        本线程是 daemon，进程退出时它多半还阻塞在 read 上。如果阻塞点是
        BufferedReader.readline()，它就持有那个缓冲区的锁；解释器收尾时
        会报 "Fatal Python error: _enter_buffered_busy: could not acquire
        lock for <_io.BufferedReader name='<stdin>'>" 然后**段错误**
        （实测退出码 139）。os.read 走原始 fd，不碰那把锁，收尾才干净。
    """
    def emit(raw: bytes) -> bool:
        if not raw:
            return True
        if raw.strip() in (b':quit', b':q'):
            stop.set()
            return False
        if crlf and not raw.endswith(b'\n'):
            raw = raw.rstrip(b'\r') + b'\r\n'
        try:
            sock.sendall(raw)
            return True
        except Exception as e:
            print('\n[tcp] 发送失败：%s' % e)
            stop.set()
            return False

    buf = b''
    while not stop.is_set():
        try:
            chunk = os.read(0, 512)
        except Exception:
            return
        if not chunk:                             # 管道 EOF：把手里的残余发完
            emit(buf)
            return
        buf += chunk
        while b'\n' in buf:
            line, _, buf = buf.partition(b'\n')
            if not emit(line):
                return


def main() -> int:
    ap = argparse.ArgumentParser(description='裸 TCP 终端（给 wifi-serial 的 :2333 用）')
    ap.add_argument('target', help='host 或 host:port')
    ap.add_argument('port', nargs='?', type=int, default=None, help='端口（target 里没写时用）')
    ap.add_argument('--crlf', dest='crlf', action='store_true', default=True,
                    help='发送时补行尾 \\r\\n（默认开）')
    ap.add_argument('--no-crlf', dest='crlf', action='store_false',
                    help='不补行尾，原样发送（发二进制/裸 AT 时用）')
    ap.add_argument('--send', action='append', metavar='STR',
                    help='连上后自动发一条，可重复；给完就走（配合 --wait）')
    ap.add_argument('--wait', type=float, default=None,
                    help='读多少秒后退出；不给则等到 Ctrl+C / 对端断开')
    ap.add_argument('--idle-exit', type=float, default=None,
                    help='静默多少秒无数据就退出（脚本化用）')
    ap.add_argument('--raw', action='store_true', help='输出保留原始 \\r\\n，不做折叠')
    ap.add_argument('--log', metavar='FILE', help='把收到的原始字节追加写入文件')
    args = ap.parse_args()

    host, port = args.target, args.port
    if ':' in args.target and port is None:
        host, _, p = args.target.rpartition(':')
        try:
            port = int(p)
        except ValueError:
            print('[tcp] 端口不是数字：%r' % args.target)
            return 3
    if port is None:
        port = 2333
    host = host.strip()
    norm_stdout()

    if not host:
        print('[tcp] 没给设备 IP。用法：tcp-term.py <设备IP> [端口] [选项]')
        print('[tcp] IP 见网页状态页的 net.ip / STA IP，默认端口 2333。')
        return 3

    # 典型事故：脚本被当成 "tcp-term.py 2333" 调用（前一个参数是空串，被
    # cmd/引号吃掉了），于是端口号顶到了 host 的位置 —— 报错却是
    # getaddrinfo failed，看着像网络问题，其实是参数错位。
    if host.isdigit():
        print('[tcp] 参数像端口号而不是设备 IP：host=%r' % host)
        print('[tcp] 多半是 IP 没传进来（空参数被吃掉了），用法：'
              'tcp-term.py <设备IP> [端口]')

    try:
        sock = socket.create_connection((host, port), timeout=6)
    except Exception as e:
        print('[tcp] 连不上 %s:%d —— %s' % (host, port, e))
        if host.isdigit():
            print('[tcp] ↑ host 是个纯数字，先确认上一条：这参数本来应该是设备 IP。')
        print('[tcp] 确认：设备在同一个局域网、网页状态页 net.mode 是 '
              'TCP-Server、端口一致；Windows 防火墙不拦出站（一般不拦）。')
        return 2

    sock.settimeout(0.5)
    print('[tcp] 已连接 %s:%d（%s）' % (host, port, '补 CRLF' if args.crlf else '裸字节'))
    print('[tcp] 输入一行回车即发往设备串口；Ctrl+C 或 :quit 退出')
    print('-' * 60)

    stop = threading.Event()
    # --send 是脚本化用法：终端里还等键盘输入只会白挂一个线程，跳过。
    # stdin 被重定向（管道/文件）时仍然读，`echo AT | tcp-term.py ...` 才有用。
    if not (args.send and sys.stdin.isatty()):
        t = threading.Thread(target=stdin_pump, args=(sock, args.crlf, stop), daemon=True)
        t.start()

    if args.send:
        for s in args.send:
            data = s.encode('utf-8')
            if args.crlf:
                data += b'\r\n'
            try:
                sock.sendall(data)
                print('> %s' % s)
            except Exception as e:
                print('[tcp] 发送失败：%s' % e)
                break
        if args.wait is None:
            args.wait = 2.0                       # 给 --send 一个合理默认

    fold = CrlfFold()
    logf = open(args.log, 'ab') if args.log else None
    t0 = time.time()
    last_rx = time.time()
    rc = 0
    try:
        while not stop.is_set():
            if args.wait is not None and time.time() - t0 > args.wait:
                break
            if args.idle_exit is not None and time.time() - last_rx > args.idle_exit:
                break
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            except KeyboardInterrupt:
                break
            except Exception as e:
                print('[tcp] 读失败：%s' % e)
                rc = 1
                break
            if not chunk:
                print('\n[tcp] 对端已关闭连接')
                break
            last_rx = time.time()
            if logf:
                logf.write(chunk)
                logf.flush()
            text = chunk.decode('utf-8', errors='replace')
            sys.stdout.write(text if args.raw else fold.feed(text))
            sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        if logf:
            logf.close()
        try:
            sock.close()
        except Exception:
            pass
    print('\n[tcp] 已断开 %s:%d' % (host, port))
    return rc


if __name__ == '__main__':
    try:
        rc = main()
    except KeyboardInterrupt:
        rc = 130
    # 文件/套接字已在 main 的 finally 里关干净了；stdin 那个 daemon 线程
    # 可能还阻塞在 os.read(0) 上，走 sys.exit 会去做解释器收尾并可能与它
    # 抢 stdio —— 直接 os._exit 跳过收尾（先手动 flush，别丢输出）。
    try:
        sys.stdout.flush()
        sys.stderr.flush()
    except Exception:
        pass
    os._exit(rc)

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""网页控制台用的串口监视器（不依赖 TTY）。

为什么不用 idf.py monitor / idf_monitor.py：
    它们要求 stdin 是交互终端（isatty），而服务端是用管道启动子进程的，
    必然报 "Monitor requires standard input to be attached to TTY"。
    这里自己做两件事，都不需要终端：
      1) 读方向：pyserial 直读 → stdout → 网页日志
      2) 写方向：stdin 的字节原样写进串口 → 网页「发送」输入框
    （代价：日志里的 0x... 地址不会自动解析成函数名）

为什么写方向要复用本进程、而不是另开一个连接直接写串口：
    ESP32-C3 的 USB Serial/JTAG（HWCDC）一被打开就会复位芯片，DTR/RTS
    就是它的复位/BOOT 线。另开连接 = 每次发命令都把设备重启一次，
    而且两个进程同时开同一个 COM 口在 Windows 上直接是「拒绝访问」。
    所以必须借用这里已经打开的那个句柄 → 走 stdin 管道。

用法：
    python3 serial-monitor.py --port /dev/ttyUSB0 --baud 115200 [--reset]

    stdin 给什么都原样写到串口（行尾不补也不删，由调用方决定）。
    手动在终端里跑时，输入一行回车就会发出去一行 —— 等于简易 miniterm。

退出：网页点「停止监视」→ 服务端 killTree 结束本进程；或串口打开/读取失败；
      或 stdin 管道被父进程关闭（写方向随之失效，读方向不受影响）。
"""
import argparse
import os
import sys
import threading
import time


def stdin_pump(ser) -> None:
    """把 stdin 上的字节原样、即时转发到串口（独立线程）。

    为什么用 os.read(0, ...) 而不是 sys.stdin.buffer.readline()：
        readline() 会持有 sys.stdin 那个 BufferedReader 的锁。本线程是
        daemon，主线程退出（串口读失败 return 4 / 正常 return 0）时它多半
        还阻塞在 read 上，解释器收尾拿不到那把锁就会：
            Fatal Python error: _enter_buffered_busy: could not acquire
            lock for <_io.BufferedReader name='<stdin>'> at shutdown
            → Segmentation fault，退出码 139
        服务端就会看到一个 139 而不是真实的 0/4。（本机已复现。）
        os.read 走原始 fd，不碰那把锁，收尾干净。

    为什么读到就写、不做行缓冲：
        串口是字节流，"行"是上层概念。攒到 \\n 才发会**卡住裸字节**——
        网页发 nl=0（AT+CIPSEND 提示 '>' 之后要发定长数据）时没有换行符，
        旧写法会一直 hold 到下一次写入才吐出去。收到即写，两种用法都对。
        行尾由调用方决定（网页端默认 data + "\\r\\n"），这里不补不删。
    """
    while True:
        try:
            chunk = os.read(0, 512)
        except Exception:
            return                          # 管道异常 / 句柄不可读，安静退出
        if not chunk:
            return                          # 父进程关掉了管道（进程要收尾了）
        try:
            ser.write(chunk)
            ser.flush()
        except Exception as e:
            print('[monitor] 写串口失败：%s' % e, flush=True)
            return


def main() -> int:
    ap = argparse.ArgumentParser(description='串口监视（pyserial 直读，无需 TTY）')
    ap.add_argument('-p', '--port', required=True, help='串口名，如 COM21 / /dev/ttyUSB0')
    ap.add_argument('-b', '--baud', type=int, default=115200)
    ap.add_argument('--reset', action='store_true',
                    help='先通过 DTR/RTS 复位芯片（可看到完整启动日志）')
    args = ap.parse_args()

    # 强制 UTF-8 输出：ESP 日志里可能有非法字节，用 replace 兜住，
    # 否则 Windows 默认代码页编码失败会直接中断监视。
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
        sys.stderr.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass

    try:
        import serial
    except ImportError:
        # 正常路径下走到这里说明 server.js 的解释器探测/自动安装都失败了，
        # 所以这里给的消息是"手工兜底"用的。
        print('[monitor] 当前解释器缺 pyserial：%s' % sys.executable, flush=True)
        print('[monitor] 手工修：%s -m pip install pyserial' % sys.executable, flush=True)
        print('[monitor] 若用 ESP-IDF，其自带 venv 里有，可设 ESPMON_PYTHON 指过去', flush=True)
        return 3

    try:
        ser = serial.Serial()
        ser.port = args.port
        ser.baudrate = args.baud
        ser.timeout = 0.2          # 短超时：既能及时退出，又能周期性 flush 残行
        ser.open()
        # 打开后立刻压低 DTR/RTS：避免把 ESP 的 EN/GPIO0 拖进复位或下载模式
        try:
            ser.dtr = False
            ser.rts = False
        except Exception:
            pass
    except Exception as e:
        print('[monitor] 打开 %s 失败：%s' % (args.port, e), flush=True)
        print('[monitor] 常见原因：端口被占用（其他监视/串口工具未关闭）、设备已拔出、驱动异常或无权限', flush=True)
        print('[monitor] Linux 权限提示：把用户加入 dialout 组  sudo usermod -aG dialout $USER', flush=True)
        return 2

    if args.reset:
        try:
            ser.rts = True
            ser.dtr = False
            time.sleep(0.15)
            ser.rts = False
            time.sleep(0.05)
        except Exception:
            pass

    print('[monitor] 已打开 %s @ %d（pyserial %s）；点「停止监视」结束'
          % (args.port, args.baud, getattr(serial, '__version__', '?')), flush=True)

    # 写方向：daemon 线程，主循环退出时自动结束
    threading.Thread(target=stdin_pump, args=(ser,), daemon=True).start()

    buf = b''
    try:
        while True:
            data = ser.read(4096)
            if not data:
                # 无换行的残留内容（比如不带 \n 的进度点）也及时吐出，避免看起来"卡住"
                if buf:
                    sys.stdout.write(buf.decode('utf-8', 'replace') + '\n')
                    sys.stdout.flush()
                    buf = b''
                continue
            buf += data
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                sys.stdout.write(line.decode('utf-8', 'replace').rstrip('\r') + '\n')
                sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    except Exception as e:
        print('[monitor] 读取中断：%s' % e, flush=True)
        return 4
    finally:
        try:
            ser.close()
        except Exception:
            pass
    return 0


if __name__ == '__main__':
    sys.exit(main())

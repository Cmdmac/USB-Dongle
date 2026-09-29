#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""网页控制台用的串口监视器（不依赖 TTY）。

为什么不用 idf.py monitor / idf_monitor.py：
    它们要求 stdin 是交互终端（isatty），而服务端是用管道启动子进程的，
    必然报 "Monitor requires standard input to be attached to TTY"。
    这里只做「把串口输出读出来送给网页」一件事：pyserial 直读，
    不转发键盘输入、不做地址解码，所以不需要终端。
    （代价：日志里的 0x... 地址不会自动解析成函数名）

用法：
    python3 serial-monitor.py --port /dev/ttyUSB0 --baud 115200 [--reset]

退出：网页点「停止监视」→ 服务端 killTree 结束本进程；或串口打开/读取失败。
"""
import argparse
import sys
import time


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
        print('[monitor] 缺少 pyserial。请安装：pip install pyserial', flush=True)
        print('[monitor] （若用 ESP-IDF，其自带 venv 里已包含，用那个 python 运行即可）', flush=True)
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

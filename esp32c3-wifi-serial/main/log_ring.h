/*
 * log_ring.h - 把 esp_log 输出同时缓存到内存环形区, 供网页"日志"页查看
 * (dongle 通常没有引出 UART0, 这是唯一的日志出口)
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

void   log_ring_init(void);
/* 把环形区内容拷到 out(以 '\0' 结尾), 返回实际长度 */
size_t log_ring_dump(char *out, size_t max);
void   log_ring_clear(void);

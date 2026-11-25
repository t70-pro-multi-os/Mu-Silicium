#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# All automatic writes remain in PID1's tmpfs. No disk or radio setup.
export PATH=/bin:/sbin HOME=/ LANG=C.UTF-8 LC_ALL=C.UTF-8
export PS1='t70pro# '
printf '\033[2J\033[H%s\n\n' 'T70Pro Linux 진단 콘솔'
printf '%s\n' '부팅 로그: dmesg' \
  '장치 시작 상태: cat /run/teclast-startup/state' \
  '장치 시작 로그: cat /run/teclast-startup.log' \
  '콘솔 로그: cat /run/teclast-console.log' \
  '이전 화면: Shift + PageUp / PageDown' ''
exec /bin/sh -i

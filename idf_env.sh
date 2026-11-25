#!/bin/bash
# ESP-IDF v6.0.1 环境
# IDF 源码放在 ~/Coding/esp-idf（不要放 ~/Documents：本机 agent 进程被 macOS TCC
# 拒绝访问 Documents/Desktop，读该目录一律 EPERM，会导致 kconfgen 等子进程卡死）
export IDF_PATH="$HOME/Coding/esp-idf"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf6.0_py3.11_env"
export ESP_IDF_VERSION="6.0.1"          # 不带 v 前缀，否则 Version.coerce 报错
export VENV_PY="$IDF_PYTHON_ENV_PATH/bin/python"
export LC_ALL=en_US.UTF-8
export LANG=en_US.UTF-8
export PYTHONPATH="$IDF_PATH/tools"
export PATH="$IDF_PYTHON_ENV_PATH/bin:$IDF_PATH/tools:$PATH"

# 绕开本机 git submodule 检查会挂死的问题（IDF 官方开关）
export IDF_SKIP_CHECK_SUBMODULES=1
# 本工程自带 components/mdns（esp-protocols 1.15.1），不连组件注册表
export IDF_COMPONENT_MANAGER=0

export IDF_TOOLS_PATH="$HOME/.espressif"
for d in "$IDF_TOOLS_PATH"/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin; do
  [ -d "$d" ] && export PATH="$d:$PATH"
done
for d in "$IDF_TOOLS_PATH"/tools/esp-clang/*/esp-clang/bin; do
  [ -d "$d" ] && export PATH="$d:$PATH"
done
for d in "$IDF_TOOLS_PATH"/tools/esp-rom-elfs/*; do
  [ -d "$d" ] && export ESP_ROM_ELF_DIR="$d"
done

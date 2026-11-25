#!/bin/bash
# ESP-IDF v5.3.2 环境（仅用于 WiFi 关联 A/B 实验，主工程仍用 6.0.1）
export IDF_PATH="$HOME/Coding/esp-idf-5.3.2"
export IDF_TOOLS_PATH="$HOME/.espressif"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.3_py3.11_env"
export ESP_IDF_VERSION="5.3.2"
export VENV_PY="$IDF_PYTHON_ENV_PATH/bin/python"
export LC_ALL=en_US.UTF-8
export LANG=en_US.UTF-8
export PYTHONPATH="$IDF_PATH/tools"
export PATH="$IDF_PYTHON_ENV_PATH/bin:$IDF_PATH/tools:$PATH"
export IDF_SKIP_CHECK_SUBMODULES=1
export IDF_COMPONENT_MANAGER=0

for d in "$IDF_TOOLS_PATH"/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin; do
  [ -d "$d" ] && export PATH="$d:$PATH"
done
for d in "$IDF_TOOLS_PATH"/tools/esp-rom-elfs/*; do
  [ -d "$d" ] && export ESP_ROM_ELF_DIR="$d"
done

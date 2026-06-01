#!/bin/bash

cd "$(dirname "$0")"
sudo chmod 777 -R .


# 定义源文件夹路径
SOURCE_DIR="."

# 定义 hosts 文件路径
HOSTS_FILE="hosts"

# 定义远程目标文件夹路径
REMOTE_DIR="$(pwd)"

# 检查 hosts 文件是否存在
if [[ ! -f "$HOSTS_FILE" ]]; then
    echo "Error: hosts file not found!"
    exit 1
fi

# 遍历 hosts 文件中的每个 IP 地址
while read -r ip || [ -n "$ip" ]; do

    echo "Copying to host: $ip..."

    # 使用 rsync 复制文件夹（排除仅编译需要的内容，减少传输；运行只需 project/third_party 下 jerasure、gf-complete 的 lib）
    sudo rsync -avz \
        --exclude='project/cmake/build/CMakeFiles' \
        --exclude='project/cmake/build/run_client' \
        --exclude='project/cmake/build/main_test' \
        --exclude='project/cmake/build/main_client' \
        --exclude='project/cmake/build/layout_test' \
        --exclude='project/cmake/build/recovery_plan_test' \
        --exclude='storage/*' \
        --exclude='/third_party/' \
        --exclude='project/third_party/asio/' \
        --exclude='project/third_party/grpc/' \
        --exclude='.git/*' \
        -e "ssh -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=/root/.ssh/known_hosts" \
        "$SOURCE_DIR/" "$ip:$REMOTE_DIR/"
    # 检查 rsync 是否成功
    if [ $? -eq 0 ]; then
        echo "Successfully copied to $ip!"
    else
        echo "Failed to copy to $ip!"
    fi

done < "$HOSTS_FILE"


echo "All done!"
#!/usr/bin/env bash
set -euo pipefail

# 创建 rmdb.zip（在仓库根目录）
# 运行：bash zip.sh 或 chmod +x zip.sh && ./zip.sh

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$ROOT_DIR/db2024/rmdb"
DEST_ZIP="$ROOT_DIR/rmdb.zip"

if [ ! -d "$SRC_DIR" ]; then
  echo "源目录不存在: $SRC_DIR" >&2
  exit 1
fi

cd "$SRC_DIR"

zip -r "$DEST_ZIP" . \
  -x '.DS_Store' \
  -x '.cache/*' \
  -x 'build/*' \
  -x '*.o' \
  -x '*.a' \
  -x '*.so'

echo "Created $DEST_ZIP"

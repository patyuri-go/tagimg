#!/usr/bin/env bash
# 创建 tagimg 图片库并导入初始表结构。
#
# 用法：
#   scripts/create_database.sh [--config PATH] [--force-recreate]
#
# 默认读取仓库根目录的 config.json 中 database 段，使用 image_dbname 作为库名。
# 已存在同名库时默认拒绝执行；确认可以删除时才加 --force-recreate。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
CONFIG_FILE="$REPO_ROOT/config.json"
SCHEMA_FILE="$REPO_ROOT/sql/001_schema.sql"
FORCE_RECREATE=0

usage() {
  cat <<'EOF'
用法: scripts/create_database.sh [--config PATH] [--force-recreate]

  --config PATH       指定配置文件，默认仓库根目录 config.json
  --force-recreate    目标库已存在时先删除再重建（会丢失全部数据）
  -h, --help          显示本帮助
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --config)
      [[ $# -ge 2 ]] || { echo "错误: --config 缺少参数" >&2; exit 2; }
      CONFIG_FILE="$2"
      shift 2
      ;;
    --force-recreate)
      FORCE_RECREATE=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "错误: 未知参数 $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

[[ -f "$CONFIG_FILE" ]] || { echo "错误: 配置文件不存在: $CONFIG_FILE" >&2; exit 2; }
[[ -f "$SCHEMA_FILE" ]] || { echo "错误: 建表脚本不存在: $SCHEMA_FILE" >&2; exit 2; }

# 从 JSON 读取数据库连接信息，密码通过环境变量传给 psql，不出现在命令行。
read_config() {
  python3 - "$CONFIG_FILE" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    database = json.load(handle)["database"]

for key in ("host", "port", "user", "password", "image_dbname"):
    value = database.get(key)
    if value is None:
        raise SystemExit(f"配置缺少 database.{key}")
    sys.stdout.write(str(value) + "\0")
PY
}

mapfile -d '' -t CONFIG_VALUES < <(read_config)
if [[ ${#CONFIG_VALUES[@]} -ne 5 ]]; then
  echo "错误: 无法从配置读取数据库连接信息" >&2
  exit 2
fi

DB_HOST="${CONFIG_VALUES[0]}"
DB_PORT="${CONFIG_VALUES[1]}"
DB_USER="${CONFIG_VALUES[2]}"
DB_PASSWORD="${CONFIG_VALUES[3]}"
DB_NAME="${CONFIG_VALUES[4]}"

# 库名会拼进 SQL 标识符，先做严格校验，避免配置里出现引号或分号。
[[ "$DB_NAME" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || {
  echo "错误: 库名不合法: $DB_NAME" >&2
  exit 2
}

export PGPASSWORD="$DB_PASSWORD"

PSQL_ADMIN=(-h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -X -v ON_ERROR_STOP=1 -tA)
PSQL_TARGET=(-h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -X -v ON_ERROR_STOP=1 -d "$DB_NAME")

echo "目标服务器: $DB_HOST:$DB_PORT，库名: $DB_NAME"

EXISTS="$(psql "${PSQL_ADMIN[@]}" -c "SELECT 1 FROM pg_database WHERE datname = '$DB_NAME'")"
if [[ "$EXISTS" == "1" ]]; then
  if [[ $FORCE_RECREATE -ne 1 ]]; then
    echo "错误: 数据库 $DB_NAME 已存在。确认可以删除后加 --force-recreate" >&2
    exit 3
  fi
  echo "数据库 $DB_NAME 已存在，按 --force-recreate 删除重建"
  psql "${PSQL_ADMIN[@]}" -c "DROP DATABASE \"$DB_NAME\" WITH (FORCE)"
fi

psql "${PSQL_ADMIN[@]}" -c "CREATE DATABASE \"$DB_NAME\""

echo "导入表结构: $SCHEMA_FILE"
psql "${PSQL_TARGET[@]}" -f "$SCHEMA_FILE"

echo "建库完成，表清单："
psql "${PSQL_TARGET[@]}" -c "
  SELECT table_name
  FROM information_schema.tables
  WHERE table_schema = 'public'
  ORDER BY table_name
"

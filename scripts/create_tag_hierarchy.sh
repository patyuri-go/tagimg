#!/usr/bin/env bash
# 在图片库里创建 wd14scan 标签层级。
#
# 用法：
#   scripts/create_tag_hierarchy.sh [--config PATH]
#
# 默认读取仓库根目录的 config.json 中 database 段，使用 image_dbname 作为库名。
# 脚本可重复执行：已存在的 wd14scan 根标签与 general / character / rating
# 二级分类不会重复创建。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
CONFIG_FILE="$REPO_ROOT/config.json"
SQL_FILE="$REPO_ROOT/sql/002_tag_hierarchy.sql"

usage() {
  cat <<'EOF'
用法: scripts/create_tag_hierarchy.sh [--config PATH]

  --config PATH  指定配置文件，默认仓库根目录 config.json
  -h, --help     显示本帮助
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --config)
      [[ $# -ge 2 ]] || { echo "错误: --config 缺少参数" >&2; exit 2; }
      CONFIG_FILE="$2"
      shift 2
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
[[ -f "$SQL_FILE" ]] || { echo "错误: SQL 脚本不存在: $SQL_FILE" >&2; exit 2; }

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

[[ "$DB_NAME" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || {
  echo "错误: 库名不合法: $DB_NAME" >&2
  exit 2
}

export PGPASSWORD="$DB_PASSWORD"

PSQL_TARGET=(-h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -X -v ON_ERROR_STOP=1 -d "$DB_NAME")

echo "目标服务器: $DB_HOST:$DB_PORT，库名: $DB_NAME"
psql "${PSQL_TARGET[@]}" -f "$SQL_FILE"

echo "当前标签层级："
psql "${PSQL_TARGET[@]}" -c "
  SELECT root.id AS root_id, root_name.name AS root, child_name.name AS category
  FROM tags root
  JOIN tag_name root_name ON root_name.tag_id = root.id
  LEFT JOIN tags child ON child.parent_id = root.id
  LEFT JOIN tag_name child_name ON child_name.tag_id = child.id
  WHERE root_name.type = 'standard' AND root_name.name = 'wd14scan'
  ORDER BY child_name.name
"

-- Chinese-CLIP ViT-L/14 图像向量表
-- 目标：PostgreSQL 13+，需要 pgvector 扩展（tagimg 库已启用）
-- 用法：psql -v ON_ERROR_STOP=1 -d tagimg -f sql/003_clip_embedding.sql
--
-- 说明 / 假设：
--   1. 本文件可重复执行：表与索引都用 IF NOT EXISTS，扩展用 IF NOT EXISTS 兜底。
--   2. 表名使用下划线，不需要双引号包裹。
--   3. 一张图片最多一条向量（主键即 image_id），图片删除时级联删除。
--   4. embedding 为 768 维 float32，已做 L2 归一化；维度由 vector(768) 强制，
--      不再额外加 CHECK 约束。
--   5. 相似度用余弦距离运算符 <=>，索引为 HNSW + vector_cosine_ops。

BEGIN;

CREATE EXTENSION IF NOT EXISTS vector;

CREATE TABLE IF NOT EXISTS chinese_clip_vit_large_patch14 (
    image_id    UUID PRIMARY KEY REFERENCES images (id) ON DELETE CASCADE,
    embedding   vector(768) NOT NULL,
    model_name  TEXT NOT NULL DEFAULT 'chinese-clip-vit-large-patch14',
    model_arch  TEXT NOT NULL DEFAULT 'ViT-L-14',
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

COMMENT ON TABLE chinese_clip_vit_large_patch14 IS 'Chinese-CLIP ViT-L/14 图像向量；与 images 一对一';
COMMENT ON COLUMN chinese_clip_vit_large_patch14.image_id IS '图片 ID，删除图片时级联删除向量';
COMMENT ON COLUMN chinese_clip_vit_large_patch14.embedding IS '768 维 float32，已 L2 归一化（点积即余弦相似度）';
COMMENT ON COLUMN chinese_clip_vit_large_patch14.model_name IS '模型名称，便于后续换模型时区分';
COMMENT ON COLUMN chinese_clip_vit_large_patch14.model_arch IS '模型结构标识，例如 ViT-L-14';

-- 余弦距离索引；表为空时建索引成本可忽略。
CREATE INDEX IF NOT EXISTS chinese_clip_vit_large_patch14_embedding_idx
    ON chinese_clip_vit_large_patch14
    USING hnsw (embedding vector_cosine_ops)
    WITH (m = 16, ef_construction = 64);

COMMIT;

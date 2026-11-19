-- image_tags：图片级标签。
--
-- 作品标签已弃用：作品的标签不再单独存储，而是由该作品下所有图片的
-- image_tags 并集派生。artwork_tags 表暂时保留，仅用于回滚，不再读写。
--
-- 历史数据的回填 SQL 不放在仓库里，迁移时单独执行。
CREATE TABLE IF NOT EXISTS image_tags (
    image_id   UUID NOT NULL REFERENCES images (id) ON DELETE CASCADE,
    tag_id     UUID NOT NULL REFERENCES tags (id) ON DELETE CASCADE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),

    PRIMARY KEY (image_id, tag_id)
);

CREATE INDEX IF NOT EXISTS idx_image_tags_tag_id ON image_tags (tag_id);

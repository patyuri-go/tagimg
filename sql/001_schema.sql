-- tagimg 初始数据库结构
-- 目标：PostgreSQL 13+（UUID 默认值使用内置 gen_random_uuid()，无需 pgcrypto 扩展）
-- 用法：psql -v ON_ERROR_STOP=1 -d <数据库名> -f sql/001_schema.sql
--
-- 说明 / 假设：
--   1. 本文件面向空库执行；重复执行会因对象已存在而报错。
--   2. 除主键、外键、标签名等结构性字段外，未强制 NOT NULL；
--      title、description、artist.name、图片尺寸等允许为空。
--   3. images.entity_id 是外部实体库 ID，本库没有 entities 表，因此不建外键；
--      允许为 NULL（尚未映射时），非 NULL 值唯一。
--   4. images.width / height / frame_count 允许为空：C++ 侧暂不解码图片，后续回填。
--   5. images.scan_at 记录上次模型扫描时间，为空表示从未扫描。
--   6. 所有 created_at / updated_at 默认 now()；updated_at 由应用在更新时维护。
--   7. tag_name.type 的枚举值使用英文 standard / alias，分别对应“标准名称 / 别名”。

BEGIN;

-- ---------------------------------------------------------------------------
-- 枚举类型
-- ---------------------------------------------------------------------------

CREATE TYPE tag_name_type AS ENUM ('standard', 'alias');

COMMENT ON TYPE tag_name_type IS '标签名称类型：standard=标准名称，alias=别名';

-- ---------------------------------------------------------------------------
-- 作品
-- ---------------------------------------------------------------------------

CREATE TABLE artworks (
    id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    title        TEXT,
    description  TEXT,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    published_at TIMESTAMPTZ,
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);

COMMENT ON TABLE artworks IS '作品表';
COMMENT ON COLUMN artworks.id IS '作品 ID';
COMMENT ON COLUMN artworks.title IS '作品标题，可为空';
COMMENT ON COLUMN artworks.description IS '作品描述，可为空';
COMMENT ON COLUMN artworks.created_at IS '作品创建时间（入库时间）';
COMMENT ON COLUMN artworks.published_at IS '原始发布时间，可为空';
COMMENT ON COLUMN artworks.updated_at IS '数据更新时间，由应用维护';

-- ---------------------------------------------------------------------------
-- 图片
-- ---------------------------------------------------------------------------

CREATE TABLE images (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    entity_id   UUID UNIQUE,
    width       INTEGER,
    height      INTEGER,
    frame_count INTEGER,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    scan_at     TIMESTAMPTZ,

    CONSTRAINT images_width_check CHECK (width IS NULL OR width > 0),
    CONSTRAINT images_height_check CHECK (height IS NULL OR height > 0),
    CONSTRAINT images_frame_count_check CHECK (frame_count IS NULL OR frame_count > 0)
);

COMMENT ON TABLE images IS '图片表';
COMMENT ON COLUMN images.id IS '图片 ID';
COMMENT ON COLUMN images.entity_id IS '外部实体库 ID；本库不建外键，非 NULL 值唯一';
COMMENT ON COLUMN images.width IS '图片宽度（像素），可空，待后续回填';
COMMENT ON COLUMN images.height IS '图片高度（像素），可空，待后续回填';
COMMENT ON COLUMN images.frame_count IS '动画帧数；静态图为 1，未知时为空';
COMMENT ON COLUMN images.created_at IS '入库时间';
COMMENT ON COLUMN images.scan_at IS '上次模型扫描时间；为空表示从未扫描';

-- ---------------------------------------------------------------------------
-- 艺术家
-- ---------------------------------------------------------------------------

CREATE TABLE artists (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name        VARCHAR,
    sort_name   VARCHAR,
    description TEXT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

COMMENT ON TABLE artists IS '艺术家表';
COMMENT ON COLUMN artists.id IS '艺术家 ID';
COMMENT ON COLUMN artists.name IS '艺术家昵称，可为空';
COMMENT ON COLUMN artists.sort_name IS '排序名称，可为空';
COMMENT ON COLUMN artists.description IS '艺术家介绍，可为空';
COMMENT ON COLUMN artists.created_at IS '创建时间';

-- ---------------------------------------------------------------------------
-- 标签与标签名称
-- ---------------------------------------------------------------------------

CREATE TABLE tags (
    id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    parent_id UUID REFERENCES tags (id) ON DELETE SET NULL,

    CONSTRAINT tags_parent_not_self CHECK (parent_id IS NULL OR parent_id <> id)
);

COMMENT ON TABLE tags IS '标签表';
COMMENT ON COLUMN tags.id IS '标签 ID';
COMMENT ON COLUMN tags.parent_id IS '父标签 ID；删除父标签时子标签改为根标签';

CREATE TABLE tag_name (
    id     UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    tag_id UUID NOT NULL REFERENCES tags (id) ON DELETE CASCADE,
    type   tag_name_type NOT NULL,
    name   TEXT NOT NULL,

    CONSTRAINT tag_name_tag_id_name_unique UNIQUE (tag_id, name)
);

COMMENT ON TABLE tag_name IS '标签名称表：一个标签可以有标准名称和多个别名';
COMMENT ON COLUMN tag_name.id IS '别名/名称记录 ID';
COMMENT ON COLUMN tag_name.tag_id IS '所属标签 ID';
COMMENT ON COLUMN tag_name.type IS '类型：standard=标准名称，alias=别名';
COMMENT ON COLUMN tag_name.name IS '名称内容';

-- ---------------------------------------------------------------------------
-- 作品与图片
-- ---------------------------------------------------------------------------

CREATE TABLE artwork_images (
    artwork_id UUID NOT NULL REFERENCES artworks (id) ON DELETE CASCADE,
    image_id   UUID NOT NULL REFERENCES images (id) ON DELETE CASCADE,
    position   INTEGER,

    PRIMARY KEY (artwork_id, image_id),
    CONSTRAINT artwork_images_position_check CHECK (position IS NULL OR position >= 0)
);

COMMENT ON TABLE artwork_images IS '作品图片关系表';
COMMENT ON COLUMN artwork_images.artwork_id IS '作品 ID';
COMMENT ON COLUMN artwork_images.image_id IS '图片 ID';
COMMENT ON COLUMN artwork_images.position IS '作品内图片顺序；为空表示未指定，排序时建议 NULLS LAST';

-- ---------------------------------------------------------------------------
-- 作品与艺术家
-- ---------------------------------------------------------------------------

CREATE TABLE artwork_artists (
    artwork_id UUID NOT NULL REFERENCES artworks (id) ON DELETE CASCADE,
    artist_id  UUID NOT NULL REFERENCES artists (id) ON DELETE CASCADE,

    PRIMARY KEY (artwork_id, artist_id)
);

COMMENT ON TABLE artwork_artists IS '作品艺术家关系表';
COMMENT ON COLUMN artwork_artists.artwork_id IS '作品 ID';
COMMENT ON COLUMN artwork_artists.artist_id IS '艺术家 ID';

-- ---------------------------------------------------------------------------
-- 作品来源
-- ---------------------------------------------------------------------------

CREATE TABLE artwork_sources (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    artwork_id  UUID NOT NULL REFERENCES artworks (id) ON DELETE CASCADE,
    source_name TEXT NOT NULL,
    source_url  TEXT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

COMMENT ON TABLE artwork_sources IS '作品来源记录表';
COMMENT ON COLUMN artwork_sources.id IS '来源记录 ID';
COMMENT ON COLUMN artwork_sources.artwork_id IS '所属作品 ID';
COMMENT ON COLUMN artwork_sources.source_name IS '来源名称';
COMMENT ON COLUMN artwork_sources.source_url IS '原始链接，可为空';
COMMENT ON COLUMN artwork_sources.created_at IS '收录时间';

-- ---------------------------------------------------------------------------
-- 作品与标签
-- ---------------------------------------------------------------------------

CREATE TABLE artwork_tags (
    artwork_id UUID NOT NULL REFERENCES artworks (id) ON DELETE CASCADE,
    tag_id     UUID NOT NULL REFERENCES tags (id) ON DELETE CASCADE,

    PRIMARY KEY (artwork_id, tag_id)
);

COMMENT ON TABLE artwork_tags IS '作品标签关系表';
COMMENT ON COLUMN artwork_tags.artwork_id IS '作品 ID';
COMMENT ON COLUMN artwork_tags.tag_id IS '标签 ID';

-- ---------------------------------------------------------------------------
-- 外键 / 查询索引
-- ---------------------------------------------------------------------------

CREATE INDEX idx_tags_parent_id ON tags (parent_id);

CREATE INDEX idx_tag_name_tag_id ON tag_name (tag_id);
CREATE INDEX idx_tag_name_name ON tag_name (name);

CREATE INDEX idx_artwork_images_image_id ON artwork_images (image_id);
CREATE INDEX idx_artwork_images_artwork_position ON artwork_images (artwork_id, position);

CREATE INDEX idx_artwork_artists_artist_id ON artwork_artists (artist_id);

CREATE INDEX idx_artwork_sources_artwork_id ON artwork_sources (artwork_id);

CREATE INDEX idx_artwork_tags_tag_id ON artwork_tags (tag_id);

CREATE INDEX idx_artworks_published_at ON artworks (published_at DESC NULLS LAST);
CREATE INDEX idx_artworks_created_at ON artworks (created_at DESC);

COMMIT;

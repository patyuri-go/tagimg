-- wd14scan 标签层级
--
-- 结构：wd14scan（根）→ general / character / rating（二级分类）→ 具体标签
-- 脚本可重复执行：已存在的层级不会重复创建。
--
-- 用法：psql -v ON_ERROR_STOP=1 -d <图片库名> -f sql/002_tag_hierarchy.sql

BEGIN;

DO $$
DECLARE
    root_id       UUID;
    category_id   UUID;
    category_name TEXT;
BEGIN
    SELECT t.id INTO root_id
    FROM tags t
    JOIN tag_name n ON n.tag_id = t.id
    WHERE n.type = 'standard'
      AND n.name = 'wd14scan'
      AND t.parent_id IS NULL
    LIMIT 1;

    IF root_id IS NULL THEN
        INSERT INTO tags (parent_id) VALUES (NULL) RETURNING id INTO root_id;
        INSERT INTO tag_name (tag_id, type, name)
        VALUES (root_id, 'standard', 'wd14scan');
    END IF;

    FOREACH category_name IN ARRAY ARRAY['general', 'character', 'rating']
    LOOP
        SELECT t.id INTO category_id
        FROM tags t
        JOIN tag_name n ON n.tag_id = t.id
        WHERE n.type = 'standard'
          AND n.name = category_name
          AND t.parent_id = root_id
        LIMIT 1;

        IF category_id IS NULL THEN
            INSERT INTO tags (parent_id) VALUES (root_id)
            RETURNING id INTO category_id;
            INSERT INTO tag_name (tag_id, type, name)
            VALUES (category_id, 'standard', category_name);
        END IF;
    END LOOP;
END $$;

COMMIT;

-- ===================================================
--  url-shortener MySQL 初始化脚本
--  说明：服务启动时会自动建库建表，此脚本用于手动初始化
--       或在需要重置数据库时使用
--  执行：mysql -u root -p < init.sql
-- ===================================================

-- 创建数据库（若已存在则不报错）
CREATE DATABASE IF NOT EXISTS `url_shortener`
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

-- 切换到目标数据库
USE `url_shortener`;

-- 创建短链映射表
CREATE TABLE IF NOT EXISTS `url_mappings` (
    `id`           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '自增主键',
    `code`         VARCHAR(16)     NOT NULL                COMMENT '短码（Base62 编码）',
    `original_url` TEXT            NOT NULL                COMMENT '原始长地址',
    `created_at`   TIMESTAMP       NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '创建时间',

    PRIMARY KEY (`id`),
    -- 短码唯一索引（防止重复写入同一短码）
    UNIQUE KEY `uk_code` (`code`),
    -- 按创建时间倒序查询的索引
    INDEX `idx_created` (`created_at`)

) ENGINE=InnoDB
  DEFAULT CHARSET=utf8mb4
  COLLATE=utf8mb4_unicode_ci
  COMMENT='短链与长链映射表';

-- ===================================================
--  （可选）创建专用数据库用户，避免使用 root 账户
-- ===================================================

-- 创建用户（请将 'your_password' 替换为实际密码）
-- CREATE USER IF NOT EXISTS 'urlshort'@'localhost' IDENTIFIED BY 'your_password';

-- 授予必要权限
-- GRANT SELECT, INSERT, DELETE ON `url_shortener`.* TO 'urlshort'@'localhost';

-- 刷新权限
-- FLUSH PRIVILEGES;

-- ===================================================
--  验证：查询当前表结构
-- ===================================================

-- SHOW CREATE TABLE `url_mappings`\G

-- ===================================================
--  （可选）插入测试数据
-- ===================================================

-- INSERT INTO `url_mappings` (`code`, `original_url`) VALUES
--     ('abc123', 'https://www.example.com/very/long/path/to/some/resource'),
--     ('def456', 'https://github.com/some-user/some-repo');

SELECT CONCAT('初始化完成，数据库: url_shortener，表: url_mappings') AS 状态;

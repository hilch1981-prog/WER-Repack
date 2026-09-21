-- WOW Legends: hardcore status per character (lives in the characters DB).
CREATE TABLE IF NOT EXISTS `wowlegends_hardcore` (
  `guid`        INT UNSIGNED   NOT NULL,
  `enabled`     TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 = hardcore character',
  `dead`        TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 = fallen (locked)',
  `death_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `death_cause` VARCHAR(100)   NOT NULL DEFAULT '',
  `death_time`  INT UNSIGNED   NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

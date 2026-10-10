-- Use only against the captured original rows; review any later changes first.
UPDATE `wl_world`.`item_template`
SET `flagsCustom` = 1
WHERE `entry` IN (45280, 46104)
  AND `duration` = 0
  AND `flagsCustom` = 0;

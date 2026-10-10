-- Match the core's removal of an invalid real-time duration flag on these two items.
UPDATE `wl_world`.`item_template`
SET `flagsCustom` = (`flagsCustom` & 4294967294)
WHERE `entry` IN (45280, 46104)
  AND `duration` = 0
  AND `flagsCustom` = 1;

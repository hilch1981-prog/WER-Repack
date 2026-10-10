-- Exact missing-path spawn only. Idle fallback, not patrol reconstruction.
UPDATE `wl_world`.`creature`
SET `MovementType` = 0
WHERE `guid` = 82897 AND `id` = 16320 AND `MovementType` = 2
  AND NOT EXISTS (
    SELECT 1 FROM `wl_world`.`creature_addon` AS a
    WHERE a.`guid` = 82897 AND a.`path_id` <> 0
  )
  AND NOT EXISTS (
    SELECT 1 FROM `wl_world`.`creature_template_addon` AS a
    WHERE a.`entry` = 16320 AND a.`path_id` <> 0
  )
  AND NOT EXISTS (
    SELECT 1 FROM `wl_world`.`waypoint_data` AS w WHERE w.`id` = 828970
  );

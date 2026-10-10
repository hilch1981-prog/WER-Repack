-- Restores the captured original MovementType, including its missing-path problem.
UPDATE `wl_world`.`creature`
SET `MovementType` = 2
WHERE `guid` = 82897 AND `id` = 16320 AND `MovementType` = 0;

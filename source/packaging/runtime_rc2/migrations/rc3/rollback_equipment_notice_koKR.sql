-- Set the ID from the migration receipt. NULL intentionally deletes nothing.
SET @wer_equip_notice_inserted_id = NULL;
DELETE FROM `wl_playerbots`.`ai_playerbot_texts`
WHERE @wer_equip_notice_inserted_id IS NOT NULL
  AND `id` = @wer_equip_notice_inserted_id
  AND `name` = 'wl_equip_item_notice'
  AND `text` = '%item_link 장비를 장착합니다.'
  AND `text_loc1` = '%item_link 장비를 장착합니다.'
  AND `say_type` = 0 AND `reply_type` = 0
  AND `text_loc2` = '' AND `text_loc3` = '' AND `text_loc4` = ''
  AND `text_loc5` = '' AND `text_loc6` = '' AND `text_loc7` = '' AND `text_loc8` = '';

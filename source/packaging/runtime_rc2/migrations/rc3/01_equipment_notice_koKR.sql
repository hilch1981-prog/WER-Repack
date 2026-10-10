-- Keep existing/custom rows unchanged. text_loc1 is LOCALE_koKR.
INSERT INTO `wl_playerbots`.`ai_playerbot_texts`
  (`name`, `text`, `say_type`, `reply_type`, `text_loc1`)
SELECT 'wl_equip_item_notice', '%item_link 장비를 장착합니다.', 0, 0,
       '%item_link 장비를 장착합니다.'
WHERE NOT EXISTS (
  SELECT 1 FROM `wl_playerbots`.`ai_playerbot_texts`
  WHERE `name` = 'wl_equip_item_notice'
);

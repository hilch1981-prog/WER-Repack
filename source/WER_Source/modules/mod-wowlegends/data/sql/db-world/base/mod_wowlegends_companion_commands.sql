-- Register the WOW Legends personal companion commands (player-level) so they
-- show in `.help`, the docs and the website command page.
DELETE FROM `command` WHERE `name` IN
    ('companion','companion create','companion summon','companion dismiss','companion forget');

-- The bare `companion` row is kept on purpose: with no arguments it shows your
-- companion's status (unlike hardcore, whose bare parent did nothing).
INSERT INTO `command` (`name`,`security`,`help`) VALUES
('companion',         0, 'Syntax: .companion\nShow your personal companion (name, race, class), or how to create one if you have none yet.'),
('companion create',  0, 'Syntax: .companion create <race> <class> <name>\nClaim your ONE permanent battle companion - a bot that fights at your side and chats/remembers you. Race must match your faction. Races: human orc dwarf nightelf undead tauren gnome troll bloodelf draenei. Classes: warrior paladin hunter rogue priest dk shaman mage warlock druid. Name: 2-12 letters, must be unused.'),
('companion summon',  0, 'Syntax: .companion summon\nCall your companion to your side (auto-joins your group and fights with you).'),
('companion dismiss', 0, 'Syntax: .companion dismiss\nSend your companion away. Recall it any time with .companion summon.'),
('companion forget',  0, 'Syntax: .companion forget\nRelease your companion permanently: the bond and its memory of you are erased, and the pool character is freed for others. You may then create a new companion.');

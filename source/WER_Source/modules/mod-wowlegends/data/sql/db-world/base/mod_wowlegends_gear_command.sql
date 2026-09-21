-- Register the WOW Legends custom .gear sub-commands in the world command table
-- so they appear in `.help`, the docs, and the website command page.
DELETE FROM `command` WHERE `name` IN
    ('gear level','gear rare','gear epic','gear max','gear undress');

INSERT INTO `command` (`name`,`security`,`help`) VALUES
('gear level',   2, 'Syntax: .gear level\nGear the selected player (or yourself) with level-appropriate UNCOMMON (green) items. Spec-aware, enchanted and gemmed.'),
('gear rare',    2, 'Syntax: .gear rare\nGear the selected player (or yourself) with RARE (blue) items for their current level.'),
('gear epic',    2, 'Syntax: .gear epic\nGear the selected player (or yourself) with EPIC (purple) items for their current level.'),
('gear max',     2, 'Syntax: .gear max\nGear the selected player (or yourself) with the best available items for their current level.'),
('gear undress', 2, 'Syntax: .gear undress\nMove all equipped items from the selected player (or yourself) into their bags.');

-- Register the WOW Legends hardcore + Mak'gora commands (player-level) so they
-- show in `.help`, the docs and the website command page.
DELETE FROM `command` WHERE `name` IN
    ('hardcore','hardcore on','hardcore status','makgora');

-- Note: no bare `hardcore` parent row on purpose (it would be a redundant entry
-- on the website; the subcommands carry their own help).
INSERT INTO `command` (`name`,`security`,`help`) VALUES
('hardcore on',     0, 'Syntax: .hardcore on\nEnable hardcore mode on this character. LEVEL 1 ONLY and permanent - if you die, the character becomes a fallen hero and can no longer be played.'),
('hardcore status', 0, 'Syntax: .hardcore status\nShow whether this character is normal, hardcore (alive) or fallen.'),
('makgora',         0, 'Syntax: .makgora\nWith another hardcore player targeted, challenge them to Mak''gora - a duel to the death. Both must accept; the next duel within 30s is lethal for the loser.');

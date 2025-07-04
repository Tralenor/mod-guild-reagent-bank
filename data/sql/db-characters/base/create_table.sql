DROP TABLE `custom_guild_reagent_bank`;
CREATE TABLE IF NOT EXISTS `custom_guild_reagent_bank` (
    `guild_id` int(11) NOT NULL,
    `item_entry` int(11) NOT NULL,
    `item_subclass` int(11) NOT NULL,
    `amount` int(11) NOT NULL,
    PRIMARY KEY (`guild_id`,`item_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

DROP TABLE `custom_guild_reagent_bank_size`;
CREATE TABLE `custom_guild_reagent_bank_size` (
    `guild_id` int NOT NULL,
    `space` int NOT NULL DEFAULT '0',
    PRIMARY KEY (`guild_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
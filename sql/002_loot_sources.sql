ALTER TABLE transaction_journal
 ADD COLUMN source_type VARCHAR(20) NULL,
 ADD COLUMN source_entry INT UNSIGNED NULL,
 ADD COLUMN source_guid VARCHAR(128) NULL,
 ADD COLUMN source_name VARCHAR(255) NULL,
 ADD COLUMN map_id INT UNSIGNED NULL,
 ADD COLUMN map_name VARCHAR(255) NULL,
 ADD COLUMN zone_id INT UNSIGNED NULL,
 ADD COLUMN zone_name VARCHAR(255) NULL,
 ADD COLUMN area_id INT UNSIGNED NULL,
 ADD COLUMN area_name VARCHAR(255) NULL,
 ADD KEY loot_source_item (source_entry,item_id,event_kind);

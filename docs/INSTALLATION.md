# Transaction Journal
Module-only recorder for AzerothCore. Apply sql/001_transaction_journal.sql to the candidate character database and select monitored characters in the Hub. AHBot GUIDs classify counterparties without recording the whole bot market. Existing auctions loaded before startup are not reported as new listings.

Auction rows are pre-commit hook observations, not proof of committed settlement or mail collection. The shared successful-mail hook captures AHBot Plus buyouts. Removal is not classified as cancellation. Post-commit guarantees are outside this version. Async inserts are best effort and may be lost on crashes or database errors. Unique listing/event keys suppress replay, and UUIDs support idempotent imports. No core changes.

## Vendor events and monitoring
The Hub writes an atomic newline-separated GUID file. Configure TransactionJournal.MonitoringFile to its in-container path; the candidate uses the existing /logs mount. The Hub process uses JOURNEY_HUB_TRANSACTION_MONITORING for the corresponding host path. The default file is empty and no characters are implicitly monitored. WorldScript reloads it every five seconds. History remains append-only when monitoring is disabled.

vendor_purchase uses the existing post-store hook and the before-request money balance to capture actual purchased units and copper spent. vendor_sale_observed compares buyback item identities at the sell-permission hook with buyback contents at the subsequent positive money hook. This captures partial stacks and durability-adjusted proceeds without guessing from template price. Sale amounts are observed before final money-hook processing. Failed attempts leave no event unless a new buyback item and positive money change occur. Token costs, refunds and vendor buyback purchases are outside this version.

## Explicit history removal
The Hub snapshots a selected character's matching event count and current maximum journal ID before confirmation. Confirmed requests are written beside the monitoring file, under transaction-journal-requests/. The module asynchronously deletes only journal rows at or below that cutoff where the character is actor or counterparty. Shared transaction rows disappear for all parties. Recording settings do not change. Completion is acknowledged after transaction commit, with interrupted requests replayed idempotently at startup. No character/inventory/economy tables are modified.

## Loot money and structured sources
Apply sql/002_loot_sources.sql before installing this module version. New item and money loot events snapshot source_type, source_entry, source_guid, source_name, map_id/map_name, zone_id/zone_name and area_id/area_name. Unknown/unavailable sources remain explicit, and historical rows retain NULL fields. World objects use their own location; item containers have item identity without an invented map or zone.

Money capture snapshots tracked nearby group members' balances in OnPlayerBeforeLootMoney and compares them at the final LootScript::OnLootMoney callback in the same handler. It records positive actual balance increases for each tracked recipient rather than treating the corpse's whole gold amount as every member's income. This includes actual sharing and money caps; unrelated money grants from other scripts inside that same handler can be included. No core changes or kill/drop-rate inference.

## Installation

1. Place this repository at modules/mod-transaction-journal in an AzerothCore source tree.
2. Apply sql/001_transaction_journal.sql then sql/002_loot_sources.sql and sql/003_mail_sends.sql to the character database. Apply each migration once.
3. Build worldserver with static modules using your existing build process.
4. Copy conf/transaction_journal.conf.dist to the worldserver module configuration directory as transaction_journal.conf.
5. Set TransactionJournal.Enable to 1. Choose a monitoring file in a directory accessible to worldserver and the Hub. Set AHBotGUIDs to your own auction-bot character GUIDs when applicable.
6. Configure the Hub's JOURNEY_HUB_TRANSACTION_MONITORING environment variable or its local transaction-journal.json monitoring_file setting with the corresponding path.
7. Restart worldserver and use Transactions → Character monitoring in Journey Hub to opt in characters.

The Hub and worldserver may see different paths to the same shared directory in container installations. Give both processes appropriate access to the monitoring file and transaction-journal-requests directory. Keep the directory private to trusted local administrators and the Hub process.

Tested against the Playerbots AzerothCore fork at core commit 93333d6. Hook signatures can differ on other versions. The SQL tables are additive and intentionally separate from gameplay state.

# Package Registry Mirror Sync

Owns primary-to-mirror synchronization and mirror freshness state. This layer
reads `_distributions/default/current.json`, copies the referenced immutable
publication, verifies the snapshot, records replay cursors, and keeps the read
plane available when write-side control-plane work is isolated.

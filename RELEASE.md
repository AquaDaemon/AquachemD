# AquachemD Release Notes & Changelog

All notable changes to AquachemD are documented here. Releases are listed in reverse chronological order (newest first).
-------------------------------------
## Release 1.1.0
- WARNING EC Child sensors in HA will get a different ID. Any reporting on TDS / salinity & SC values will need to be modified, Conductivity will stay with the same ID.
- Added sensor conditions, Set conditions on sensor values (<,>,=,<=,>=)
- Fixed bug with EC temperature correction readings
- Normalized many config values
- Fixed Log and Config downloads.
- Fix deadlock when only 1 condition `sensor_condition` is present.
- Added setting to select which sensor ID to use for compensated temperature sensor readings
- Added running_dose_ml MQTT updates when it's been reset. 
- Added labels for child sensors.
- Changed ID's for child sensors.
- Can set i2c bus in config.
- Added Release notes to UI. (update dialogue)

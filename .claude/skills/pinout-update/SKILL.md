# Pinout Update
When reassigning GPIO pins:
1. Read current pin table in CLAUDE.md
2. Check for LoRa/SPI trace-crossing conflicts
3. Update main.cpp constants for ALL build variants (S3 and non-S3)
4. Update CLAUDE.md pinout table including relay row
5. Summarize changes and ask for confirmation before flashing

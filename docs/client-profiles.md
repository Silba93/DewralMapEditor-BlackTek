# Startup and client profiles

The Tibia UI welcome window separates map actions and client configuration. Recent maps can be filtered by name or path, opened with Enter/double click, and removed from the list without deleting the map.

Use **Manage clients** on the welcome screen or **Map > Client profile > Manage clients** in the editor. Profiles have a stable ID independent of their display name. Existing numeric/custom profiles and map assignments migrate on first load; legacy custom resource directory names are retained separately.

Blank file fields use automatic detection. Explicit paths can point to different directories. Standard classic profiles require DAT, SPR, and OTB. BlackTek profiles require DAT, SPR, and `items.toml`. Optional XML and OTFI can be disabled where offered. Multiple candidates require an explicit choice. OTFI paths/flags and manual format overrides use the same resolver during validation and loading.

**Validate files** parses a temporary set of readers asynchronously. It never publishes data into active models. Save persists the draft; changing or deleting a saved profile does not replace an open document's configuration snapshot. Missing/deleted map profiles prompt for a replacement.

Verification: `client_load_service_test`, QML `tst_client_profile.qml`, `tst_startup_profiles.qml`, `tst_startup_window.qml`, and native `--smoke-test --smoke-documents` with real 7.72 and 10.98 assets. Native diagnostics `--smoke-test --smoke-client-profiles --document-client <folder>` capture both windows using isolated settings.

## BlackTek zones

Open the map with a profile whose item source is **BlackTek items.toml**. The editor looks for `data/world/<map_name>-zones/` in a matching BlackTek server project. For a standalone map it looks for a sibling folder named after the OTBM file with `-zones` appended. If the folder is missing, choose **Locate zones folder** to link any folder that contains the world’s zone TOML. The editor remembers the link for the map. There is no legacy spawn XML fallback.

Open **Tools → BlackTek zones** to search zones, jump to their locations, and edit their source. Use **Apply TOML changes** to add the draft to the document’s undo history, then save the map to write it to disk. **Save As** creates an independent zone folder for the new map. To use that copy on the server, configure its world name to match the new map.

The zone editor blocks changes that the server loader would reject, such as a zone with no valid geometry or an NPC entry with no name. Warnings identify values that the server accepts but ignores, replaces with defaults, or wraps. Unknown fields stay in the source so other server versions can still use them. Creature names are not checked against the running server’s Lua registrations.

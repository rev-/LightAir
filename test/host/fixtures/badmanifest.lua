-- Manifest-rejection fixture: a well-formed table from a future (or
-- broken) API version.  peekManifest must refuse it rather than
-- register a game the binding cannot drive.
return {
  api = 999, type_id = 0x7F03, name = "FromTheFuture",
}

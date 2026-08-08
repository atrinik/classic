# Recorded session fixtures

`recorded_stream.txt` is a deterministic, redacted semantic recording. It
exercises the renderer-independent reducers without importing a wire-protocol
parser into the session library. Each line names one accepted reducer input;
tests replay the stream and verify the resulting player, map, inventory,
dialog, quest, message, and party state.

Raw packet recording and decoding belongs at the future protocol/API boundary.
Keep this fixture free of account names, server addresses, chat, or other live
player data.

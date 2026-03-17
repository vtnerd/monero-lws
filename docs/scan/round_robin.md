# Round-Robin
This is the default scan algorithm used by `monero-lws-daemon`. Each account
is sorted by height, and then distributed evenly across threads. Older accounts
will typically end up on the first thread, however, some accounts are stuck
"waiting" for the old accounts to "catch-up".
